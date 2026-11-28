/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The Videos categories and iTunes' video playlists. The iTunes library's
 * videos keep the category iTunes gave them; a video file copied to the
 * player outside it joins the category its MP4 media kind names, by the
 * video index (files/video_index.c), and a file with none is a movie. Both
 * play in the same viewer. In these lists Previous and Next do nothing and
 * Play/Pause plays the selected video: none of the three controls the music.
 ****************************************************************************/
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "config.h"
#include "core_alloc.h"
#include "draw/bmp.h"
#include "kernel.h"
#include "lcd.h"
#include "lang.h"
#include "widgets/splash.h"
#include "root_menu.h"
#include "settings/settings.h"
#include "speech/talk.h"
#include "files/filetypes.h"             /* HAVE_VIDEO_PLAYER */
#include "files/video_index.h"
#include "metadata/video_art.h"
#include "system/library_files.h"
#include "plugins/rock_compat.h"
#include "input/action.h"
#include "ui/scene.h"
#include "ui/stock_art.h"
#include "widgets/list.h"
#include "widgets/menu.h"
#include "stock_media.h"

#define VIDEO_MASK (STOCK_MOVIE | STOCK_TV_SHOW | STOCK_MUSIC_VIDEO)
#define MEDIA_LIMIT 32768
#define MAX_PLAYLISTS 2048
#define COVER_SIZE VIDEO_ART_DIM
#define COVER_SOURCE 128
/* Covers kept drawn: a screen of rows and the ones scrolled in next */
#define COVER_SLOTS 8
/* A list of videos' rows, as stock's: with a cover, or a TV episode's
 * shorter one with none */
#define COVER_ROW 55
#define EPISODE_ROW 44
/* iTunes' times count from 1904 */
#define MAC_EPOCH 2082844800u

struct video_track {
    uint32_t id;
    uint32_t media;
    off_t title;
    off_t path;
    off_t artist;
    off_t show;
    off_t artwork;
    uint32_t artwork_id;
    uint64_t dbid;
    uint32_t season;
    uint32_t episode;
    uint32_t duration_ms;
    uint32_t released;          /* VIDEO_DATE(), 0 for none */
    uint64_t art_key;           /* a loose video's cover thumbnail */
    bool loose;                 /* strings are in loose_text, not the db */
};

/* How a list of videos draws its rows: with covers, as one season's
 * episodes with their release dates, or as a playlist's, bare */
enum track_rows { ROWS_COVERED, ROWS_EPISODES, ROWS_PLAYLIST };

struct cover_slot {
    int track;                  /* -1 for none */
    bool placeholder;           /* drawn while the cover is not there */
};

struct videos {
    struct stock_db db;
    struct stock_db art;
    int cover_handle;
    fb_data *cover;             /* an iTunes thumbnail as read, then slots */
    struct cover_slot slots[COVER_SLOTS];
    int next_slot;
    int slot_edge;
    unsigned art_generation;
    struct bitmap bitmap;
    struct video_track *tracks;
    int count;
    int *rows;
    int row_count;
    int row_capacity;
    off_t *playlists;
    int *playlist_sizes;        /* each playlist's videos */
    int playlist_count;
    char *loose_text;
    size_t loose_used, loose_size;
    int capacity;
    enum track_rows style;
};

static bool text_at(const struct videos *v, off_t at, char *buf, size_t n)
{
    struct stock_record r;
    return at && stock_record_read(&v->db, at, v->db.end, "mhod", &r) &&
           stock_db_music_text(&v->db, &r, buf, n);
}

/* A track's string at `at`, its title, path, artist or show */
static bool track_text(const struct videos *v, const struct video_track *t,
                       off_t at, char *buf, size_t n)
{
    if (!t->loose)
        return text_at(v, at, buf, n);
    if (!at)
        return false;
    strlcpy(buf, v->loose_text + at - 1, n);
    return true;
}

static int compare_id(const void *left, const void *right)
{
    uint32_t a = ((const struct video_track *)left)->id;
    uint32_t b = ((const struct video_track *)right)->id;
    return (a > b) - (a < b);
}

static bool read_dbid(const struct stock_db *db, off_t at, uint64_t *id)
{
    uint32_t low, high;
    if (!stock_db_u32(db, at, &low) || !stock_db_u32(db, at + 4, &high))
        return false;
    *id = ((uint64_t)high << 32) | low;
    return true;
}

static int compare_art_id(const void *left, const void *right)
{
    const struct video_track *a = left;
    const struct video_track *b = right;
    uint64_t id_a = a->artwork_id ? a->artwork_id : a->dbid;
    uint64_t id_b = b->artwork_id ? b->artwork_id : b->dbid;
    return (id_a > id_b) - (id_a < id_b);
}

