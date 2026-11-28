/* Copyright (C) 2026 AstraPod contributors
 * GPL version 2 or later.
 * The stock menu tree's persistent On-The-Go queue and playlist screens. */

#include "config.h"
#include "action.h"
#include "audio.h"
#include "button.h"
#include "dir.h"
#include "file.h"
#include "files/filetypes.h"
#include "database/path_key.h"
#include "general.h"
#include "kernel.h"
#include "lang.h"
#include "pathfuncs.h"
#include "rbpaths.h"
#include "root_menu.h"
#include "catalog.h"
#include "fruit_otg.h"
#include "playlist.h"
#include "database/tagcache.h"
#include "screens/browse/browser_db.h"
#include "screens/context_menu.h"
#include "settings/settings.h"
#include "system/strutil.h"
#include "string-extra.h"
#include "widgets/dialog.h"
#include "widgets/list.h"
#include "widgets/splash.h"
#include "draw/screen_access.h"
#include "draw/text_wrap.h"
#include "font.h"
#include "lcd.h"
#include "screens/extras/extras_bar.h"
#include "system/shutdown.h"
#include "ui/native_chrome.h"
#include "ui/native_theme_runtime.h"
#include "ui/stock_art.h"

#define OTG_PATH ROCKBOX_DIR "/on-the-go.m3u8"
#define OTG_STAGE ROCKBOX_DIR "/on-the-go.tmp"
#define OTG_LIMIT 1000

/* Offsets are rebuilt when opening the catalogue and after each edit. The
 * queue is never a backing file for the current playback playlist. */
static off_t offsets[OTG_LIMIT];
static int count;
static int staging_fd;
static int staging_count;

static bool append_track(const char *path)
{
    if (!path || path[0] != '/' || strchr(path, '\r') || strchr(path, '\n') ||
        staging_count >= OTG_LIMIT)
        return false;

    size_t len = strlen(path);
    if (write(staging_fd, path, len) != (ssize_t)len ||
        write(staging_fd, "\n", 1) != 1)
        return false;
    staging_count++;
    return true;
}

static bool copy_queue(int source)
{
    char block[512];
    ssize_t n;

    while ((n = read(source, block, sizeof(block))) > 0)
        if (write(staging_fd, block, n) != n)
            return false;
    return n == 0;
}

void fruit_otg_refresh(void)
{
    char path[MAX_PATH + 1];
    int fd = open(OTG_PATH, O_RDONLY);
    count = 0;
    if (fd < 0)
        return;

    while (count < OTG_LIMIT)
    {
        off_t offset = lseek(fd, 0, SEEK_CUR);
        if (offset < 0 || read_line(fd, path, sizeof(path)) <= 0)
            break;
        if (path[0] == '/')
            offsets[count++] = offset;
    }
    close(fd);
}

int fruit_otg_count(void)
{
    return count;
}

/* Count each saved list once as its rows are revealed. Counting must not
 * use the playlist parser's action polling from a draw callback. */
int fruit_playlist_count(const char *path, unsigned mtime)
{
    static struct {
        uint64_t key;
        unsigned mtime;
        int songs;
    } cache[16];
    static unsigned next;
    uint64_t key = path_key(path);
    for (size_t i = 0; i < ARRAYLEN(cache); i++)
        if (key && cache[i].key == key && cache[i].mtime == mtime)
            return cache[i].songs;

    int fd = open_utf8(path, O_RDONLY);
    if (fd < 0)
        return -1;

    char line[MAX_PATH + 1];
    int songs = 0;
    int n;
    while ((n = read_line(fd, line, sizeof(line))) > 0)
        if (line[0] && line[0] != '#')
            songs++;
    close(fd);
    if (n < 0)
        return -1;

    unsigned i = next++ % ARRAYLEN(cache);
    cache[i].key = key;
    cache[i].mtime = mtime;
    cache[i].songs = songs;
    return songs;
}

const char *fruit_otg_count_text(char *buf, size_t size, int songs, bool remove)
{
    int phrase = remove ?
        (songs == 1 ? LANG_FRUIT_OTG_REMOVE_ONE : LANG_FRUIT_OTG_REMOVE_MANY) :
        (songs == 1 ? LANG_FRUIT_OTG_ONE_SONG : LANG_FRUIT_OTG_MANY_SONGS);
    snprintf(buf, size, str(phrase), songs);
    return buf;
}

