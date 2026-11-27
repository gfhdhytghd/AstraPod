/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Browse Classic photo albums and display their indexed LCD thumbnails.
 * Loose pictures keep the existing image viewer.
 ****************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "backlight.h"
#include "button.h"
#include "core_alloc.h"
#include "draw/color.h"
#include "input/action.h"
#include "font.h"
#include "misc.h"
#include "kernel.h"
#include "lang.h"
#include "lcd.h"
#include "root_menu.h"
#include "settings/settings.h"
#include "system.h"
#include "ui/scene.h"
#include "widgets/list.h"
#include "widgets/menu.h"
#include "stock_media.h"

#define PHOTO_LIMIT 32768
#define ALBUM_LIMIT 2048
#define THUMB_EDGE 64
#define GRID_COLS (LCD_WIDTH / 76)
#define GRID_ROWS ((LCD_HEIGHT - 24) / 72)
#define GRID_PAGE (GRID_COLS * GRID_ROWS)
#define PHOTO_PIXELS (LCD_WIDTH * LCD_HEIGHT)

enum { PHOTO_CUT, PHOTO_FADE, PHOTO_SLIDE };

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
    if (mode == PHOTO_FADE || mode == PHOTO_SLIDE)
    {
        for (int i = 1; i <= 7; i++)
        {
            if (mode == PHOTO_FADE)
            {
                int blend = i * 32;
                for (unsigned p = 0; p < PHOTO_PIXELS; p++)
                    mix[p] = color_blend(before[p], after[p], blend);
                lcd_bitmap(mix, 0, 0, LCD_WIDTH, LCD_HEIGHT);
            }
            else
            {
                int dx = LCD_WIDTH * i / 8;
                lcd_bitmap_part(before, dx, 0, LCD_WIDTH,
                                0, 0, LCD_WIDTH - dx, LCD_HEIGHT);
                lcd_bitmap_part(after, 0, 0, LCD_WIDTH,
                                LCD_WIDTH - dx, 0, dx, LCD_HEIGHT);
            }
            lcd_update();
            sleep(MAX(1, HZ / 32));
        }
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

static bool photo_view(const struct albums *a, int *sel, bool playing)
{
    int handle = core_alloc(3 * PHOTO_PIXELS * sizeof(fb_data));
    if (handle <= 0)
        return false;
    fb_data *current = core_get_data_pinned(handle);
    fb_data *next = current + PHOTO_PIXELS;
    fb_data *mix = next + PHOTO_PIXELS;
    draw_photo(a, *sel, current);
    lcd_bitmap(current, 0, 0, LCD_WIDTH, LCD_HEIGHT);
    lcd_update();

    long due = current_tick + global_settings.photo_duration * HZ;
    bool usb = false;
    if (playing)
        slideshow_light(true);
    for (;;)
    {
        int action = get_action(CONTEXT_WPS, playing ? MAX(1, due - current_tick)
                                                       : TIMEOUT_BLOCK);
        int step = 0;
        if (action == ACTION_WPS_MENU)
            break;
        if (default_event_handler(action) == SYS_USB_CONNECTED)
        {
            usb = true;
            break;
        }
        if (action == ACTION_WPS_PLAY || action == ACTION_WPS_BROWSE)
        {
            playing = !playing;
            if (playing)
                slideshow_light(true);
            else
                slideshow_light(false);
            due = current_tick + global_settings.photo_duration * HZ;
        }
        else if (action == ACTION_WPS_SKIPPREV || action == ACTION_WPS_VOLDOWN)
            step = -1;
        else if (action == ACTION_WPS_SKIPNEXT || action == ACTION_WPS_VOLUP ||
                 (playing && TIME_AFTER(current_tick + 1, due)))
            step = 1;
        if (!step)
            continue;

        int pos = *sel + step;
        if (pos < 0 || pos >= a->slide_count)
        {
            if (playing && global_settings.photo_repeat)
                pos = pos < 0 ? a->slide_count - 1 : 0;
            else if (playing)
                break;
            else
                continue;
        }
        *sel = pos;
        draw_photo(a, pos, next);
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
        due = current_tick + global_settings.photo_duration * HZ;
    }
    slideshow_light(false);
    core_unpin(handle);
    core_free(handle);
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
        int action = get_action(CONTEXT_STD, TIMEOUT_BLOCK);
        int button;
        get_action_statuscode(&button);
        if ((button & BUTTON_MENU) && action == ACTION_STD_CANCEL)
            break;
        if (default_event_handler(action) == SYS_USB_CONNECTED)
        {
            usb = true;
            break;
        }
        if (action == ACTION_STD_PREV || action == ACTION_STD_PREVREPEAT)
            sel = MAX(0, sel - 1);
        else if (action == ACTION_STD_NEXT || action == ACTION_STD_NEXTREPEAT)
            sel = MIN(a->slide_count - 1, sel + 1);
        else if ((action == ACTION_STD_OK && (button & BUTTON_SELECT)) ||
                 action == ACTION_TREE_WPS)
        {
            usb = photo_view(a, &sel, action == ACTION_TREE_WPS);
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

static const int photo_setting_names[] = {
    LANG_SLIDESHOW_TIME, LANG_PHOTO_TRANSITION, LANG_PHOTO_REPEAT,
};

static const char *setting_name(int row, void *data, char *buf, size_t size)
{
    (void)data;
    (void)buf;
    (void)size;
    return str(photo_setting_names[row]);
}

static const char *setting_value(int row, void *data, char *buf, size_t size)
{
    (void)data;
    static const int modes[] = {LANG_OFF, LANG_PHOTO_FADE, LANG_PHOTO_SLIDE};
    if (row == 1)
        return str(modes[global_settings.photo_transition]);
    if (row == 2)
        return str(global_settings.photo_repeat ? LANG_ON : LANG_OFF);
    snprintf(buf, size, "%d s", global_settings.photo_duration);
    return buf;
}

static enum list_row_kind setting_kind(int row, void *data)
{
    (void)row;
    (void)data;
    return LIST_ROW_SETTING;
}

static int change_settings(int action, struct gui_synclist *lists)
{
    action = stock_media_list_action(action, lists);
    if (action != ACTION_STD_OK)
        return action;

    struct user_settings *s = &global_settings;
    switch (gui_synclist_get_sel_pos(lists))
    {
        case 0:
            s->photo_duration = s->photo_duration >= 20 ? 2 : s->photo_duration + 1;
            break;
        case 1:
            s->photo_transition = (s->photo_transition + 1) % 3;
            break;
        case 2:
            s->photo_repeat = !s->photo_repeat;
            break;
    }
    settings_save();
    return ACTION_REDRAW;
}

int stock_photos_settings(void)
{
    struct simplelist_info info;
    simplelist_info_init(&info, str(LANG_SETTINGS), ARRAYLEN(photo_setting_names), NULL);
    info.get_name = setting_name;
    info.get_kind = setting_kind;
    info.get_value = setting_value;
    info.action_callback = change_settings;
    bool menu_back = action_set_menu_back(true);
    bool usb = simplelist_show_list(&info);
    action_set_menu_back(menu_back);
    return usb ? MENU_ATTACHED_USB : 0;
}