static void read_artwork(struct videos *v)
{
    struct stock_record list, image;
    if (!v->count || !stock_artwork_open(&v->art) ||
        !stock_db_list(&v->art, 1, "mhli", &list))
        return;

    qsort(v->tracks, v->count, sizeof(*v->tracks), compare_art_id);
    for (off_t at = list.body; at < list.end; at = image.end)
    {
        struct video_track key = {0};
        if (!stock_record_read(&v->art, at, list.end, "mhii", &image))
            break;
        if (image.body - at < 28 ||
            !stock_db_u32(&v->art, at + 16, &key.artwork_id) ||
            !read_dbid(&v->art, at + 20, &key.dbid))
            continue;
        for (int i = 0; i < 2; i++)
        {
            int low = 0, high = v->count;
            while (low < high)
            {
                int mid = low + (high - low) / 2;
                if (compare_art_id(&v->tracks[mid], &key) < 0)
                    low = mid + 1;
                else
                    high = mid;
            }
            while (low < v->count && !compare_art_id(&v->tracks[low], &key))
                v->tracks[low++].artwork = at;
            key.artwork_id = 0;
        }
        yield();
    }
}

static int by_id(const struct videos *v, uint32_t id)
{
    int low = 0, high = v->count;
    while (low < high)
    {
        int mid = low + (high - low) / 2;
        if (id == v->tracks[mid].id)
            return mid;
        if (id < v->tracks[mid].id)
            high = mid;
        else
            low = mid + 1;
    }
    return -1;
}

static void read_tracks(struct videos *v, const struct stock_record *list,
                        unsigned capacity)
{
    struct stock_record track, child;
    for (off_t at = list->body; at < list->end && (unsigned)v->count < capacity;
         at = track.end)
    {
        struct video_track item = {0};
        if (!stock_record_read(&v->db, at, list->end, "mhit", &track))
            break;
        if (track.body - at < 212 || !stock_db_u32(&v->db, at + 208, &item.media) ||
            !(item.media & VIDEO_MASK) || (item.media & (4 | 0x8000)) ||
            !stock_db_u32(&v->db, at + 16, &item.id))
            continue;
        read_dbid(&v->db, at + 112, &item.dbid);
        stock_db_u32(&v->db, at + 40, &item.duration_ms);
        uint32_t released;
        if (track.body - at >= 144 && stock_db_u32(&v->db, at + 140, &released)
            && released >= MAC_EPOCH)
        {
            time_t when = released - MAC_EPOCH;
            struct tm tm;
            gmtime_r(&when, &tm);
            item.released = VIDEO_DATE(tm.tm_year + 1900, tm.tm_mon + 1,
                                       tm.tm_mday);
        }
        if (track.body - at >= 220)
        {
            stock_db_u32(&v->db, at + 212, &item.season);
            stock_db_u32(&v->db, at + 216, &item.episode);
        }
        if (track.body - at >= 356)
            stock_db_u32(&v->db, at + 352, &item.artwork_id);
        for (off_t p = track.body; p < track.end; p = child.end)
        {
            uint32_t type;
            if (!stock_record_read(&v->db, p, track.end, "mhod", &child) ||
                child.body - p < 16 || !stock_db_u32(&v->db, p + 12, &type))
                break;
            if (type == 1)
                item.title = p;
            else if (type == 2)
                item.path = p;
            else if (type == 4)
                item.artist = p;
            else if (type == 19)
                item.show = p;
        }
        char path[MAX_PATH];
        if (text_at(v, item.path, path, sizeof(path)) && stock_video_path(path))
            v->tracks[v->count++] = item;
        yield();
    }
}

/* The playlist's videos into `rows`, at most `limit`; with no `rows`, only
 * counted */
static int playlist_members(struct videos *v, const struct stock_record *playlist,
                            int *rows, int limit)
{
    struct stock_record r;
    uint32_t metadata, count;
    if (playlist->body - playlist->at < 21 ||
        !stock_db_u32(&v->db, playlist->at + 12, &metadata) ||
        !stock_db_u32(&v->db, playlist->at + 16, &count))
        return 0;

    off_t p = playlist->body;
    while (metadata--)
    {
        if (!stock_record_read(&v->db, p, playlist->end, "mhod", &r))
            return 0;
        p = r.end;
    }
    int shown = 0;
    while (count-- && shown < limit)
    {
        uint32_t id;
        if (!stock_record_read(&v->db, p, playlist->end, "mhip", &r) ||
            r.body - p < 28 || !stock_db_u32(&v->db, p + 24, &id))
            break;
        int row = by_id(v, id);
        if (row >= 0)
        {
            if (rows)
                rows[shown] = row;
            shown++;
        }
        p = r.end;
        if (r.body == r.end)
        {
            /* Older writers leave the member's MHODs outside its length. */
            while (stock_record_read(&v->db, p, playlist->end, "mhod", &r))
                p = r.end;
        }
    }
    return shown;
}

/* The playlists holding a video, and with `sizes` how many each holds */
static void read_playlists(struct videos *v, bool sizes)
{
    struct stock_record list, playlist;
    if (!stock_db_list(&v->db, 3, "mhlp", &list) &&
        !stock_db_list(&v->db, 2, "mhlp", &list))
        return;
    for (off_t at = list.body; at < list.end && v->playlist_count < MAX_PLAYLISTS;
         at = playlist.end)
    {
        uint32_t flags;
        if (!stock_record_read(&v->db, at, list.end, "mhyp", &playlist) ||
            playlist.body - at < 24 || !stock_db_u32(&v->db, at + 20, &flags))
            break;
        int held;
        if (flags & 0xff ||
            !(held = playlist_members(v, &playlist, NULL,
                                      sizes ? MEDIA_LIMIT : 1)))
            continue;
        if (sizes)
            v->playlist_sizes[v->playlist_count] = held;
        v->playlists[v->playlist_count++] = at;
    }
}