bool fruit_otg_add(const char *path, bool db_group)
{
    fruit_otg_refresh();
    if (count >= OTG_LIMIT)
        return false;

    int source = open(OTG_PATH, O_RDONLY);
    if (source < 0 && file_exists(OTG_PATH))
        return false;
    staging_fd = open(OTG_STAGE, O_CREAT | O_TRUNC | O_WRONLY, 0666);
    if (staging_fd < 0)
    {
        if (source >= 0)
            close(source);
        return false;
    }

    staging_count = count;
    bool ok = source < 0 || copy_queue(source);
    if (source >= 0)
        close(source);

    if (ok && db_group)
    {
        static struct tagcache_search search;
        static char filename[MAX_PATH];
        ok = browser_db_visit_selection(&search, append_track, filename,
                                        sizeof(filename));
    }
    else if (ok && path && filetype_get_attr(path) == FILE_ATTR_M3U)
        ok = playlist_visit_all(path, append_track);
    else if (ok)
        ok = append_track(path);

    ok = ok && staging_count > count && fsync(staging_fd) == 0;
    if (close(staging_fd) < 0)
        ok = false;
    if (ok && rename(OTG_STAGE, OTG_PATH) == 0)
    {
        fruit_otg_refresh();
        return true;
    }
    remove(OTG_STAGE);
    return false;
}

static bool queue_path(int item, char *path, size_t size)
{
    int fd = open(OTG_PATH, O_RDONLY);
    if (fd < 0)
        return false;
    bool ok = lseek(fd, offsets[item], SEEK_SET) == offsets[item] &&
              read_line(fd, path, size) > 0;
    close(fd);
    return ok;
}

/* The renderer asks for a title and its subtitle in the same draw. */
static int cached_item = -1;
static char cached_title[MAX_PATH], cached_artist[MAX_PATH];

static void queue_tags(int item)
{
    char path[MAX_PATH + 1];
    if (item == cached_item)
        return;

    cached_item = item;
    cached_title[0] = cached_artist[0] = '\0';
    if (!queue_path(item, path, sizeof(path)))
        return;

    int idx = tagcache_find_path(path);
    if (!tagcache_entry_string(idx, tag_title, cached_title,
                              sizeof(cached_title)))
    {
        const char *base = strrchr(path, '/');
        strlcpy(cached_title, base ? base + 1 : path, sizeof(cached_title));
    }
    tagcache_entry_string(idx, tag_artist, cached_artist, sizeof(cached_artist));
}

struct queue_screen {
    char save_path[MAX_PATH];
    char save_name[MAX_PATH];
    int context_row;
    int clear_result;
};

static bool next_save(struct queue_screen *screen)
{
    char dir[MAX_PATH];
    char filename[MAX_PATH];
    catalog_get_directory(dir, sizeof(dir));
    for (int i = 1; i <= 9999; i++)
    {
        /* The persisted name has to stay stable across UI languages. */
        snprintf(filename, sizeof(filename), "New Playlist %d.m3u8", i);
        size_t len = path_append(screen->save_path, dir, filename,
                                 sizeof(screen->save_path));
        if (len >= sizeof(screen->save_path))
            break;
        if (!file_exists(screen->save_path) && !dir_exists(screen->save_path))
        {
            snprintf(screen->save_name, sizeof(screen->save_name),
                     str(LANG_FRUIT_OTG_NEW_PLAYLIST), i);
            return true;
        }
    }
    return false;
}

static const char *queue_name(int item, void *data, char *buf, size_t size)
{
    (void)data;
    (void)buf;
    (void)size;
    if (item < 2)
        return str(item == 0 ? LANG_FRUIT_OTG_SAVE : LANG_FRUIT_OTG_CLEAR);
    queue_tags(item - 2);
    return cached_title;
}

static const char *queue_subtitle(int item, void *data, char *buf, size_t size)
{
    struct queue_screen *screen = data;
    if (item == 0)
        return screen->save_name;
    if (item == 1)
        return fruit_otg_count_text(buf, size, count, true);
    queue_tags(item - 2);
    return cached_artist;
}

static int play_queue(int row)
{
    audio_stop();
    if (playlist_create(NULL, NULL) < 0 ||
        playlist_insert_playlist(NULL, OTG_PATH, PLAYLIST_INSERT_LAST, false) < 0 ||
        row >= playlist_amount())
    {
        splash(HZ, ID2P(LANG_FAILED));
        return GO_TO_PREVIOUS;
    }

    if (global_settings.shuffle_mode)
        row = playlist_shuffle(current_tick, row);
    playlist_start(row, 0, 0);
    return GO_TO_WPS;
}

/* Filter the context-dependent navigation bindings on these lists. The
 * stock transport router handles playback buttons before navigation. */
