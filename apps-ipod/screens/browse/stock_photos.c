/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Browse Classic photo albums and display their indexed LCD thumbnails.
 * Loose pictures keep the existing image viewer.
 ****************************************************************************/
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ui/motion_schedule.h"
#include "config.h"
#include "backlight.h"
#include "audio.h"
#include "audio/playback.h"
#include "button.h"
#include "core_alloc.h"
#include "draw/color.h"
#include "draw/viewport.h"
#include "input/action.h"
#include "font.h"
#include "files/filetypes.h"
#include "playlist/playlist.h"
#include "misc.h"
#include "kernel.h"
#include "lang.h"
#include "lcd.h"
#include "rbpaths.h"
#include "root_menu.h"
#include "screens/extras/extras_bar.h"
#include "screens/playback/wps.h"
#include "string-extra.h"
#include "system/activity.h"
#include "system/volume.h"
#include "settings/settings.h"
#include "system.h"
#include "ui/menu_pane.h"
#include "ui/scene.h"
#include "ui/stock_art.h"
#include "widgets/list.h"
#include "widgets/menu.h"
#include "widgets/option_select.h"
#include "widgets/splash.h"
#include "browser_flat.h"
#include "stock_media.h"

#define PHOTO_LIMIT 32768
#define ALBUM_LIMIT 2048
#define THUMB_EDGE 64
#define PHOTO_PIXELS (LCD_WIDTH * LCD_HEIGHT)

/* Stock's thumbnail grid: five 62 x 63 cells a row on 64 x 65 pitches below
 * a 46-row header, three rows on screen. A 64 x 64 thumbnail sits in its cell
 * with its first column and last row cut off. */
#define GRID_TOP 46
#define GRID_COLS 5
#define GRID_ROWS 3
#define GRID_PAGE (GRID_COLS * GRID_ROWS)
#define GRID_PITCH_X 64
#define GRID_PITCH_Y 65
#define CELL_W 62
#define CELL_H 63
#define GRID_GREY LCD_RGBPACK(32, 32, 32)
#define FRAME_OUTER LCD_RGBPACK(248, 192, 24)
#define FRAME_INNER LCD_RGBPACK(208, 160, 32)
#define SUBTITLE_FONT FONT_DIR "/13-helvetica-bold.pbf"
#define SUBTITLE_GREY 0x939393
#define SUBTITLE_BASELINE 38
#define SUBTITLE_LEFT 4
#define SUBTITLE_RIGHT 316
/* Mac HFS dates count from 1904. */
#define MAC_EPOCH 2082844800u

enum {
    PHOTO_CUT, PHOTO_FADE, PHOTO_WIPE, PHOTO_BLACK, PHOTO_ZOOM, PHOTO_CENTER,
    PHOTO_RANDOM,
};

struct photo {
    uint32_t id;
    off_t at;
};

struct albums {
    struct stock_db db;
    bool indexed;
    struct photo *photos;
    int photo_count;
    off_t *album;
    int album_count;
    off_t *slides;
    int slide_count;
    int slide_capacity;
};

static int compare_photo(const void *left, const void *right)
{
    uint32_t a = ((const struct photo *)left)->id;
    uint32_t b = ((const struct photo *)right)->id;
    return (a > b) - (a < b);
}

static off_t photo_offset(const struct albums *a, uint32_t id)
{
    int low = 0, high = a->photo_count;
    while (low < high)
    {
        int mid = low + (high - low) / 2;
        if (id == a->photos[mid].id)
            return a->photos[mid].at;
        if (id < a->photos[mid].id)
            high = mid;
        else
            low = mid + 1;
    }
    return 0;
}

static void read_albums(struct albums *a, const struct stock_record *list)
{
    struct stock_record r;
    for (off_t p = list->body; p < list->end && a->album_count < ALBUM_LIMIT;
         p = r.end)
    {
        if (!stock_record_read(&a->db, p, list->end, "mhba", &r) ||
            r.body - p < 32)
            break;
        a->album[a->album_count++] = p;
    }
}

static void read_photos(struct albums *a, const struct stock_record *list,
                        unsigned capacity)
{
    struct stock_record r;
    for (off_t p = list->body; p < list->end && (unsigned)a->photo_count < capacity;
         p = r.end)
    {
        uint32_t id;
        if (!stock_record_read(&a->db, p, list->end, "mhii", &r) ||
            r.body - p < 20 || !stock_db_u32(&a->db, p + 16, &id))
            break;
        a->photos[a->photo_count++] = (struct photo){id, p};
        yield();
    }
    qsort(a->photos, a->photo_count, sizeof(*a->photos), compare_photo);
}

static const char *album_name(int row, void *data, char *buf, size_t size)
{
    struct albums *a = data;
    if (row == 1)
        return str(LANG_SETTINGS);
    if (!a->indexed)
        return str(LANG_FRUIT_ALL_PHOTOS);
    if (row > 1)
        row--;
    struct stock_record album, name;
    uint32_t flags;
    off_t at = a->album[row];
    if (!stock_record_read(&a->db, at, a->db.end, "mhba", &album) ||
        !stock_db_u32(&a->db, at + 28, &flags))
        return "";
    if (((flags >> 16) & 0xff) == 1)
        return str(LANG_FRUIT_ALL_PHOTOS);
    if (stock_record_read(&a->db, album.body, album.end, "mhod", &name) &&
        stock_db_photo_text(&a->db, &name, buf, size))
        return buf;
    return str(LANG_FRUIT_PHOTOS);
}