static const char *video_title(int row, void *data, char *buf, size_t len)
{
    const struct videos *v = data;
    const struct video_track *item = &v->tracks[v->rows[row]];
    if (track_text(v, item, item->title, buf, len))
        return buf;
    if (track_text(v, item, item->path, buf, len))
    {
        const char *leaf = strrchr(buf, ':');
        return leaf ? leaf + 1 : buf;
    }
    return "";
}

static const char *playlist_title(int row, void *data, char *buf, size_t len)
{
    const struct videos *v = data;
    struct stock_record r;
    return stock_record_read(&v->db, v->playlists[row], v->db.end, "mhyp", &r) &&
           text_at(v, r.body, buf, len) ? buf : "Video playlist";
}

/* A length as stock writes it under a movie's title */
static const char *format_length(uint32_t ms, char *buf, size_t size)
{
    unsigned seconds = ms / 1000;
    if (seconds < 60)
        snprintf(buf, size, str(LANG_VIDEO_SECONDS), seconds);
    else if (seconds < 3600)
        snprintf(buf, size, "%u:%02u", seconds / 60, seconds % 60);
    else
        snprintf(buf, size, "%u:%02u:%02u", seconds / 3600,
                 seconds / 60 % 60, seconds % 60);
    return buf;
}

/* A release date as stock writes it, "March 8, 2026", or as much of it
 * as is known */
static const char *format_date(uint32_t date, char *buf, size_t size)
{
    unsigned year = VIDEO_DATE_YEAR(date), month = VIDEO_DATE_MONTH(date);
    unsigned day = VIDEO_DATE_DAY(date);
    if (!date)
        return str(LANG_VIDEO_UNKNOWN_RELEASE);
    if (!month)
        snprintf(buf, size, "%u", year);
    else if (!day)
        snprintf(buf, size, "%s %u",
                 str(LANG_EXTRAS_CALENDARS_JANUARY + month - 1), year);
    else
        snprintf(buf, size, "%s %u, %u",
                 str(LANG_EXTRAS_CALENDARS_JANUARY + month - 1), day, year);
    return buf;
}

/* The line under a video's title: in a season, the episode's release
 * date; else a music video's artist, an episode's show, or a film's
 * length */
static const char *video_value(int row, void *data, char *buf, size_t size)
{
    const struct videos *v = data;
    if (row >= v->row_count)
        return NULL;
    const struct video_track *t = &v->tracks[v->rows[row]];
    if (v->style == ROWS_EPISODES)
        return format_date(t->released, buf, size);
    if (t->media & STOCK_MUSIC_VIDEO)
        return track_text(v, t, t->artist, buf, size) ? buf :
               (const char *)str(LANG_VIDEO_UNKNOWN_ARTIST);
    if (t->media & STOCK_TV_SHOW)
        return track_text(v, t, t->show, buf, size) ? buf :
               (const char *)str(LANG_VIDEO_UNKNOWN_SHOW);
    return t->duration_ms ? format_length(t->duration_ms, buf, size) : NULL;
}

static enum list_row_kind video_kind(int row, void *data)
{
    (void)row;
    (void)data;
    return LIST_ROW_VIDEO;
}

/* The stock picture a video of `media` shows while it has no cover */
static int placeholder_of(uint32_t media)
{
    return (media & STOCK_TV_SHOW) ? STOCK_ART_MEDIA_TVSHOWSPROXY :
           (media & STOCK_MUSIC_VIDEO) ? STOCK_ART_MEDIA_MUSICVIDEOSPROXY :
           STOCK_ART_MEDIA_VIDEOSPROXY;
}

/* The stock placeholder, scaled to `edge` */
static void paint_placeholder(int id, fb_data *px, int edge)
{
    const struct stock_art *art = &stock_art[id];
    fb_data line[COVER_SIZE];
    for (int y = 0; y < edge; y++)
    {
        int sy = y * art->height / edge;
        for (int x = 0; x < edge; x++)
            line[x] = LCD_WHITE;
        if (art->width == edge)
            stock_art_row(id, 0, sy, line, edge);
        else
        {
            fb_data source[COVER_SOURCE];
            int width = MIN(art->width, COVER_SOURCE);
            for (int x = 0; x < width; x++)
                source[x] = LCD_WHITE;
            stock_art_row(id, 0, sy, source, width);
            for (int x = 0; x < edge; x++)
                line[x] = source[x * width / edge];
        }
        memcpy(px + y * edge, line, edge * sizeof(fb_data));
    }
}

/* The cover of `track`, or the placeholder, into `px` at `edge`. True for
 * the cover. */