int fruit_list_action(int action, struct gui_synclist *list)
{
    (void)list;
    int button;
    get_action_statuscode(&button);
    if (!IS_SYSEVENT(action) &&
        (button & (BUTTON_LEFT | BUTTON_RIGHT | BUTTON_PLAY)))
        return ACTION_NONE;
    return action;
}

static void clear_measure(struct dialog *dialog, struct screen *screen,
                          struct viewport *box, void *data)
{
    (void)dialog;
    (void)data;
    box->x = 18;
    box->width = screen->lcdwidth - 36;
    box->height = 46;
    box->y = screen->lcdheight - box->height - 16;
}

static void clear_draw(struct dialog *dialog, struct screen *screen,
                       struct viewport *content, void *data)
{
    bool cancel = *(bool *)data;
    int width = MIN(72, content->width / 2);
    int x = (content->width - 2 * width) / 2;
    int y = (content->height - 24) / 2;
    dialog_draw_button(screen, &dialog->style, x, y, width, 24,
                       str(LANG_FRUIT_OTG_CONFIRM_CLEAR), !cancel);
    dialog_draw_button(screen, &dialog->style, x + width, y, width, 24,
                       str(LANG_CANCEL_0), cancel);
}

static int clear_action(struct dialog *dialog, int action, void *data)
{
    bool *cancel = data;
    action = fruit_list_action(action, NULL);
    switch (action)
    {
        case ACTION_STD_PREV:
        case ACTION_STD_PREVREPEAT:
            *cancel = false;
            break;
        case ACTION_STD_NEXT:
        case ACTION_STD_NEXTREPEAT:
            *cancel = true;
            break;
        case ACTION_STD_CANCEL:
            return DIALOG_CANCEL;
        case ACTION_STD_OK:
            if (dialog->backlight_on)
                return *cancel ? DIALOG_CANCEL : DIALOG_ACCEPT;
            break;
        default:
            if (default_event_handler(action) == SYS_USB_CONNECTED)
                return DIALOG_ABORT;
            break;
    }
    return DIALOG_CONTINUE;
}

static int confirm_clear(void)
{
    static const struct dialog_callbacks callbacks = {
        .measure = clear_measure, .draw = clear_draw, .on_action = clear_action,
    };
    struct dialog_style style;
    struct dialog dialog;
    bool cancel = false;
    dialog_style_default(&style);
    style.box_bg = LCD_RGBPACK(35, 37, 37);
    style.box_border_color = LCD_RGBPACK(117, 117, 117);
    style.button_fg = LCD_WHITE;
    style.button_bg = LCD_RGBPACK(40, 42, 42);
    dialog_init(&dialog, CONTEXT_STD, NULL, &style, &callbacks, &cancel);
    dialog.motion_route = "otg-clear";
    return dialog_run(&dialog, HZ / 2);
}

static int queue_action(int action, struct gui_synclist *list)
{
    action = fruit_list_action(action, list);
    struct queue_screen *screen = list->data;
    if (action == ACTION_STD_OK && list->selected_item == 1)
    {
        screen->clear_result = confirm_clear();
        return screen->clear_result == DIALOG_CANCEL ? ACTION_REDRAW :
                                                        ACTION_STD_CANCEL;
    }
    if (action == ACTION_STD_CONTEXT && list->selected_item >= 2)
    {
        screen->context_row = list->selected_item;
        /* Exit the retained list before opening a nested browser. */
        return ACTION_STD_CANCEL;
    }
    return action;
}