static void read_album(struct albums *a, int row)
{
    struct stock_record album, r;
    uint32_t metadata, count, flags;
    off_t at = a->album[row];
    a->slide_count = 0;
    if (!stock_record_read(&a->db, at, a->db.end, "mhba", &album) ||
        !stock_db_u32(&a->db, at + 12, &metadata) ||
        !stock_db_u32(&a->db, at + 16, &count) ||
        !stock_db_u32(&a->db, at + 28, &flags))
        return;

    if (((flags >> 16) & 0xff) == 1 && !count)
    {
        for (int i = 0; i < a->photo_count; i++)
            a->slides[a->slide_count++] = a->photos[i].at;
        return;
    }

    off_t p = album.body;
    while (metadata--)
    {
        if (!stock_record_read(&a->db, p, album.end, "mhod", &r))
            return;
        p = r.end;
    }
    while (count-- && a->slide_count < a->slide_capacity)
    {
        uint32_t id;
        if (!stock_record_read(&a->db, p, album.end, "mhia", &r) ||
            r.body - p < 20 || !stock_db_u32(&a->db, p + 16, &id))
            break;
        off_t picture = photo_offset(a, id);
        if (picture)
            a->slides[a->slide_count++] = picture;
        p = r.end;
    }
}

static int subtitle_font = -1;

/* The date a photo was taken, as "November 27, 2026", or "" if unknown. */
static void photo_date(const struct albums *a, int sel, char *buf, size_t size)
{
    uint32_t mac;
    *buf = '\0';
    if (!stock_db_u32(&a->db, a->slides[sel] + 40, &mac) || mac < MAC_EPOCH)
        return;
    time_t when = mac - MAC_EPOCH;
    struct tm tm;
    gmtime_r(&when, &tm);
    snprintf(buf, size, "%s %d, %d", str(LANG_EXTRAS_CALENDARS_JANUARY + tm.tm_mon),
             tm.tm_mday, tm.tm_year + 1900);
}

/* The black title bar, then the album's photo count and the selected
 * photo's date on the grey band below it. */
static void draw_grid_header(const struct albums *a, const char *title,
                             int sel)
{
    enum { ROWS = 4 };
    uint32_t pixels[LCD_WIDTH * ROWS];
    char count[32], date[48];
    int date_width = 0;
    if (subtitle_font < 0)
        subtitle_font = font_load_ex(SUBTITLE_FONT, 0,
                                     global_settings.glyphs_to_cache);
    snprintf(count, sizeof(count), a->slide_count == 1 ?
             str(LANG_FRUIT_ONE_PHOTO) : str(LANG_FRUIT_PHOTO_COUNT),
             a->slide_count);
    photo_date(a, sel, date, sizeof(date));
    int top = 0;
    if (subtitle_font >= 0)
    {
        font_getstringsize((const unsigned char *)date, &date_width, NULL,
                           subtitle_font);
        top = SUBTITLE_BASELINE - font_get(subtitle_font)->ascent;
    }
    bool playing = audio_status() & AUDIO_STATUS_PLAY;
    for (int y = 0; y < GRID_TOP; y += ROWS)
    {
        struct stock_canvas band = { pixels, 0, y, LCD_WIDTH,
                                     MIN(ROWS, GRID_TOP - y) };
        for (int i = 0; i < LCD_WIDTH * band.height; i++)
            pixels[i] = 0x202020;
        stock_art_put(&band, STOCK_ART_PHOTOS_HEADER, 0, 0);
        extras_bar_put_marks(&band, title, true, playing);
        if (subtitle_font >= 0)
        {
            stock_canvas_text(&band, subtitle_font, count, SUBTITLE_LEFT, top,
                              SUBTITLE_GREY);
            stock_canvas_text(&band, subtitle_font, date,
                              SUBTITLE_RIGHT - date_width, top, SUBTITLE_GREY);
        }
        stock_canvas_show(&band, (struct ui_rect){ 0, y, LCD_WIDTH,
                                                   band.height });
    }
}

static void draw_frame(int x, int y)
{
    lcd_set_foreground(FRAME_OUTER);
    lcd_fillrect(x, y, CELL_W, 2);
    lcd_fillrect(x, y + CELL_H - 2, CELL_W, 2);
    lcd_fillrect(x, y + 2, 2, CELL_H - 4);
    lcd_fillrect(x + CELL_W - 2, y + 2, 2, CELL_H - 4);
    lcd_set_foreground(FRAME_INNER);
    lcd_drawrect(x + 2, y + 2, CELL_W - 4, CELL_H - 4);
}

static void draw_grid(const struct albums *a, const char *title,
                      int sel, int first)
{
    static fb_data tile[THUMB_EDGE * THUMB_EDGE];

    draw_grid_header(a, title, sel);
    lcd_set_foreground(GRID_GREY);
    lcd_fillrect(0, GRID_TOP, LCD_WIDTH, LCD_HEIGHT - GRID_TOP);
    for (int i = first; i < a->slide_count && i < first + GRID_PAGE; i++)
    {
        int cell = i - first;
        int x = cell % GRID_COLS * GRID_PITCH_X + 1;
        int y = GRID_TOP + cell / GRID_COLS * GRID_PITCH_Y;
        lcd_set_foreground(LCD_BLACK);
        lcd_fillrect(x, y, CELL_W, CELL_H);
        if (stock_photo_pixels(&a->db, a->slides[i], THUMB_EDGE, THUMB_EDGE,
                               tile))
            lcd_bitmap_part(tile, 1, 0, THUMB_EDGE, x, y, CELL_W, CELL_H);
        if (i == sel)
            draw_frame(x, y);
    }
}