static bool load_cover(struct videos *v, const struct video_track *track,
                       fb_data *px, int edge)
{
    if (track->loose)
    {
        if (video_art_load(track->art_key, edge, px, LCD_BLACK))
            return true;
    }
    else if (track->artwork)
    {
        int source = stock_artwork_pixels(&v->art, track->artwork, v->cover);
        if (source)
        {
            for (int y = 0; y < edge; y++)
                for (int x = 0; x < edge; x++)
                    px[y * edge + x] = v->cover[(y * source / edge) * source +
                                                x * source / edge];
            return true;
        }
    }
    paint_placeholder(placeholder_of(track->media), px, edge);
    return false;
}

/* The cover of track `item`, from the slots */
static const struct bitmap *cover_of(struct videos *v, int item,
                                     struct dim *size)
{
    if (!v->cover)
        return NULL;
    int edge = MIN(COVER_SIZE, MIN(size->width, size->height));
    if (edge <= 0)
        return NULL;
    if (edge != v->slot_edge)
    {
        for (int i = 0; i < COVER_SLOTS; i++)
            v->slots[i].track = -1;
        v->slot_edge = edge;
    }
    int slot;
    for (slot = 0; slot < COVER_SLOTS; slot++)
        if (v->slots[slot].track == item)
            break;
    fb_data *slots = v->cover + COVER_SOURCE * COVER_SOURCE;
    if (slot == COVER_SLOTS)
    {
        slot = v->next_slot;
        v->next_slot = (slot + 1) % COVER_SLOTS;
        v->slots[slot].track = item;
        v->slots[slot].placeholder =
            !load_cover(v, &v->tracks[item], slots + slot * COVER_SIZE * COVER_SIZE,
                        edge);
    }
    v->bitmap = (struct bitmap){
        .width = edge, .height = edge, .format = FORMAT_NATIVE,
        .data = (void *)(slots + slot * COVER_SIZE * COVER_SIZE),
    };
    size->width = size->height = edge;
    return &v->bitmap;
}

static const struct bitmap *video_cover(int row, void *data, struct dim *size)
{
    struct videos *v = data;
    return row < v->row_count ? cover_of(v, v->rows[row], size) : NULL;
}

/* A loose video's cover made since its row was drawn is drawn again */
static int fresh_covers(struct videos *v, int action)
{
    unsigned generation = video_art_generation();
    if (generation != v->art_generation)
    {
        v->art_generation = generation;
        bool stale = false;
        for (int i = 0; i < COVER_SLOTS; i++)
            if (v->slots[i].track >= 0 && v->slots[i].placeholder &&
                v->tracks[v->slots[i].track].loose)
            {
                v->slots[i].track = -1;
                stale = true;
            }
        if (stale && action == ACTION_NONE)
            action = ACTION_REDRAW;
    }
    return action;
}

/* PLAY plays the selected video */
static int videos_list_action(int action, struct gui_synclist *lists)
{
    if (action == ACTION_TREE_WPS)
        return ACTION_STD_OK;
    return stock_media_list_action(fresh_covers(lists->data, action), lists);
}

static int show_tracks(struct videos *v, char *title)
{
    if (!v->row_count)
        return 0;
    struct simplelist_info info;
    simplelist_info_init(&info, title, v->row_count, v);
    info.get_name = video_title;
    info.get_value = video_value;
    info.get_kind = video_kind;
    info.action_callback = videos_list_action;
    info.get_albumart = v->style == ROWS_COVERED ? video_cover : NULL;
    info.video_rows = true;
    info.row_height = v->style == ROWS_COVERED ? COVER_ROW : EPISODE_ROW;
    info.selection = 0;
    while (true)
    {
        if (simplelist_show_list(&info))
            return MENU_ATTACHED_USB;
        int sel = info.selection;
        if (sel < 0)
            return 0;
#ifdef HAVE_VIDEO_PLAYER
        while (sel >= 0 && sel < v->row_count)
        {
            char path[MAX_PATH], label[MAX_PATH];
            const struct video_track *track = &v->tracks[v->rows[sel]];
            if (!track_text(v, track, track->path, path, sizeof(path)) ||
                (!track->loose && !stock_video_path(path)) ||
                !file_exists(path))
            {
                splash(HZ, ID2P(LANG_FILE_NOT_FOUND));
                break;
            }
            const char *name = track_text(v, track, track->title, label,
                                          sizeof(label)) ? label : NULL;
            info.selection = sel;
            int result = rock_open_video(path, name, sel > 0);
            if (result == ROCK_VIDEO_USB)
                return MENU_ATTACHED_USB;
            if (result == ROCK_VIDEO_PREVIOUS)
                sel--;
            else if (result == ROCK_VIDEO_FINISHED || result == ROCK_VIDEO_NEXT)
                sel++;
            else
                break;
        }
#endif
    }
}

static enum list_row_kind video_folder_kind(int row, void *data)
{
    (void)row;
    (void)data;
    return LIST_ROW_MENU;
}

/* "N Videos" */
static const char *count_value(int count, char *buf, size_t size)
{
    if (count == 1)
        return str(LANG_VIDEO_ONE_VIDEO);
    snprintf(buf, size, str(LANG_VIDEO_COUNT), count);
    return buf;
}

