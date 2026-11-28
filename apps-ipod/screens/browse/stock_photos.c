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
#include "config.h"
#include "backlight.h"
#include "audio.h"
#include "audio/playback.h"
#include "button.h"
#include "core_alloc.h"
#include "draw/color.h"
#include "input/action.h"
#include "font.h"
#include "files/filetypes.h"
#include "playlist/playlist.h"
#include "misc.h"
#include "kernel.h"
#include "lang.h"
#include "lcd.h"
#include "root_menu.h"
#include "screens/playback/wps.h"
#include "system/volume.h"
#include "settings/settings.h"
#include "system.h"
#include "ui/scene.h"
#include "widgets/list.h"
#include "widgets/menu.h"
#include "widgets/option_select.h"
#include "stock_media.h"

#define PHOTO_LIMIT 32768
#define ALBUM_LIMIT 2048
#define THUMB_EDGE 64
#define GRID_COLS (LCD_WIDTH / 76)
#define GRID_ROWS ((LCD_HEIGHT - 24) / 72)
#define GRID_PAGE (GRID_COLS * GRID_ROWS)
#define PHOTO_PIXELS (LCD_WIDTH * LCD_HEIGHT)

enum { PHOTO_CUT, PHOTO_FADE, PHOTO_WIPE, PHOTO_BLACK, PHOTO_ZOOM, PHOTO_CENTER };

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
    if (row == a->album_count + 1)
        return "Loose photo files";
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

static void draw_grid(const struct albums *a, const char *title, int sel)
{
    static fb_data tile[THUMB_EDGE * THUMB_EDGE];
    int first = sel / GRID_PAGE * GRID_PAGE;
    int gap = LCD_WIDTH / GRID_COLS;
    lcd_set_foreground(LCD_RGBPACK(20, 28, 41));
    lcd_fillrect(0, 0, LCD_WIDTH, LCD_HEIGHT);
    lcd_set_foreground(LCD_WHITE);
    lcd_setfont(FONT_UI);
    lcd_putsxy(12, 4, title);

    for (int i = first; i < a->slide_count && i < first + GRID_PAGE; i++)
    {
        int cell = i - first;
        int x = cell % GRID_COLS * gap + (gap - THUMB_EDGE) / 2;
        int y = 24 + cell / GRID_COLS * 72;
        if (i == sel)
        {
            lcd_set_foreground(LCD_RGBPACK(77, 190, 237));
            lcd_fillrect(x - 3, y - 3, THUMB_EDGE + 6, THUMB_EDGE + 6);
        }
        lcd_set_foreground(LCD_BLACK);
        lcd_fillrect(x, y, THUMB_EDGE, THUMB_EDGE);
        if (stock_photo_pixels(&a->db, a->slides[i], THUMB_EDGE, THUMB_EDGE, tile))
            lcd_bitmap(tile, x, y, THUMB_EDGE, THUMB_EDGE);
    }
    lcd_update();
}

static void draw_photo(const struct albums *a, int sel, fb_data *pixels)
{
    if (!stock_photo_pixels(&a->db, a->slides[sel], LCD_WIDTH, LCD_HEIGHT, pixels))
        memset(pixels, 0, PHOTO_PIXELS * sizeof(*pixels));
}