static void draw_photo(const struct albums *a, int sel, fb_data *pixels)
{
    if (!stock_photo_pixels(&a->db, a->slides[sel], LCD_WIDTH, LCD_HEIGHT, pixels))
        memset(pixels, 0, PHOTO_PIXELS * sizeof(*pixels));
}

/* A slideshow's first transition is always Cross Fade, whatever the
 * setting. */
static void transition(const fb_data *before, const fb_data *after,
                       fb_data *mix, int *previous, bool initial)
{
    int mode = initial ? PHOTO_FADE : global_settings.photo_transition;
    if (initial)
        *previous = mode;
    else if (mode == PHOTO_RANDOM)
    {
        static const int effects[] = {
            PHOTO_FADE, PHOTO_BLACK, PHOTO_ZOOM, PHOTO_WIPE, PHOTO_CENTER
        };
        int pick = rand() % ARRAYLEN(effects);
        if (effects[pick] == *previous)
            pick = (pick + 1) % ARRAYLEN(effects);
        mode = effects[pick];
        *previous = mode;
    }

    struct ui_motion motion = {0};
    if (mode != PHOTO_CUT)
        motion = ui_motion_begin("photo-slideshow");
    long start = current_tick;
    for (int i = 1; mode != PHOTO_CUT && i < 16; i++)
    {
        long frame = start + i * HZ / 32;
        if (TIME_BEFORE(current_tick, frame))
            sleep(frame - current_tick);
        int elapsed = (current_tick - start) * 1000 / HZ;
        if (elapsed >= 500)
            break;

        if (mode == PHOTO_WIPE || mode == PHOTO_CENTER)
        {
            int width = (LCD_WIDTH * elapsed / 500) & ~1;
            int x = mode == PHOTO_CENTER ? (LCD_WIDTH - width) / 2 : 0;
            lcd_bitmap_part(after, x, 0, LCD_WIDTH, x, 0, width, LCD_HEIGHT);
        }
        else if (mode == PHOTO_ZOOM)
        {
            int scale = 500 - MIN(elapsed, 250);
            int width = LCD_WIDTH * scale / 500;
            int height = LCD_HEIGHT * scale / 500;
            int x = (LCD_WIDTH - width) / 2;
            int y = (LCD_HEIGHT - height) / 2;
            int blend = MIN(elapsed, 375) * 256 / 375;
            for (unsigned p = 0; p < PHOTO_PIXELS; p++)
                mix[p] = color_blend(LCD_BLACK, after[p], blend);
            for (int row = 0; row < height; row++)
            {
                const fb_data *src = before + (row * LCD_HEIGHT / height) * LCD_WIDTH;
                fb_data *dest = mix + (row + y) * LCD_WIDTH + x;
                for (int col = 0; col < width; col++)
                    dest[col] = color_blend(dest[col], src[col * LCD_WIDTH / width],
                                            256 - blend);
            }
            lcd_bitmap(mix, 0, 0, LCD_WIDTH, LCD_HEIGHT);
        }
        else
        {
            const fb_data *from = before;
            const fb_data *to = after;
            int blend = elapsed * 256 / 500;
            if (mode == PHOTO_BLACK)
            {
                from = elapsed <= 250 ? before : after;
                to = NULL;
                int fade = 200 - MIN(200, abs(elapsed - 250));
                blend = fade * 256 / 200;
            }
            for (unsigned p = 0; p < PHOTO_PIXELS; p++)
                mix[p] = color_blend(from[p], to ? to[p] : LCD_BLACK, blend);
            lcd_bitmap(mix, 0, 0, LCD_WIDTH, LCD_HEIGHT);
        }
        lcd_update();
    }
    long end = start + HZ / 2;
    if (mode != PHOTO_CUT && TIME_BEFORE(current_tick, end))
        sleep(end - current_tick);
    lcd_bitmap(after, 0, 0, LCD_WIDTH, LCD_HEIGHT);
    lcd_update();
    ui_motion_end(&motion);
}

static void slideshow_light(bool on)
{
    backlight_set_timeout(on && global_settings.backlight_timeout > 0 ?
                          0 : global_settings.backlight_timeout);
    backlight_set_timeout_plugged(on && global_settings.backlight_timeout_plugged > 0 ?
                                  0 : global_settings.backlight_timeout_plugged);
}

static void start_order(int *order, int count, int selected)
{
    for (int i = 0; i < count; i++)
        order[i] = i;
    if (!global_settings.photo_shuffle)
        return;
    order[0] = selected;
    order[selected] = 0;
    for (int i = count - 1; i > 1; i--)
    {
        int j = 1 + rand() % i;
        int tmp = order[j];
        order[j] = order[i];
        order[i] = tmp;
    }
}

enum slideshow_music {
    SLIDESHOW_UNCHANGED,
    SLIDESHOW_PAUSED_MUSIC,
    SLIDESHOW_RESUMED_MUSIC,
    SLIDESHOW_OTG,
};