static const char *playlist_value(int row, void *data, char *buf, size_t size)
{
    const struct videos *v = data;
    return count_value(v->playlist_sizes[row], buf, size);
}

static int show_playlists(struct videos *v, char *title)
{
    struct simplelist_info info;
    simplelist_info_init(&info, title, v->playlist_count, v);
    info.get_name = playlist_title;
    info.get_value = playlist_value;
    info.get_kind = video_folder_kind;
    info.action_callback = stock_media_list_action;
    info.video_rows = true;
    info.row_height = EPISODE_ROW;
    while (true)
    {
        if (simplelist_show_list(&info))
            return MENU_ATTACHED_USB;
        if (info.selection < 0)
            return 0;

        struct stock_record playlist;
        char name[96];
        int row = info.selection;
        if (!stock_record_read(&v->db, v->playlists[row], v->db.end, "mhyp", &playlist))
            return 0;
        uint32_t members;
        if (!stock_db_u32(&v->db, playlist.at + 16, &members))
            return 0;
        int row_handle = -1;
        int *previous_rows = v->rows;
        int previous_capacity = v->row_capacity;
        if (members > (unsigned)v->row_capacity)
        {
            unsigned capacity = MIN(members, MEDIA_LIMIT);
            row_handle = core_alloc(capacity * sizeof(*v->rows));
            if (row_handle > 0)
            {
                v->rows = core_get_data_pinned(row_handle);
                v->row_capacity = capacity;
            }
        }
        v->row_count = playlist_members(v, &playlist, v->rows, v->row_capacity);
        const char *label = playlist_title(row, v, name, sizeof(name));
        if (label != name)
            snprintf(name, sizeof(name), "%s", label);
        v->style = ROWS_PLAYLIST;
        int rc = show_tracks(v, name);
        v->style = ROWS_COVERED;
        if (row_handle > 0)
        {
            v->rows = previous_rows;
            v->row_capacity = previous_capacity;
            core_unpin(row_handle);
            core_free(row_handle);
        }
        if (rc)
            return rc;
    }
}

struct video_group_entry {
    char name[96];
    int track;
    uint32_t season;
    uint32_t episode;
};

struct video_groups {
    struct videos *v;
    struct video_group_entry *entries;
    int *first;                 /* each group's first entry, then the end */
    int *seasons;               /* each group's seasons */
    bool all;
};

/* A show's seasons, the first entry of each and then the show's end */
#define MAX_SEASONS 64
struct video_seasons {
    const struct video_groups *groups;
    int count;
    int first[MAX_SEASONS + 1];
};

static int compare_group(const void *left, const void *right)
{
    const struct video_group_entry *a = left;
    const struct video_group_entry *b = right;
    int order = strcasecmp(a->name, b->name);
    if (order)
        return order;
    if (a->season != b->season)
        return a->season < b->season ? -1 : 1;
    if (a->episode != b->episode)
        return a->episode < b->episode ? -1 : 1;
    return a->track - b->track;
}

static const char *group_title(int row, void *data, char *buf, size_t size)
{
    (void)buf;
    (void)size;
    const struct video_groups *groups = data;
    if (groups->all && row == 0)
        return str(LANG_ALL_ALBUMS);
    return groups->entries[groups->first[row - groups->all]].name;
}

static const char *episodes_value(int episodes, char *buf, size_t size)
{
    if (episodes == 1)
        return str(LANG_VIDEO_ONE_EPISODE);
    snprintf(buf, size, str(LANG_VIDEO_EPISODES), episodes);
    return buf;
}

/* A show's seasons, or its episodes when it has one season */
static const char *group_value(int row, void *data, char *buf, size_t size)
{
    const struct video_groups *groups = data;
    if (groups->seasons[row] > 1)
    {
        snprintf(buf, size, str(LANG_VIDEO_SEASONS), groups->seasons[row]);
        return buf;
    }
    return episodes_value(groups->first[row + 1] - groups->first[row], buf,
                          size);
}

/* A show's cover is its first episode's */
static const struct bitmap *group_cover(int row, void *data, struct dim *size)
{
    struct video_groups *groups = data;
    return cover_of(groups->v, groups->entries[groups->first[row]].track,
                    size);
}

static int group_action(int action, struct gui_synclist *lists)
{
    struct video_groups *groups = lists->data;
    return stock_media_list_action(fresh_covers(groups->v, action), lists);
}

static const char *season_title(int row, void *data, char *buf, size_t size)
{
    const struct video_seasons *seasons = data;
    const struct video_group_entry *e =
        &seasons->groups->entries[seasons->first[row]];
    snprintf(buf, size, str(LANG_VIDEO_SEASON), (int)e->season);
    return buf;
}

static const char *season_value(int row, void *data, char *buf, size_t size)
{
    const struct video_seasons *seasons = data;
    return episodes_value(seasons->first[row + 1] - seasons->first[row], buf,
                          size);
}

/* The episodes of entries [first, end) */
static int show_episodes(struct videos *v, const struct video_groups *groups,
                         int first, int end, char *title)
{
    v->row_count = 0;
    for (int i = first; i < end; i++)
        v->rows[v->row_count++] = groups->entries[i].track;
    v->style = ROWS_EPISODES;
    int rc = show_tracks(v, title);
    v->style = ROWS_COVERED;
    return rc;
}