static int fruit_info(int title, int message, int art, int y, bool dark)
{
    static struct ui_theme theme;
    struct native_chrome chrome;
    struct native_chrome_indicators indicators;
    struct ui_rect screen = {0, 0, LCD_WIDTH, LCD_HEIGHT};
    struct ui_rect bar = {0, 0, LCD_WIDTH, 20};
    struct ui_view_frame frame = {.bounds = bar, .clip = bar};
    int font = ui_theme_current_font_load(dark ? UI_FONT_LIST_VALUE :
                                          UI_FONT_BODY,
                                          global_settings.glyphs_to_cache);
    bool back = action_set_menu_back(true);
    ui_theme_snapshot(&theme);
    native_chrome_open(&chrome, &theme);

    bool redraw = true;
    int result = GO_TO_PREVIOUS;
    long end = current_tick + HZ * 2;
    while (!dark || TIME_BEFORE(current_tick, end))
    {
        if (native_chrome_poll(&chrome) || redraw)
        {
            struct viewport *old = lcd_set_viewport(NULL);
            lcd_set_background(dark ? LCD_RGBPACK(32, 35, 32) : LCD_WHITE);
            lcd_clear_viewport();
            lcd_set_foreground(dark ? LCD_WHITE : LCD_RGBPACK(64, 64, 64));
            lcd_setfont(font >= 0 ? font : FONT_UI);

            if (dark)
                extras_bar_draw(str(title));
            else
            {
                native_chrome_indicators(&chrome.state, &indicators);
                native_chrome_draw(&chrome, str(title), false, &frame,
                                   NULL, &indicators, NULL);
            }
            if (art >= 0)
                stock_art_draw(art, (LCD_WIDTH - stock_art[art].width) / 2,
                               art == STOCK_ART_ONTHEGO_HELP ? 44 : 34, screen);
            int pad = art == STOCK_ART_ONTHEGO_HELP ? 20 : 16;
            text_wrap_draw(&screens[SCREEN_MAIN], str(message), pad, y,
                           LCD_WIDTH - 2 * pad, dark ? 'c' : 'l');
            lcd_set_viewport(old);
            lcd_update();
        }

        int action = fruit_list_action(get_action(CONTEXT_STD, HZ / 5), NULL);
        if (action == ACTION_STD_CANCEL || (dark && action == ACTION_STD_OK))
            break;
        if (default_event_handler(action) == SYS_USB_CONNECTED)
        {
            result = GO_TO_ROOT;
            break;
        }
        redraw = action == ACTION_REDRAW;
    }

    native_chrome_close(&chrome);
    if (dark)
        extras_bar_release();
    if (font >= 0)
        font_unload(font);
    action_set_menu_back(back);
    return result;
}

int fruit_otg_show(char *saved_path, size_t size, int *track_selection)
{
    struct queue_screen screen;
    struct simplelist_info list;
    int result = GO_TO_PREVIOUS;

    fruit_otg_refresh();
    if (!count)
        return fruit_info(LANG_FRUIT_OTG, LANG_FRUIT_OTG_HELP,
                          STOCK_ART_ONTHEGO_HELP, 129, false);

    int selected = MIN(*track_selection, count + 1);
    *track_selection = 0;
    bool back = action_set_menu_back(true);
    while (count > 0)
    {
        bool can_save = next_save(&screen);
        if (!can_save)
            screen.save_name[0] = '\0';
        screen.context_row = -1;
        screen.clear_result = DIALOG_CANCEL;
        cached_item = -1;
        simplelist_info_init(&list, str(LANG_FRUIT_OTG), count + 2, &screen);
        list.get_name = queue_name;
        list.get_subtitle = queue_subtitle;
        list.action_callback = queue_action;
        list.row_height = 44;
        list.selection = selected;
        if (simplelist_show_list(&list))
        {
            result = GO_TO_ROOT;
            break;
        }
        if (screen.context_row >= 2)
        {
            char path[MAX_PATH + 1];
            selected = screen.context_row;
            if (queue_path(selected - 2, path, sizeof(path)))
            {
                queue_tags(selected - 2);
                int ret = context_menu_show_fruit_track(path, cached_title);
                if (ret == ONPLAY_MAINMENU || ret == ONPLAY_START_PLAY)
                {
                    result = ret == ONPLAY_START_PLAY ? GO_TO_WPS : GO_TO_ROOT;
                    break;
                }
            }
            continue;
        }
        selected = list.selection;
        if (selected < 0)
            break;
        if (selected >= 2)
        {
            result = play_queue(selected - 2);
            if (result == GO_TO_WPS)
                *track_selection = selected;
            break;
        }

        if (selected == 0)
        {
            if (can_save && strlen(screen.save_path) < size &&
                !file_exists(screen.save_path) &&
                rename(OTG_PATH, screen.save_path) == 0)
            {
                strlcpy(saved_path, screen.save_path, size);
                fruit_otg_refresh();
                break;
            }
            splash(HZ, ID2P(LANG_FAILED));
            continue;
        }

        if (screen.clear_result == DIALOG_ABORT)
        {
            result = GO_TO_ROOT;
            break;
        }
        if (screen.clear_result != DIALOG_ACCEPT)
            continue;
        if (remove(OTG_PATH) < 0)
            splash(HZ, ID2P(LANG_FAILED));
        fruit_otg_refresh();
    }

    action_set_menu_back(back);
    return result;
}

int fruit_genius_show(void)
{
    return fruit_info(LANG_FRUIT_GENIUS, LANG_FRUIT_GENIUS_HELP,
                      STOCK_ART_GENIUS_HELP, 158, false);
}

int fruit_genius_unavailable(void)
{
    return fruit_info(LANG_FRUIT_GENIUS, LANG_FRUIT_GENIUS_UNAVAILABLE, -1,
                      128, true);
}