static bool start_otg_music(void)
{
    struct stock_db otg = {.fd = open("/iPod_Control/iTunes/OTGPlaylistInfo",
                                     O_RDONLY)};
    if (otg.fd < 0)
        return false;
    otg.end = ffilesize(otg.fd);
    struct stock_db music = {.fd = -1};
    struct stock_record tracks, track, child;
    uint32_t header, stride, count;
    char tag[4];
    int handle = -1;
    bool started = false;
    if (read(otg.fd, tag, sizeof(tag)) != sizeof(tag) ||
        memcmp(tag, "mhpo", sizeof(tag)) ||
        !stock_db_u32(&otg, 4, &header) ||
        !stock_db_u32(&otg, 8, &stride) ||
        !stock_db_u32(&otg, 12, &count) ||
        header < 20 || header > (uint64_t)otg.end || stride < 4 ||
        !count || count > (uint64_t)(otg.end - header) / stride ||
        !stock_db_open(&music, false) ||
        !stock_db_list(&music, 1, "mhlt", &tracks))
        goto done;

    unsigned capacity = MIN(tracks.count, PHOTO_LIMIT);
    handle = core_alloc(capacity * sizeof(off_t));
    if (handle <= 0)
        goto done;
    off_t *index = core_get_data_pinned(handle);
    unsigned entries = 0;
    for (off_t pos = tracks.body; pos < tracks.end && entries < capacity;
         pos = track.end)
    {
        if (!stock_record_read(&music, pos, tracks.end, "mhit", &track))
            break;
        index[entries++] = pos;
        yield();
    }

    for (uint32_t item = 0; item < MIN(count, PHOTO_LIMIT); item++)
    {
        uint32_t number, media;
        off_t pos = header + (uint64_t)item * stride;
        if (!stock_db_u32(&otg, pos, &number) || number >= entries ||
            !stock_record_read(&music, index[number], tracks.end, "mhit", &track))
            continue;
        if (track.body - track.at >= 212 &&
            (!stock_db_u32(&music, track.at + 208, &media) || media > 1))
            continue;
        for (off_t p = track.body; p < track.end; p = child.end)
        {
            uint32_t type;
            if (!stock_record_read(&music, p, track.end, "mhod", &child) ||
                child.body - p < 16 || !stock_db_u32(&music, p + 12, &type))
                break;
            if (type != 2)
                continue;
            char path[MAX_PATH];
            if (stock_db_music_text(&music, &child, path, sizeof(path)) &&
                stock_music_path(path) && file_exists(path) &&
                (filetype_get_attr(path) & FILE_ATTR_MASK) == FILE_ATTR_AUDIO)
            {
                if (!started)
                {
                    audio_stop();
                    playlist_set_aside();
                    audio_set_unrecorded(true);
                    started = true;
                    if (playlist_create(NULL, NULL) < 0)
                        goto done;
                }
                if (playlist_insert_track(NULL, path, PLAYLIST_INSERT_LAST,
                                          false, false) < 0)
                    goto done;
            }
            break;
        }
        yield();
    }

done:
    if (handle > 0)
    {
        core_unpin(handle);
        core_free(handle);
    }
    stock_db_close(&music);
    stock_db_close(&otg);
    if (started && playlist_amount())
        playlist_start(0, 0, 0);
    return started;
}

static void slideshow_stop(enum slideshow_music music, bool usb)
{
    slideshow_light(false);
    if (music == SLIDESHOW_OTG)
    {
        audio_stop();
        audio_set_unrecorded(false);
        playlist_bring_back();
    }
    else if (!usb && music == SLIDESHOW_PAUSED_MUSIC &&
             audio_status() == (AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE))
        audio_resume();
    else if (!usb && music == SLIDESHOW_RESUMED_MUSIC &&
             audio_status() == AUDIO_STATUS_PLAY)
        audio_pause();
}

static enum slideshow_music slideshow_start(int *order, int count, int selected)
{
    start_order(order, count, selected);
    slideshow_light(true);
    switch (global_settings.photo_music)
    {
        case 0:
            if (audio_status() == AUDIO_STATUS_PLAY)
            {
                audio_pause();
                return SLIDESHOW_PAUSED_MUSIC;
            }
            break;
        case 1:
            if (audio_status() == (AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE))
            {
                audio_resume();
                return SLIDESHOW_RESUMED_MUSIC;
            }
            break;
        case 2:
            if (start_otg_music())
                return SLIDESHOW_OTG;
            break;
    }
    return SLIDESHOW_UNCHANGED;
}

#define PHOTO_ARROW_KEYS(earlier, later) \
    { ACTION_NONE, BUTTON_LEFT, BUTTON_NONE }, \
    { ACTION_NONE, BUTTON_RIGHT, BUTTON_NONE }, \
    { earlier, BUTTON_LEFT | BUTTON_REL, BUTTON_LEFT }, \
    { later, BUTTON_RIGHT | BUTTON_REL, BUTTON_RIGHT }, \
    { ACTION_NONE, BUTTON_LEFT | BUTTON_REL, BUTTON_NONE }, \
    { ACTION_NONE, BUTTON_RIGHT | BUTTON_REL, BUTTON_NONE }, \
    { ACTION_WPS_SEEKBACK, BUTTON_LEFT | BUTTON_REPEAT, BUTTON_LEFT }, \
    { ACTION_WPS_SEEKFWD, BUTTON_RIGHT | BUTTON_REPEAT, BUTTON_RIGHT }, \
    { ACTION_NONE, BUTTON_LEFT | BUTTON_REPEAT, BUTTON_NONE }, \
    { ACTION_NONE, BUTTON_RIGHT | BUTTON_REPEAT, BUTTON_NONE }

static const struct button_mapping *photo_view_keys(int context)
{
    (void)context;
    static const struct button_mapping keys[] = {
        PHOTO_ARROW_KEYS(ACTION_WPS_SKIPPREV, ACTION_WPS_SKIPNEXT),
        LAST_ITEM_IN_LIST__NEXTLIST(CONTEXT_WPS)
    };
    return keys;
}

static const struct button_mapping *photo_grid_keys(int context)
{
    (void)context;
    static const struct button_mapping keys[] = {
        PHOTO_ARROW_KEYS(ACTION_LISTTREE_PGUP, ACTION_LISTTREE_PGDOWN),
        LAST_ITEM_IN_LIST__NEXTLIST(CONTEXT_PAGED_LIST)
    };
    return keys;
}