/* The seasons of the show of entries [first, end), each opening its
 * episodes */
static int show_seasons(struct videos *v, const struct video_groups *groups,
                        int first, int end, char *title)
{
    struct video_seasons seasons = {.groups = groups};
    for (int i = first; i < end && seasons.count < MAX_SEASONS; i++)
        if (i == first ||
            groups->entries[i].season != groups->entries[i - 1].season)
            seasons.first[seasons.count++] = i;
    seasons.first[seasons.count] = end;

    struct simplelist_info info;
    simplelist_info_init(&info, title, seasons.count, &seasons);
    info.get_name = season_title;
    info.get_value = season_value;
    info.get_kind = video_folder_kind;
    info.action_callback = stock_media_list_action;
    info.video_rows = true;
    info.row_height = EPISODE_ROW;
    while (true)
    {
        if (simplelist_show_list(&info))
            return MENU_ATTACHED_USB;
        int row = info.selection;
        if (row < 0)
            return 0;
        char name[32];
        season_title(row, &seasons, name, sizeof(name));
        int rc = show_episodes(v, groups, seasons.first[row],
                               seasons.first[row + 1], name);
        if (rc)
            return rc;
    }
}

static int show_groups(struct videos *v, char *title, bool television)
{
    int count = v->row_count;
    int handle = core_alloc(count * sizeof(struct video_group_entry) +
                            (2 * count + 1) * sizeof(int));
    if (handle <= 0)
        return show_tracks(v, title);
    struct video_groups groups = {.v = v,
                                  .entries = core_get_data_pinned(handle),
                                  .all = !television};
    groups.first = (int *)(groups.entries + count);
    groups.seasons = groups.first + count + 1;
    for (int i = 0; i < count; i++)
    {
        struct video_group_entry *entry = &groups.entries[i];
        const struct video_track *track = &v->tracks[v->rows[i]];
        if (!track_text(v, track, television ? track->show : track->artist,
                        entry->name, sizeof(entry->name)))
            snprintf(entry->name, sizeof(entry->name), "%s",
                     str(television ? LANG_VIDEO_UNKNOWN_SHOW :
                                      LANG_VIDEO_UNKNOWN_ARTIST));
        entry->track = v->rows[i];
        entry->season = television ? track->season : 0;
        entry->episode = television ? track->episode : 0;
    }
    qsort(groups.entries, count, sizeof(*groups.entries), compare_group);
    int ngroups = 0;
    for (int i = 0; i < count; i++)
    {
        const struct video_group_entry *e = &groups.entries[i];
        bool show = i == 0 || strcasecmp(e[-1].name, e->name);
        if (show)
        {
            groups.first[ngroups] = i;
            groups.seasons[ngroups++] = 0;
        }
        if (show || e[-1].season != e->season)
            groups.seasons[ngroups - 1]++;
    }
    groups.first[ngroups] = count;

    struct simplelist_info info;
    simplelist_info_init(&info, title, ngroups + groups.all, &groups);
    info.get_name = group_title;
    info.get_kind = video_folder_kind;
    info.action_callback = stock_media_list_action;
    if (television)
    {
        info.get_value = group_value;
        info.get_albumart = group_cover;
        info.action_callback = group_action;
        info.video_rows = true;
        info.row_height = COVER_ROW;
    }
    int rc = 0;
    while (!rc)
    {
        if (simplelist_show_list(&info))
        {
            rc = MENU_ATTACHED_USB;
            break;
        }
        if (info.selection < 0)
            break;
        int group = info.selection - groups.all;
        int first = group < 0 ? 0 : groups.first[group];
        int end = group < 0 ? count : groups.first[group + 1];
        char name[96];
        snprintf(name, sizeof(name), "%s",
                 group_title(info.selection, &groups, NULL, 0));
        if (television && groups.seasons[group] > 1)
            rc = show_seasons(v, &groups, first, end, name);
        else if (television)
            rc = show_episodes(v, &groups, first, end, name);
        else
        {
            v->row_count = 0;
            for (int i = first; i < end; i++)
                v->rows[v->row_count++] = groups.entries[i].track;
            rc = show_tracks(v, name);
        }
    }
    core_unpin(handle);
    core_free(handle);
    return rc;
}

/* Where the video index's strings are read to */
static struct video_index_text index_text;

struct loose_count {
    int count;
    size_t bytes;
};

static bool count_loose(const struct video_entry *e, void *data)
{
    struct loose_count *c = data;
    c->count++;
    c->bytes += strlen(e->path) + strlen(e->title) + strlen(e->artist) +
                strlen(e->show) + 4;
    return c->count < MEDIA_LIMIT;
}

/* `text` into loose_text: where it starts plus one, or 0 for none */
static off_t keep_text(struct videos *v, const char *text)
{
    size_t n = strlen(text) + 1;
    if (n == 1 || v->loose_used + n > v->loose_size)
        return 0;
    off_t at = v->loose_used + 1;
    memcpy(v->loose_text + v->loose_used, text, n);
    v->loose_used += n;
    return at;
}

