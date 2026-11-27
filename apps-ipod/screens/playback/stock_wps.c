/* Fruit Company Now Playing controls. GPL version 2+. */
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "string-extra.h"
#include "audio.h"
#include "metadata.h"
#include "database/tagcache.h"
#include "input/action.h"
#include "playlist/playlist.h"
#include "screens/browse/browser_db.h"
#include "settings/settings.h"
#include "cider.h"
#include "stock_wps.h"
#include "wps.h"

/* Each bottom control settles back after four idle seconds. */
#define CONTROL_HOLD (4 * HZ)
#define MAX_STARS 5

static long rating_index(const struct mp3entry *track)
{
    long index = browser_db_track_idx(track);
    return index >= 0 ? index : tagcache_find_path(track->path);
}

static int current_stars(const struct mp3entry *track)
{
    long index = rating_index(track);
    int rating = index < 0 ? track->rating : browser_db_get_rating(index);
    return MIN(MAX_STARS, MAX(0, (rating + 1) / 2));
}

static void finish_edit(struct stock_wps *stock)
{
    if (!stock->edited)
        return;
    stock->edited = false;

    const struct stock_wps_controls *controls = &stock->controls;
    if (controls->mode == STOCK_WPS_SCRUB)
    {
        if (controls->remote)
            cider_control("seek", controls->position);
        else
        {
            audio_pre_ff_rewind();
            audio_ff_rewind(controls->position);
        }
    }
    else if (controls->mode == STOCK_WPS_SHUFFLE)
    {
        if (controls->remote)
            cider_mode_set(CIDER_MODE_SHUFFLE, controls->shuffle);
        else if (playlist_set_shuffle_mode(controls->shuffle))
            settings_save();
    }
}

void stock_wps_update(struct stock_wps *stock, const struct wps_state *state,
                      unsigned track_serial)
{
    struct stock_wps_controls *controls = &stock->controls;
    const struct mp3entry *track = state->id3;
    bool remote = cider_active();
    if (!track || controls->remote != remote)
    {
        controls->mode = STOCK_WPS_PROGRESS;
        stock->edited = false;
    }
    controls->remote = remote;
    if (!track)
        return;

    if (stock->track_serial != track_serial || strcmp(stock->path, track->path))
    {
        stock->track_serial = track_serial;
        strlcpy(stock->path, track->path, sizeof(stock->path));
        if (controls->mode != STOCK_WPS_SHUFFLE)
            stock->edited = false;
        if (controls->mode == STOCK_WPS_RATING)
            controls->stars = current_stars(track);
    }
    if (controls->mode != STOCK_WPS_PROGRESS &&
        TIME_AFTER(current_tick, stock->deadline - 1))
    {
        finish_edit(stock);
        controls->mode = STOCK_WPS_PROGRESS;
    }
    if (controls->mode == STOCK_WPS_SCRUB)
    {
        controls->length = track->length;
        if (!stock->edited)
            controls->position = track->elapsed;
        controls->position = MIN(controls->position, track->length);
    }
    if (controls->mode != STOCK_WPS_SHUFFLE || !stock->edited)
        controls->shuffle = remote ? cider_mode_get(CIDER_MODE_SHUFFLE) :
                            playlist_get_current()->shuffle_mode;
}

static void wheel_edit(struct stock_wps *stock, const struct mp3entry *track,
                       int detents)
{
    struct stock_wps_controls *controls = &stock->controls;
    switch (controls->mode)
    {
        case STOCK_WPS_SCRUB:
        {
            unsigned long length = controls->length;
            unsigned long step = MAX(250UL, length / 100);
            unsigned long end = length ? length - 1 : 0;
            unsigned long old = controls->position;
            int64_t position = (int64_t)old + (int64_t)step * detents;
            controls->position = MIN((uint64_t)MAX(0, position), end);
            stock->edited |= controls->position != old;
            break;
        }
        case STOCK_WPS_RATING:
        {
            int next = MIN(MAX_STARS, MAX(0, controls->stars + detents));
            if (next == controls->stars || !tagcache_is_usable())
                break;
            uint32_t generation = tagcache_generation();
            long index = rating_index(track);
            if (index >= 0 && tagcache_generation() == generation &&
                tagcache_is_usable())
            {
                browser_db_set_rating(index, next * 2);
                controls->stars = next;
                stock->edited = true;
            }
            break;
        }
        case STOCK_WPS_SHUFFLE:
        {
            int last = controls->remote ? SHUFFLE_SONGS : SHUFFLE_ALBUMS;
            int next = MIN(last, MAX(SHUFFLE_OFF, controls->shuffle + detents));
            if (next == controls->shuffle)
                break;
            controls->shuffle = next;
            stock->edited = true;
            break;
        }
        default:
            break;
    }
}

bool stock_wps_action(struct stock_wps *stock, const struct wps_state *state,
                      long action)
{
    struct stock_wps_controls *controls = &stock->controls;
    const struct mp3entry *track = state->id3;
    if (!track)
        return action == ACTION_WPS_BROWSE;
    switch (action)
    {
        case ACTION_WPS_BROWSE:
            stock->deadline = current_tick + CONTROL_HOLD;
            if (stock->edited)
            {
                finish_edit(stock);
                controls->mode = STOCK_WPS_PROGRESS;
            }
            else
                switch (controls->mode)
                {
                    case STOCK_WPS_PROGRESS:
                        controls->mode = STOCK_WPS_SCRUB;
                        controls->length = track->length;
                        controls->position = MIN(track->elapsed, track->length);
                        break;
                    case STOCK_WPS_SCRUB:
                        /* The remote protocol has no per-song star rating. */
                        controls->mode = controls->remote ? STOCK_WPS_SHUFFLE :
                                         STOCK_WPS_RATING;
                        if (!controls->remote)
                            controls->stars = current_stars(track);
                        break;
                    case STOCK_WPS_RATING:
                        controls->mode = STOCK_WPS_SHUFFLE;
                        break;
                    default:
                        controls->mode = STOCK_WPS_PROGRESS;
                        break;
                }
            return true;

        case ACTION_WPS_VOLUP:
        case ACTION_WPS_VOLDOWN:
        {
            stock->deadline = current_tick + CONTROL_HOLD;
            if (controls->mode == STOCK_WPS_PROGRESS ||
                controls->mode == STOCK_WPS_VOLUME)
            {
                controls->mode = STOCK_WPS_VOLUME;
                return false;
            }
            int detents = MAX(1, (get_action_data() >> 24) & 0x7f);
            wheel_edit(stock, track,
                       action == ACTION_WPS_VOLUP ? detents : -detents);
            return true;
        }

        case ACTION_WPS_SKIPPREV:
        case ACTION_WPS_SKIPNEXT:
        case ACTION_WPS_SEEKFWD:
        case ACTION_WPS_SEEKBACK:
            if (controls->mode == STOCK_WPS_SCRUB)
                stock->edited = false;
            break;
        case ACTION_WPS_PLAY:
            if (controls->mode == STOCK_WPS_SCRUB)
                finish_edit(stock);
            break;
        case ACTION_WPS_MENU:
            finish_edit(stock);
            break;
        default:
            break;
    }
    return false;
}