#undef PHOTO_ARROW_KEYS

static int photo_wheel_steps(void)
{
    unsigned data = get_action_data();
    return (data >> 31) ? MAX(1u, (data >> 24) & 0x7f) : 1;
}

static void present_photo(const fb_data *pixels, bool brightness)
{
    lcd_bitmap(pixels, 0, 0, LCD_WIDTH, LCD_HEIGHT);
#ifdef HAVE_BACKLIGHT_BRIGHTNESS
    if (brightness)
    {
        struct viewport area = { .flags = VP_DEFAULT_FLAGS };
        viewport_set_fullscreen(&area, SCREEN_MAIN);
        splash_progress_in(&area,
                           global_settings.brightness - MIN_BRIGHTNESS_SETTING,
                           MAX_BRIGHTNESS_SETTING - MIN_BRIGHTNESS_SETTING,
                           "%s", str(LANG_BRIGHTNESS));
    }
#else
    (void)brightness;
#endif
    lcd_update();
}

/* Stock pushes between the grid and a photo in 8 frames of 1/30 s, easing
 * out quadratically; forward moves the pages left. */
static void push_pages(const fb_data *from, const fb_data *to, bool forward)
{
    if (global_settings.animations)
    {
        struct ui_motion motion = ui_motion_begin("photo-push");
        long start = current_tick;
        for (int k = 1; k < 8; k++)
        {
            long frame = start + k * HZ / 30;
            if (TIME_BEFORE(current_tick, frame))
                sleep(frame - current_tick);
            int rest = 8 - k;
            int off = (LCD_WIDTH * (64 - rest * rest) + 63) / 64;
            int keep = LCD_WIDTH - off;
            if (forward)
            {
                lcd_bitmap_part(from, off, 0, LCD_WIDTH, 0, 0, keep, LCD_HEIGHT);
                lcd_bitmap_part(to, 0, 0, LCD_WIDTH, keep, 0, off, LCD_HEIGHT);
            }
            else
            {
                lcd_bitmap_part(to, keep, 0, LCD_WIDTH, 0, 0, off, LCD_HEIGHT);
                lcd_bitmap_part(from, 0, 0, LCD_WIDTH, off, 0, keep, LCD_HEIGHT);
            }
            lcd_update();
        }
        long end = start + 8 * HZ / 30;
        if (TIME_BEFORE(current_tick, end))
            sleep(end - current_tick);
        ui_motion_end(&motion);
    }
    lcd_bitmap(to, 0, 0, LCD_WIDTH, LCD_HEIGHT);
    lcd_update();
}

enum photo_view_result {
    PHOTO_VIEW_GRID,
    PHOTO_VIEW_ALBUMS,
    PHOTO_VIEW_USB,
};

static enum photo_view_result photo_view(const struct albums *a, int *sel)
{
    int handle = core_alloc(3 * PHOTO_PIXELS * sizeof(fb_data) +
                            a->slide_count * sizeof(int));
    if (handle <= 0)
        return PHOTO_VIEW_GRID;
    fb_data *current = core_get_data_pinned(handle);
    fb_data *next = current + PHOTO_PIXELS;
    fb_data *mix = next + PHOTO_PIXELS;
    int *order = (int *)(mix + PHOTO_PIXELS);
    int position = *sel;
    enum slideshow_music music = SLIDESHOW_UNCHANGED;
    int interval = MIN(20, MAX(0, global_settings.photo_duration)) * HZ;
    int last_transition = PHOTO_CUT;
    bool playing = false, initial = false;
    long due = current_tick + interval;
    bool brightness = false;
    long brightness_due = 0;
#ifdef HAVE_BACKLIGHT_BRIGHTNESS
    int initial_brightness = global_settings.brightness;
#endif
    enum photo_view_result result = PHOTO_VIEW_GRID;
    memcpy(mix, FBADDR(0, 0), PHOTO_PIXELS * sizeof(fb_data));
    draw_photo(a, *sel, current);
    push_pages(mix, current, true);
    for (;;)
    {
        long now = current_tick;
        int timeout = playing && interval ? MAX(1, due - now) : TIMEOUT_BLOCK;
        if (brightness)
        {
            int remaining = MAX(1, brightness_due - now);
            timeout = timeout == TIMEOUT_BLOCK ? remaining : MIN(timeout, remaining);
        }
        int action = get_custom_action(
            CONTEXT_PLUGIN | CONTEXT_WHEEL_DETENTS,
            timeout, photo_view_keys);
        if (action == ACTION_WPS_MENU || action == ACTION_STD_CANCEL)
        {
            result = playing ? PHOTO_VIEW_ALBUMS : PHOTO_VIEW_GRID;
            break;
        }
        if (action == SYS_USB_CONNECTED)
        {
            result = PHOTO_VIEW_USB;
            break;
        }
        default_event_handler(action);
        bool redraw = action == ACTION_REDRAW;
        if (brightness && !TIME_BEFORE(current_tick, brightness_due))
        {
            brightness = false;
            redraw = true;
        }

        int pos = playing ? position : *sel;
        int step = 0;
        if (action == ACTION_WPS_PLAY)
            wps_wheel_transport(action);
        else if (action == ACTION_WPS_BROWSE)
        {
            if (!playing)
            {
                music = slideshow_start(order, a->slide_count, *sel);
                pos = position = global_settings.photo_shuffle ? 0 : *sel;
                last_transition = PHOTO_CUT;
                playing = initial = true;
                due = current_tick + interval;
            }
#ifdef HAVE_BACKLIGHT_BRIGHTNESS
            else
            {
                brightness = !brightness;
                brightness_due = current_tick + 5 * HZ;
                redraw = true;
            }
#endif
        }
        else if (action == ACTION_WPS_VOLUP || action == ACTION_WPS_VOLDOWN)
        {
            int wheel = photo_wheel_steps();
            if (action == ACTION_WPS_VOLDOWN)
                wheel = -wheel;
#ifdef HAVE_BACKLIGHT_BRIGHTNESS
            if (brightness)
            {
                int level = global_settings.brightness + wheel;
                global_settings.brightness =
                    MIN(MAX_BRIGHTNESS_SETTING, MAX(MIN_BRIGHTNESS_SETTING, level));
                backlight_set_brightness(global_settings.brightness);
                brightness_due = current_tick + 5 * HZ;
                redraw = true;
            }
            else
#endif
            if (playing)
                adjust_volume(wheel);
            else
                step = wheel;
        }
        else if (action == ACTION_WPS_SKIPPREV)
            step = -1;
        else if (action == ACTION_WPS_SKIPNEXT)
            step = 1;
        else if (!playing && action == ACTION_WPS_SEEKBACK)
            pos = 0;
        else if (!playing && action == ACTION_WPS_SEEKFWD)
            pos = a->slide_count - 1;

        if (playing && !step && interval && !TIME_BEFORE(current_tick, due))
            step = 1;
        pos += step;
        if (playing && (pos < 0 || pos >= a->slide_count))
        {
            if (global_settings.photo_repeat)
                pos = pos < 0 ? a->slide_count - 1 : 0;
            else if (step > 0)
                break;
            else
                pos = position;
        }
        else
            pos = MIN(a->slide_count - 1, MAX(0, pos));

        if (playing ? (pos != position || (step && global_settings.photo_repeat))
                    : pos != *sel)
        {
            position = pos;
            *sel = playing ? order[pos] : pos;
            draw_photo(a, *sel, next);
            if (playing)
            {
                if (brightness || redraw)
                    lcd_bitmap(current, 0, 0, LCD_WIDTH, LCD_HEIGHT);
                transition(current, next, mix, &last_transition, initial);
                initial = false;
                brightness = brightness && TIME_BEFORE(current_tick, brightness_due);
            }
            fb_data *swap = current;
            current = next;
            next = swap;
            due = current_tick + interval;
            redraw = !playing || brightness;
        }
        if (redraw)
            present_photo(current, brightness);
    }
    slideshow_stop(music, result == PHOTO_VIEW_USB);
#ifdef HAVE_BACKLIGHT_BRIGHTNESS
    if (global_settings.brightness != initial_brightness)
        settings_save();
#endif
    core_unpin(handle);
    core_free(handle);
    if (result == PHOTO_VIEW_USB)
        default_event_handler(SYS_USB_CONNECTED);
    return result;
}