static bool add_loose(const struct video_entry *e, void *data)
{
    struct videos *v = data;
    if (v->count >= v->capacity)
        return false;
    struct video_track *t = &v->tracks[v->count];
    /* Ids from the top, clear of the library's own */
    *t = (struct video_track){
        .id = UINT32_MAX - v->count, .media = e->kind,
        .season = e->season, .episode = e->episode,
        .duration_ms = e->duration_ms, .released = e->released,
        .art_key = e->art_key, .loose = true,
    };
    t->path = keep_text(v, e->path);
    t->title = keep_text(v, e->title);
    t->artist = keep_text(v, e->artist);
    t->show = keep_text(v, e->show);
    if (t->path)
        v->count++;
    return true;
}

struct title_key {
    char text[24];
    int track;
};

static int compare_title(const void *left, const void *right)
{
    const struct title_key *a = left, *b = right;
    int order = strcasecmp(a->text, b->text);
    return order ? order : a->track - b->track;
}

/* The rows in title order */
static void sort_rows(struct videos *v)
{
    int handle = core_alloc(v->row_count * sizeof(struct title_key));
    if (handle <= 0)
        return;
    struct title_key *keys = core_get_data_pinned(handle);
    for (int i = 0; i < v->row_count; i++)
    {
        keys[i].track = v->rows[i];
        video_title(i, v, keys[i].text, sizeof(keys[i].text));
        yield();
    }
    qsort(keys, v->row_count, sizeof(*keys), compare_title);
    for (int i = 0; i < v->row_count; i++)
        v->rows[i] = keys[i].track;
    core_unpin(handle);
    core_free(handle);
}

/* A video's category: TV over music video over movie */
static unsigned category_of(uint32_t media)
{
    return (media & STOCK_TV_SHOW) ? STOCK_TV_SHOW :
           (media & STOCK_MUSIC_VIDEO) ? STOCK_MUSIC_VIDEO : STOCK_MOVIE;
}

static void census_add(struct stock_video_census *census, uint32_t media)
{
    unsigned category = category_of(media);
    if (category == STOCK_TV_SHOW)
        census->tv_shows++;
    else if (category == STOCK_MUSIC_VIDEO)
        census->music_videos++;
    else
        census->movies++;
}

static bool census_loose(const struct video_entry *e, void *data)
{
    census_add(data, e->kind);
    return true;
}

static off_t index_size(void)
{
    int fd = open(LIB_VIDEO_INDEX_FILE, O_RDONLY);
    if (fd < 0)
        return 0;
    off_t size = ffilesize(fd);
    close(fd);
    return size;
}

/* Library videos with artwork, for the menu pane: a sample of at most
 * PANE_ART of them, taken when the census counts. The pane's thread reads
 * it, so `pane_art_count` is cleared while it is rewritten. */
#define PANE_ART 128
/* Set in a pane cover's number for one of the library's */
#define PANE_LIBRARY (1 << 24)
static struct {
    uint32_t media;
    off_t image;
} pane_art[PANE_ART];
static volatile int pane_art_count;

static void sample_pane_art(struct videos *v)
{
    int count = 0, seen = 0;
    pane_art_count = 0;
    if (!stock_artwork_open(&v->art))
        return;
    read_artwork(v);
    for (int i = 0; i < v->count; i++)
    {
        if (!v->tracks[i].artwork)
            continue;
        int slot = seen < PANE_ART ? seen : rand() % (seen + 1);
        seen++;
        if (slot >= PANE_ART)
            continue;
        pane_art[slot].media = v->tracks[i].media;
        pane_art[slot].image = v->tracks[i].artwork;
        count = MIN(seen, PANE_ART);
    }
    stock_db_close(&v->art);
    pane_art_count = count;
}

/* One of the library's covers of `kinds` other than `previous`, chosen at
 * random, or -1 */
static int library_pane_cover(unsigned kinds, int previous, fb_data *pixels)
{
    int count = pane_art_count, chosen = -1, eligible = 0;
    for (int i = 0; i < count; i++)
    {
        if (!(category_of(pane_art[i].media) & kinds))
            continue;
        if (i == previous && eligible)
            continue;
        if (i == previous || rand() % ++eligible == 0)
            chosen = i;
    }
    if (chosen < 0)
        return -1;
    struct stock_db art;
    bool ok = stock_artwork_open(&art) &&
              stock_artwork_large(&art, pane_art[chosen].image, pixels,
                                  VIDEO_ART_PANE);
    stock_db_close(&art);
    return ok ? chosen : -1;
}

int stock_videos_pane_cover(unsigned kinds, int previous, fb_data *pixels)
{
    bool was_library = previous >= 0 && (previous & PANE_LIBRARY);
    bool library_first = rand() & 1;
    for (int pass = 0; pass < 2; pass++)
    {
        int cover;
        if (library_first == !pass)
        {
            cover = library_pane_cover(kinds, was_library ?
                                       previous & ~PANE_LIBRARY : -1, pixels);
            if (cover >= 0)
                return cover | PANE_LIBRARY;
        }
        else if ((cover = video_art_pane_load(kinds, was_library ? -1 :
                                              previous, pixels)) >= 0)
            return cover;
    }
    return -1;
}