static void transition(const fb_data *before, const fb_data *after, fb_data *mix)
{
    int mode = global_settings.photo_transition;
    for (int i = 1; mode != PHOTO_CUT && i < 16; i++)
    {
        if (mode == PHOTO_WIPE || mode == PHOTO_CENTER)
        {
            int width = (LCD_WIDTH * i / 16) & ~1;
            int x = mode == PHOTO_CENTER ? (LCD_WIDTH - width) / 2 : 0;
            lcd_bitmap_part(after, x, 0, LCD_WIDTH, x, 0, width, LCD_HEIGHT);
        }
        else if (mode == PHOTO_ZOOM)
        {
            int width = LCD_WIDTH * (16 - i) / 16;
            int height = LCD_HEIGHT * (16 - i) / 16;
            int x = (LCD_WIDTH - width) / 2;
            int y = (LCD_HEIGHT - height) / 2;
            memcpy(mix, after, PHOTO_PIXELS * sizeof(*mix));
            for (int row = 0; row < height; row++)
            {
                const fb_data *src = before + (row * LCD_HEIGHT / height) * LCD_WIDTH;
                fb_data *dest = mix + (row + y) * LCD_WIDTH + x;
                for (int col = 0; col < width; col++)
                    dest[col] = src[col * LCD_WIDTH / width];
            }
            lcd_bitmap(mix, 0, 0, LCD_WIDTH, LCD_HEIGHT);
        }
        else
        {
            const fb_data *from = before;
            const fb_data *to = after;
            int blend = i * 16;
            if (mode == PHOTO_BLACK)
            {
                from = i <= 8 ? before : after;
                to = NULL;
                blend = (i <= 8 ? i : 16 - i) * 32;
            }
            for (unsigned p = 0; p < PHOTO_PIXELS; p++)
                mix[p] = color_blend(from[p], to ? to[p] : LCD_BLACK, blend);
            lcd_bitmap(mix, 0, 0, LCD_WIDTH, LCD_HEIGHT);
        }
        lcd_update();
        sleep(MAX(1, HZ / 32));
    }
    lcd_bitmap(after, 0, 0, LCD_WIDTH, LCD_HEIGHT);
    lcd_update();
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

static const struct button_mapping *photo_view_keys(int context)
{
    (void)context;
    static const struct button_mapping keys[] = {
        { ACTION_WPS_SKIPPREV, BUTTON_LEFT, BUTTON_NONE },
        { ACTION_WPS_SKIPNEXT, BUTTON_RIGHT, BUTTON_NONE },
        { ACTION_NONE, BUTTON_LEFT | BUTTON_REL, BUTTON_NONE },
        { ACTION_NONE, BUTTON_RIGHT | BUTTON_REL, BUTTON_NONE },
        { ACTION_NONE, BUTTON_LEFT | BUTTON_REPEAT, BUTTON_NONE },
        { ACTION_NONE, BUTTON_RIGHT | BUTTON_REPEAT, BUTTON_NONE },
        LAST_ITEM_IN_LIST__NEXTLIST(CONTEXT_WPS)
    };
    return keys;
}

static bool photo_view(const struct albums *a, int *sel)
{
    int handle = core_alloc(3 * PHOTO_PIXELS * sizeof(fb_data) +
                            a->slide_count * sizeof(int));
    if (handle <= 0)
        return false;
    fb_data *current = core_get_data_pinned(handle);
    fb_data *next = current + PHOTO_PIXELS;
    fb_data *mix = next + PHOTO_PIXELS;
    int *order = (int *)(mix + PHOTO_PIXELS);
    int position = *sel;
    enum slideshow_music music = SLIDESHOW_UNCHANGED;
    int interval = MIN(20, MAX(0, global_settings.photo_duration)) * HZ;
    bool playing = false;
    draw_photo(a, *sel, current);
    lcd_bitmap(current, 0, 0, LCD_WIDTH, LCD_HEIGHT);
    lcd_update();

    long due = current_tick + interval;
    bool usb = false;
    for (;;)
    {
        int action = get_custom_action(
            CONTEXT_PLUGIN | CONTEXT_LOCAL_LEFT_RIGHT,
            playing && interval ? MAX(1, due - current_tick) : TIMEOUT_BLOCK,
            photo_view_keys);
        int step = 0;
        if (action == ACTION_WPS_MENU || action == ACTION_STD_CANCEL)
            break;
        if (action == SYS_USB_CONNECTED)
        {
            usb = true;
            break;
        }
        default_event_handler(action);
        if (action == ACTION_WPS_PLAY)
        {
            wps_wheel_transport(action);
            continue;
        }
        if (action == ACTION_WPS_BROWSE)
        {
            playing = !playing;
            if (playing)
            {
                music = slideshow_start(order, a->slide_count, *sel);
                position = global_settings.photo_shuffle ? 0 : *sel;
            }
            else
            {
                slideshow_stop(music, false);
                music = SLIDESHOW_UNCHANGED;
            }
            due = current_tick + interval;
            continue;
        }
        if (playing && (action == ACTION_WPS_VOLUP ||
                        action == ACTION_WPS_VOLDOWN))
        {
            adjust_volume(action == ACTION_WPS_VOLUP ? 1 : -1);
            continue;
        }
        if (action == ACTION_WPS_SKIPPREV || action == ACTION_WPS_VOLDOWN)
            step = -1;
        else if (action == ACTION_WPS_SKIPNEXT || action == ACTION_WPS_VOLUP ||
                 (playing && interval && TIME_AFTER(current_tick + 1, due)))
            step = 1;
        if (!step)
            continue;

        int pos = (playing ? position : *sel) + step;
        if (pos < 0 || pos >= a->slide_count)
        {
            if (playing && global_settings.photo_repeat)
                pos = pos < 0 ? a->slide_count - 1 : 0;
            else if (playing && step > 0)
                break;
            else
                continue;
        }
        position = pos;
        *sel = playing ? order[pos] : pos;
        draw_photo(a, *sel, next);
        if (playing)
            transition(current, next, mix);
        else
        {
            lcd_bitmap(next, 0, 0, LCD_WIDTH, LCD_HEIGHT);
            lcd_update();
        }
        fb_data *swap = current;
        current = next;
        next = swap;
        due = current_tick + interval;
    }
    slideshow_stop(music, usb);
    core_unpin(handle);
    core_free(handle);
    if (usb)
        default_event_handler(SYS_USB_CONNECTED);
    return usb;
}

static bool photo_grid(const struct albums *a, const char *title)
{
    int sel = 0;
    unsigned foreground = lcd_get_foreground();
    lcd_set_viewport(NULL);
    bool usb = false;
    if (a->slide_count)
        draw_grid(a, title, sel);
    while (a->slide_count)
    {
        int action = get_action(CONTEXT_PAGED_LIST | CONTEXT_LOCAL_LEFT_RIGHT,
                                TIMEOUT_BLOCK);
        int button;
        get_action_statuscode(&button);
        if (action == ACTION_STD_CANCEL)
            break;
        if (default_event_handler(action) == SYS_USB_CONNECTED)
        {
            usb = true;
            break;
        }
        if (action == ACTION_LISTTREE_PGUP && sel >= GRID_PAGE)
            sel -= GRID_PAGE;
        else if (action == ACTION_LISTTREE_PGDOWN &&
                 (sel / GRID_PAGE + 1) * GRID_PAGE < a->slide_count)
            sel = MIN(a->slide_count - 1, sel + GRID_PAGE);
        else if (action == ACTION_STD_PREV || action == ACTION_STD_PREVREPEAT)
            sel = MAX(0, sel - 1);
        else if (action == ACTION_STD_NEXT || action == ACTION_STD_NEXTREPEAT)
            sel = MIN(a->slide_count - 1, sel + 1);
        else if (action == ACTION_STD_OK && (button & BUTTON_SELECT))
        {
            usb = photo_view(a, &sel);
            if (usb)
                break;
        }
        else
            continue;
        draw_grid(a, title, sel);
    }
    lcd_set_foreground(foreground);
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
                         a.indexed ? a.album_count + 2 : 2, &a);
    info.get_name = album_name;
    info.action_callback = stock_media_list_action;
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
        else if (!a.indexed || info.selection == a.album_count + 1)
            rc = root_menu_run_screen(GO_TO_IMAGES);
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
    { PHOTO_CUT, LANG_PHOTO_NONE }, { PHOTO_FADE, LANG_PHOTO_FADE },
    { PHOTO_BLACK, LANG_PHOTO_BLACK }, { PHOTO_ZOOM, LANG_PHOTO_ZOOM },
    { PHOTO_WIPE, LANG_PHOTO_WIPE }, { PHOTO_CENTER, LANG_PHOTO_CENTER },
};

static const struct media_choice duration_choices[] = {
    { 0, LANG_PHOTO_MANUAL }, { 2, 0 }, { 5, 0 }, { 10, 0 }, { 20, 0 },
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
    char *title = setting->setting == &global_settings.photo_music
                        ? str(LANG_PHOTO_MUSIC_PICKER) : str(setting->lang_id);
    struct simplelist_info info;
    struct ui_scene scene = {0};
    ui_scene_open_page(&scene, "media-choices-push");
    simplelist_info_init(&info, title, count, (void *)choices);
    info.get_name = choice_name;
    info.action_callback = stock_media_list_action;
    for (int i = 0; i < count; i++)
        if (choices[i].value == option_value_as_int(setting))
            info.selection = i;
    bool usb = simplelist_show_list(&info);
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