/* Push the grid back in from the left over the photo on the screen. */
static void return_to_grid(const struct albums *a, const char *title,
                           int sel, int first)
{
    int handle = core_alloc(2 * PHOTO_PIXELS * sizeof(fb_data));
    if (handle <= 0)
    {
        draw_grid(a, title, sel, first);
        lcd_update();
        return;
    }
    fb_data *photo = core_get_data_pinned(handle);
    fb_data *grid = photo + PHOTO_PIXELS;
    memcpy(photo, FBADDR(0, 0), PHOTO_PIXELS * sizeof(fb_data));
    draw_grid(a, title, sel, first);
    memcpy(grid, FBADDR(0, 0), PHOTO_PIXELS * sizeof(fb_data));
    push_pages(photo, grid, false);
    core_unpin(handle);
    core_free(handle);
}

static bool photo_grid(const struct albums *a, const char *title)
{
    if (!a->slide_count)
        return false;

    int sel = 0, first = 0;
    int last_row = (a->slide_count - 1) / GRID_COLS;
    int last_first = MAX(0, last_row - GRID_ROWS + 1) * GRID_COLS;
    unsigned foreground = lcd_get_foreground();
    viewportmanager_theme_enable(SCREEN_MAIN, false, NULL);
    lcd_set_viewport(NULL);
    bool usb = false;
    draw_grid(a, title, sel, first);
    lcd_update();
    for (;;)
    {
        bool back = false;
        int action = get_custom_action(CONTEXT_PLUGIN | CONTEXT_PAGED_LIST,
                                       TIMEOUT_BLOCK, photo_grid_keys);
        int button;
        get_action_statuscode(&button);
        if (action == ACTION_STD_CANCEL)
            break;
        if (default_event_handler(action) == SYS_USB_CONNECTED)
        {
            usb = true;
            break;
        }
        if (action == ACTION_WPS_SEEKBACK)
            sel = 0;
        else if (action == ACTION_WPS_SEEKFWD)
            sel = a->slide_count - 1;
        else if (action == ACTION_LISTTREE_PGUP && first > 0)
        {
            sel = MAX(0, sel - GRID_PAGE);
            first = MAX(0, first - GRID_PAGE);
        }
        else if (action == ACTION_LISTTREE_PGDOWN && first < last_first)
        {
            sel = MIN(a->slide_count - 1, sel + GRID_PAGE);
            first = MIN(last_first, first + GRID_PAGE);
        }
        else if (action == ACTION_STD_PREV || action == ACTION_STD_PREVREPEAT)
            sel = MAX(0, sel - 1);
        else if (action == ACTION_STD_NEXT || action == ACTION_STD_NEXTREPEAT)
            sel = MIN(a->slide_count - 1, sel + 1);
        else if (action == ACTION_STD_OK && (button & BUTTON_SELECT))
        {
            enum photo_view_result result = photo_view(a, &sel);
            usb = result == PHOTO_VIEW_USB;
            if (result != PHOTO_VIEW_GRID)
                break;
            back = true;
        }
        else
            continue;
        int row_start = sel / GRID_COLS * GRID_COLS;
        first = MIN(row_start, MAX(first, row_start - GRID_PAGE + GRID_COLS));
        if (back)
            return_to_grid(a, title, sel, first);
        else
        {
            draw_grid(a, title, sel, first);
            lcd_update();
        }
    }
    lcd_set_foreground(foreground);
    if (subtitle_font >= 0)
        font_unload(subtitle_font);
    subtitle_font = -1;
    extras_bar_release();
    viewportmanager_theme_undo(SCREEN_MAIN, false);
    return usb;
}