/* Counted again only when the iTunes library or the video index has been
 * written since */
void stock_videos_census(struct stock_video_census *census)
{
    static struct stock_video_census counted;
    static off_t counted_db = -1, counted_index = -1;
    off_t db_size = stock_db_size(), loose_size = index_size();
    if (db_size == counted_db && loose_size == counted_index)
    {
        *census = counted;
        return;
    }
    counted = (struct stock_video_census){0};
    pane_art_count = 0;
    struct videos v = {.art.fd = -1, .db.fd = -1};
    struct stock_record list;
    if (db_size && stock_db_open(&v.db, false) &&
        stock_db_list(&v.db, 1, "mhlt", &list) && list.count)
    {
        unsigned count = MIN(list.count, MEDIA_LIMIT);
        int handle = core_alloc(count * sizeof(*v.tracks) +
                                MAX_PLAYLISTS * sizeof(*v.playlists));
        if (handle > 0)
        {
            v.tracks = core_get_data_pinned(handle);
            v.playlists = (off_t *)(v.tracks + count);
            read_tracks(&v, &list, count);
            qsort(v.tracks, v.count, sizeof(*v.tracks), compare_id);
            for (int i = 0; i < v.count; i++)
                census_add(&counted, v.tracks[i].media);
            read_playlists(&v, false);
            counted.playlists = v.playlist_count;
            sample_pane_art(&v);
            core_unpin(handle);
            core_free(handle);
        }
    }
    stock_db_close(&v.db);
    video_index_each(census_loose, &counted, &index_text);
    counted_db = db_size;
    counted_index = loose_size;
    *census = counted;
}

int stock_videos_screen(enum stock_video_kind kind, int title)
{
    struct videos v = {.art.fd = -1, .db.fd = -1};
    struct stock_record list = {0};
    struct loose_count loose = {0};
    bool menu_back = action_set_menu_back(true);
    int rc = 0, handle = -1;
    struct ui_scene scene = {0};
    if (!stock_db_open(&v.db, false) ||
        !stock_db_list(&v.db, 1, "mhlt", &list))
        list.count = 0;
    if (kind != STOCK_VIDEO_PLAYLISTS)
        video_index_each(count_loose, &loose, &index_text);

    unsigned count = MIN(list.count + loose.count, MEDIA_LIMIT);
    if (!count)
        goto leave;
    handle = core_alloc(count * (sizeof(*v.tracks) + sizeof(*v.rows)) +
                        MAX_PLAYLISTS * (sizeof(*v.playlists) +
                                         sizeof(*v.playlist_sizes)) +
                        loose.bytes);
    v.cover_handle = core_alloc((COVER_SOURCE * COVER_SOURCE +
                                 COVER_SLOTS * COVER_SIZE * COVER_SIZE) *
                                sizeof(fb_data));
    if (handle <= 0)
        goto leave;
    if (v.cover_handle > 0)
        v.cover = core_get_data_pinned(v.cover_handle);
    for (int i = 0; i < COVER_SLOTS; i++)
        v.slots[i].track = -1;
    v.art_generation = video_art_generation();
    v.tracks = core_get_data_pinned(handle);
    v.playlists = (off_t *)(v.tracks + count);
    v.playlist_sizes = (int *)(v.playlists + MAX_PLAYLISTS);
    v.rows = v.playlist_sizes + MAX_PLAYLISTS;
    v.loose_text = (char *)(v.rows + count);
    v.loose_size = loose.bytes;
    v.row_capacity = v.capacity = count;
    if (list.count)
    {
        read_tracks(&v, &list, count);
        read_artwork(&v);
    }
    if (loose.count)
        video_index_each(add_loose, &v, &index_text);
    qsort(v.tracks, v.count, sizeof(*v.tracks), compare_id);
    if (kind == STOCK_VIDEO_PLAYLISTS)
    {
        read_playlists(&v, true);
        if (v.playlist_count)
        {
            ui_scene_open_page(&scene, "videos-push");
            rc = show_playlists(&v, str(title));
        }
    }
    else
    {
        for (int i = 0; i < v.count; i++)
            if (category_of(v.tracks[i].media) == kind)
                v.rows[v.row_count++] = i;
        if (v.row_count)
        {
            ui_scene_open_page(&scene, "videos-push");
            if (kind == STOCK_MOVIE)
            {
                sort_rows(&v);
                rc = show_tracks(&v, str(title));
            }
            else
                rc = show_groups(&v, str(title), kind == STOCK_TV_SHOW);
        }
    }

leave:
    if (handle > 0)
    {
        core_unpin(handle);
        core_free(handle);
    }
    if (v.cover_handle > 0)
    {
        core_unpin(v.cover_handle);
        core_free(v.cover_handle);
    }
    stock_db_close(&v.art);
    stock_db_close(&v.db);
    ui_scene_close_modal(&scene, !rc, "videos-pop");
    action_set_menu_back(menu_back);
    return rc;
}