int stock_media_list_action(int action, struct gui_synclist *lists)
{
    (void)lists;
    int button;
    get_action_statuscode(&button);
    if ((button & (BUTTON_LEFT | BUTTON_RIGHT | BUTTON_PLAY)) &&
        (action == ACTION_STD_OK || action == ACTION_STD_CANCEL ||
         action == ACTION_TREE_WPS))
        return ACTION_NONE;
    return action;
}

/* Whether the file walk has found any images, or may yet. */
static bool loose_photos(void)
{
    if (file_index_is_busy())
        return true;
    int fd = open(file_index_list(FLAT_IMAGES), O_RDONLY);
    if (fd < 0)
        return false;
    bool any = ffilesize(fd) > 0;
    close(fd);
    return any;
}

/* The Photos menu's pane: the main menu's cover slideshow, as on stock, or
 * stock's No Photos picture when the library has no photos. */
static void paint_no_photos(struct stock_canvas *band)
{
    stock_art_mask(band, STOCK_ART_NO_CONTENT_PHOTO_PREVIEW, 178, 44,
                   0xffffff);
}

static enum list_pane_kind photos_pane(int row, void *data, int *art)
{
    (void)row;
    (void)art;
    if (((struct albums *)data)->indexed)
        return LIST_PANE_ART;
    struct menu_pane_content c = {
        .kind = LIST_PANE_PICTURE, .art = -1, .baseline = 179,
        .paint = paint_no_photos,
    };
    strlcpy(c.title, str(LANG_FRUIT_NO_PHOTOS), sizeof(c.title));
    menu_pane_set_picture(&c);
    return LIST_PANE_PICTURE;
}

static int photos_list_action(int action, struct gui_synclist *lists)
{
    if (action == ACTION_REDRAW)
        gui_synclist_set_pane_callback(lists, photos_pane);
    return stock_media_list_action(action, lists);
}

int stock_photos_screen(void)
{
    struct albums a = {0};
    struct stock_record images, albums;
    int handle = -1, rc = 0;
    bool menu_back = action_set_menu_back(true);
    if (stock_db_open(&a.db, true) &&
        stock_db_list(&a.db, 1, "mhli", &images) && images.count &&
        stock_db_list(&a.db, 2, "mhla", &albums))
    {
        unsigned count = MIN(images.count, PHOTO_LIMIT);
        handle = core_alloc(count * (sizeof(*a.photos) + sizeof(*a.slides)) +
                            ALBUM_LIMIT * sizeof(*a.album));
        if (handle > 0)
        {
            a.photos = core_get_data_pinned(handle);
            a.album = (off_t *)(a.photos + count);
            a.slides = a.album + ALBUM_LIMIT;
            a.slide_capacity = count;
            read_photos(&a, &images, count);
            read_albums(&a, &albums);
            a.indexed = a.album_count && a.photo_count;
        }
    }

    struct ui_scene scene = {0};
    ui_scene_open_page(&scene, "photos-push");
    struct simplelist_info info;
    simplelist_info_init(&info, str(LANG_FRUIT_PHOTOS),
                         a.indexed ? a.album_count + 1 : 2, &a);
    info.get_name = album_name;
    info.action_callback = photos_list_action;
    info.page_with_left_right = true;
    while (true)
    {
        if (simplelist_show_list(&info))
        {
            rc = MENU_ATTACHED_USB;
            break;
        }
        if (info.selection < 0)
            break;
        if (info.selection == 1)
            rc = stock_photos_settings();
        else if (!a.indexed)
        {
            /* Stock leaves an empty library's menu up. */
            if (!loose_photos())
                continue;
            struct ui_scene files = {0};
            ui_scene_open_page(&files, "loose-photos-push");
            rc = browser_flat(FLAT_IMAGES);
            ui_scene_close_modal(&files, rc == GO_TO_PREVIOUS,
                                 "loose-photos-pop");
            if (rc == GO_TO_PREVIOUS)
                rc = 0;
        }
        else
        {
            char title[96];
            read_album(&a, info.selection ? info.selection - 1 : 0);
            if (photo_grid(&a, album_name(info.selection, &a, title, sizeof(title))))
                rc = MENU_ATTACHED_USB;
        }
        if (rc)
            break;
    }
    ui_scene_close_modal(&scene, !rc, "photos-pop");
    if (handle > 0)
    {
        core_unpin(handle);
        core_free(handle);
    }
    stock_db_close(&a.db);
    action_set_menu_back(menu_back);
    return rc;
}

struct media_option {
    void *value;
    bool submenu;
};

#define MEDIA_OPTION(field, submenu) { &global_settings.field, submenu }

static const struct media_option photo_options[] = {
    MEDIA_OPTION(photo_duration, true),
    MEDIA_OPTION(photo_music, true),
    MEDIA_OPTION(photo_repeat, false),
    MEDIA_OPTION(photo_shuffle, false),
    MEDIA_OPTION(photo_transition, true),
    MEDIA_OPTION(media_tv_out, false),
    MEDIA_OPTION(media_tv_signal, false),
};

static const struct media_option video_options[] = {
    MEDIA_OPTION(media_tv_out, false),
    MEDIA_OPTION(media_tv_signal, false),
    MEDIA_OPTION(video_tv_screen, false),
    MEDIA_OPTION(video_picture_mode, false),
    MEDIA_OPTION(video_alt_audio, false),
    MEDIA_OPTION(video_captions, false),
    MEDIA_OPTION(video_subtitles, false),
};

static const struct settings_list *media_setting(int row, const void *data)
{
    const struct media_option *options = data;
    return find_setting(options[row].value);
}

static const char *setting_name(int row, void *data, char *buf, size_t size)
{
    (void)buf;
    (void)size;
    return str(media_setting(row, data)->lang_id);
}

static const char *setting_value(int row, void *data, char *buf, size_t size)
{
    const struct settings_list *setting = media_setting(row, data);
    if (setting->setting == &global_settings.photo_duration &&
        !global_settings.photo_duration)
        return str(LANG_PHOTO_MANUAL);
    if ((setting->flags & F_T_MASK) == F_T_BOOL)
        return str(option_value_as_int(setting) ? LANG_ON : LANG_OFF);
    return option_get_valuestring(setting, buf, size,
                                  option_value_as_int(setting));
}

static enum list_row_kind setting_kind(int row, void *data)
{
    const struct media_option *options = data;
    return options[row].submenu ? LIST_ROW_MENU : LIST_ROW_SETTING;
}

struct media_choice {
    int value;
    int label;
};

static const struct media_choice music_choices[] = {
    { 1, LANG_NOW_PLAYING }, { 0, LANG_OFF }, { 2, LANG_PHOTO_OTG },
};

static const struct media_choice transition_choices[] = {
    { PHOTO_CUT, LANG_PHOTO_NONE }, { PHOTO_RANDOM, LANG_RANDOM },
    { PHOTO_FADE, LANG_PHOTO_FADE },
    { PHOTO_BLACK, LANG_PHOTO_BLACK }, { PHOTO_ZOOM, LANG_PHOTO_ZOOM },
    { PHOTO_WIPE, LANG_PHOTO_WIPE }, { PHOTO_CENTER, LANG_PHOTO_CENTER },
};

static const struct media_choice duration_choices[] = {
    { 0, LANG_PHOTO_MANUAL }, { 2, 0 }, { 3, 0 }, { 5, 0 }, { 10, 0 }, { 20, 0 },
};

static const char *choice_name(int row, void *data, char *buf, size_t size)
{
    const struct media_choice *choices = data;
    if (choices[row].label)
        return str(choices[row].label);
    snprintf(buf, size, "%d %s", choices[row].value, str(LANG_PHOTO_SECONDS));
    return buf;
}

static bool media_picker(const struct settings_list *setting)
{
    const struct media_choice *choices = duration_choices;
    int count = ARRAYLEN(duration_choices);
    if (setting->setting == &global_settings.photo_transition)
    {
        choices = transition_choices;
        count = ARRAYLEN(transition_choices);
    }
    else if (setting->setting == &global_settings.photo_music)
    {
        choices = music_choices;
        count = ARRAYLEN(music_choices);
    }
    char *title = str(setting->lang_id);
    if (setting->setting == &global_settings.photo_music)
        title = str(LANG_PHOTO_MUSIC_PICKER);
    else if (setting->setting == &global_settings.photo_duration)
        title = str(LANG_PHOTO_DURATION_PICKER);
    struct simplelist_info info;
    struct ui_scene scene = {0};
    ui_scene_open_page(&scene, "media-choices-push");
    simplelist_info_init(&info, title, count, (void *)choices);
    info.fruit_nav = true;
    info.get_name = choice_name;
    info.action_callback = stock_media_list_action;
    for (int i = 0; i < count; i++)
        if (choices[i].value == option_value_as_int(setting))
            info.selection = i;
    /* A picker's rows carry no chevron. */
    push_current_activity(ACTIVITY_OPTIONSELECT);
    bool usb = simplelist_show_list(&info);
    pop_current_activity();
    if (!usb && info.selection >= 0)
        *(int *)setting->setting = choices[info.selection].value;
    ui_scene_close_modal(&scene, !usb, "media-choices-pop");
    return usb;
}

static int media_settings(const struct media_option *options, int count)
{
    struct simplelist_info info;
    struct ui_scene scene = {0};
    ui_scene_open_page(&scene, "media-settings-push");
    simplelist_info_init(&info, str(LANG_SETTINGS), count, (void *)options);
    info.fruit_nav = true;
    info.get_name = setting_name;
    info.get_kind = setting_kind;
    info.get_value = setting_value;
    info.action_callback = stock_media_list_action;
    bool menu_back = action_set_menu_back(true);
    bool usb;
    while (!(usb = simplelist_show_list(&info)) && info.selection >= 0)
    {
        int row = info.selection;
        const struct settings_list *setting = media_setting(row, options);
        if (options[row].submenu)
        {
            if (media_picker(setting))
            {
                usb = true;
                break;
            }
        }
        else
            option_select_next_val(setting, false, true);
        settings_save();
    }
    action_set_menu_back(menu_back);
    ui_scene_close_modal(&scene, !usb, "media-settings-pop");
    return usb ? MENU_ATTACHED_USB : 0;
}

int stock_photos_settings(void)
{
    return media_settings(photo_options, ARRAYLEN(photo_options));
}

int stock_videos_settings(void)
{
    return media_settings(video_options, ARRAYLEN(video_options));
}
