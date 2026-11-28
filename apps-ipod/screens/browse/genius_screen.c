/* SPDX-License-Identifier: GPL-2.0-or-later
 * The Genius Mixes browser: a paged mosaic on Classic and Fruit, a list
 * on Scrim. Tracks play through the normal queue and Now Playing screen.
 */
#include <string.h>
#include "config.h"
#include "core_alloc.h"
#include "action.h"
#include "button.h"
#include "database/genius_mixes.h"
#include "database/tagcache.h"
#include "draw/text_wrap.h"
#include "file.h"
#include "font.h"
#include "kernel.h"
#include "lang.h"
#include "lcd.h"
#include "metadata/art_cache.h"
#include "screens/playback/wps.h"
#include "playlist/playlist.h"
#include "root_menu.h"
#include "screens.h"
#include "settings/settings.h"
#include "speech/language.h"
#include "system/bg_task.h"
#include "system/shutdown.h"
#include "ui/frame.h"
#include "ui/native_chrome.h"
#include "ui/native_theme_runtime.h"
#include "ui/scene.h"
#include "ui/stock_art.h"
#include "widgets/list.h"
#include "widgets/splash.h"

#define TILE_EDGE 70
#define TILE_PIXELS (TILE_EDGE * TILE_EDGE)
#define MOSAIC_EDGE (TILE_EDGE * 2)
#define MOSAIC_Y 29
#define MOSAIC_X ((LCD_WIDTH - MOSAIC_EDGE) / 2)

static struct genius_catalog catalog;
static int page;

static struct
{
    struct mutex lock;
    int32_t ids[GENIUS_COVERS];
    int32_t commit;
    uint32_t generation;
    unsigned request, drawn;
    unsigned mask;
    int handle;
    bool active;
} artwork;

/* All file reads and resampling stay on the library worker. The render lock
 * covers only bitmap copies and the request, never a disk operation. */
static void load_art(const struct queue_event *ev)
{
    (void)ev;
    int32_t ids[GENIUS_COVERS], commit;
    uint32_t generation;
    mutex_lock(&artwork.lock);
    if (!artwork.active || artwork.drawn == artwork.request)
    {
        mutex_unlock(&artwork.lock);
        return;
    }
    unsigned ticket = artwork.request;
    memcpy(ids, artwork.ids, sizeof(ids));
    commit = artwork.commit;
    generation = artwork.generation;
    mutex_unlock(&artwork.lock);

    unsigned mask = 0;
    int handle = core_alloc_free_space(GENIUS_COVERS * TILE_PIXELS *
                                       sizeof(fb_data));
    if (handle <= 0)
        return;
    fb_data *pixels = core_get_data_pinned(handle);
    struct tagcache_search tcs;
    if (tagcache_search(&tcs, tag_length))
    {
        if (commit == tagcache_commit_id() &&
            generation == tagcache_generation())
        {
            for (int i = 0; i < GENIUS_COVERS; i++)
            {
                char track[MAX_PATH + 1];
                if (ids[i] >= 0 &&
                    tagcache_retrieve(&tcs, ids[i], tag_filename,
                                      track, sizeof(track)) &&
                    art_cache_load_track_tile(track, TILE_EDGE,
                                              pixels + i * TILE_PIXELS))
                    mask |= 1u << i;
            }
        }
        tagcache_search_finish(&tcs);
    }

    core_put_data_pinned(pixels);
    mutex_lock(&artwork.lock);
    if (ticket == artwork.request)
    {
        if (mask)
        {
            artwork.handle = handle;
            handle = 0;
        }
        artwork.mask = mask;
        artwork.drawn = ticket;
    }
    mutex_unlock(&artwork.lock);
    if (handle > 0)
        core_free(handle);
}

static struct bg_task mosaic_task = {.name = "Genius artwork",
                                    .handle_event = load_art};

static void set_art_request(bool active)
{
    mutex_lock(&artwork.lock);
    artwork.request++;
    artwork.active = active;
    int old_handle = artwork.handle;
    artwork.handle = 0;
    artwork.mask = 0;
    if (active)
    {
        memcpy(artwork.ids, catalog.mixes[page].covers, sizeof(artwork.ids));
        artwork.commit = catalog.commit;
        artwork.generation = catalog.generation;
    }
    mutex_unlock(&artwork.lock);
    if (old_handle > 0)
        core_free(old_handle);
    if (active && !bg_task_queued(&mosaic_task, BG_EVENT_PANEL))
        bg_task_post(&mosaic_task, BG_EVENT_PANEL);
}

static int play_mix(void)
{
    struct tagcache_search tcs;
    if (!tagcache_search(&tcs, tag_length))
    {
        splash(HZ, ID2P(LANG_TAGCACHE_BUSY));
        return GO_TO_PREVIOUS;
    }
    if (!genius_mixes_current(&catalog))
    {
        tagcache_search_finish(&tcs);
        return GO_TO_PREVIOUS;
    }

    struct genius_mix *mix = &catalog.mixes[page];
    int tracks[GENIUS_MAX_TRACKS], count = 0;
    char path[MAX_PATH + 1];
    for (int i = 0; i < mix->count; i++)
        if (tagcache_retrieve(&tcs, mix->tracks[i], tag_filename,
                              path, sizeof(path)) && file_exists(path))
            tracks[count++] = mix->tracks[i];

    int added = 0;
    if (count > 0 && playlist_create(NULL, NULL) >= 0)
    {
        struct playlist_insert_context context;
        if (playlist_insert_context_create(NULL, &context, PLAYLIST_INSERT_LAST,
                                           false, false) >= 0)
            for (int i = 0; i < count; i++)
            {
                if (!tagcache_retrieve(&tcs, tracks[i], tag_filename,
                                       path, sizeof(path)) ||
                    playlist_insert_context_add(&context, path) < 0)
                    break;
                added++;
            }
        playlist_insert_context_release(&context);
    }
    tagcache_search_finish(&tcs);
    if (added)
    {
        playlist_start(0, 0, 0);
        return GO_TO_WPS;
    }
    splash(HZ, ID2P(LANG_FAILED));
    return GO_TO_PREVIOUS;
}

static const char *mix_name(int row, void *data, char *buf, size_t size)
{
    (void)data;
    (void)buf;
    (void)size;
    return catalog.mixes[row].name;
}

static int show_list(void)
{
    struct simplelist_info list;
    simplelist_info_init(&list, str(LANG_FRUIT_GENIUS_MIXES),
                         catalog.count, NULL);
    list.get_name = mix_name;
    list.selection = page;
    if (simplelist_show_list(&list))
        return GO_TO_ROOT;
    if (list.selection < 0)
        return GO_TO_PREVIOUS;
    page = list.selection;
    return play_mix();
}

static void draw_tile(int n, const fb_data *pixels)
{
    int x = MOSAIC_X + (n % 2) * TILE_EDGE;
    int y = MOSAIC_Y + (n / 2) * TILE_EDGE;
    if (pixels && (artwork.mask & (1u << n)))
    {
        lcd_bitmap(pixels + n * TILE_PIXELS, x, y, TILE_EDGE, TILE_EDGE);
        return;
    }

    lcd_set_foreground(n == 1 || n == 2 ? LCD_RGBPACK(213, 219, 224)
                                        : LCD_RGBPACK(227, 232, 236));
    lcd_fillrect(x, y, TILE_EDGE, TILE_EDGE);
    lcd_set_foreground(LCD_RGBPACK(151, 161, 170));
    /* Two notes in the missing-cover tile. */
    x += 23;
    y += 25;
    lcd_fillrect(x + 4, y + 14, 8, 5);
    lcd_fillrect(x + 17, y + 12, 8, 5);
    lcd_fillrect(x + 10, y + 1, 2, 14);
    lcd_fillrect(x + 23, y - 1, 2, 14);
    lcd_drawline(x + 10, y + 1, x + 24, y - 1);
    lcd_drawline(x + 10, y + 2, x + 24, y);
}

static void draw_page(const struct native_chrome *chrome, int font,
                      bool tiles, int message)
{
    struct viewport *old = lcd_set_viewport(NULL);
    lcd_set_background(tiles ? LCD_WHITE
                             : ui_theme_rgb565(UI_COLOR_BACKGROUND));
    lcd_clear_viewport();
    const struct ui_rect bar = {0, 0, LCD_WIDTH, 20};
    const struct ui_view_frame frame = {.bounds = bar, .clip = bar};
    struct native_chrome_indicators indicators;
    native_chrome_indicators(&chrome->state, &indicators);
    native_chrome_draw(chrome, str(LANG_FRUIT_GENIUS_MIXES), false, &frame,
                       NULL, &indicators, NULL);
    unsigned fg = tiles ? LCD_RGBPACK(64, 64, 64)
                        : ui_theme_rgb565(UI_COLOR_FOREGROUND);
    lcd_set_foreground(fg);
    lcd_setfont(font >= 0 ? font : FONT_UI);
    if (message)
    {
        int y = 101;
        if (tiles && message == LANG_FRUIT_GENIUS_MIXES_HELP)
        {
            const struct ui_rect screen = {0, 0, LCD_WIDTH, LCD_HEIGHT};
            stock_art_draw(STOCK_ART_GENIUS_HELP,
                           (LCD_WIDTH - stock_art[STOCK_ART_GENIUS_HELP].width)
                           / 2, 34, screen);
            y = 151;
        }
        text_wrap_draw(&screens[SCREEN_MAIN], str(message), 16, y,
                       LCD_WIDTH - 32, 'c');
    }
    else
    {
        lcd_set_foreground(LCD_RGBPACK(183, 187, 192));
        lcd_drawrect(MOSAIC_X - 1, MOSAIC_Y - 1,
                     MOSAIC_EDGE + 2, MOSAIC_EDGE + 2);
        mutex_lock(&artwork.lock);
        const fb_data *pixels = artwork.handle > 0 ?
                                core_get_data_pinned(artwork.handle) : NULL;
        for (int i = 0; i < GENIUS_COVERS; i++)
            draw_tile(i, pixels);
        if (pixels)
            core_put_data_pinned((void *)pixels);
        mutex_unlock(&artwork.lock);
        lcd_set_foreground(fg);
        /* The caption clips independently of the page dots. */
        struct viewport label = **screens[SCREEN_MAIN].current_viewport;
        label.y = MOSAIC_Y + MOSAIC_EDGE + 7;
        label.height = 44;
        struct viewport *prev = lcd_set_viewport(&label);
        text_wrap_draw(&screens[SCREEN_MAIN], catalog.mixes[page].name, 12, 0,
                       LCD_WIDTH - 24, 'c');
        lcd_set_viewport(prev);
        for (int i = 0; i < catalog.count; i++)
        {
            int x = (LCD_WIDTH - (catalog.count - 1) * 10) / 2 + i * 10;
            lcd_set_foreground(i == page ? fg : LCD_RGBPACK(180, 184, 189));
            lcd_hline(x - 1, x + 1, 224);
            lcd_fillrect(x - 2, 225, 5, 3);
            lcd_hline(x - 1, x + 1, 228);
        }
    }
    lcd_set_viewport(old);
}

static const struct button_mapping *genius_keymap(int context)
{
    (void)context;
    static const struct button_mapping keys[] = {
        { ACTION_STD_PREV, BUTTON_LEFT | BUTTON_REL, BUTTON_LEFT },
        { ACTION_STD_NEXT, BUTTON_RIGHT | BUTTON_REL, BUTTON_RIGHT },
        { ACTION_WPS_PLAY, BUTTON_PLAY | BUTTON_REL, BUTTON_PLAY },
        { ACTION_NONE, BUTTON_PLAY | BUTTON_REPEAT, BUTTON_NONE },
        LAST_ITEM_IN_LIST__NEXTLIST(CONTEXT_STD)
    };
    return keys;
}

static int message_for(bool loaded)
{
    if (!loaded)
        return genius_mixes_failed() ? LANG_FAILED : LANG_WAIT;
    return catalog.count > 0 ? 0 : LANG_FRUIT_GENIUS_MIXES_HELP;
}

int genius_screen(void *param)
{
    (void)param;
    static struct ui_theme theme;
    static bool initialized;
    if (!initialized)
    {
        mutex_init(&artwork.lock);
        initialized = true;
    }

    bool tiles = ui_theme_style(UI_STYLE_SELECTION) == UI_SELECTION_BAR;
    bool loaded = genius_mixes_load(&catalog);
    page = loaded && catalog.count ? MIN(page, catalog.count - 1) : 0;
    if (!tiles && loaded && catalog.count)
        return show_list();

    struct native_chrome chrome;
    ui_theme_snapshot(&theme);
    native_chrome_open(&chrome, &theme);
    int font = ui_theme_current_font_load(UI_FONT_BODY,
                                         global_settings.glyphs_to_cache);
    bool back = action_set_menu_back(true);
    bool transport = action_set_wheel_transport(tiles);
    int result = GO_TO_PREVIOUS;
    int message = message_for(loaded);
    if (!message)
        set_art_request(true);
    unsigned displayed = 0;
    struct ui_scene pan = {0};
    long next = current_tick + HZ;
    bool redraw = true;
    while (true)
    {
        if (TIME_AFTER(current_tick, next))
        {
            next = current_tick + HZ;
            if (!loaded || !genius_mixes_current(&catalog))
            {
                int old = message;
                loaded = genius_mixes_load(&catalog);
                message = message_for(loaded);
                if (!message)
                {
                    page = MIN(page, catalog.count - 1);
                    if (!tiles)
                        break;
                    set_art_request(true);
                    redraw = true;
                }
                redraw |= old != message;
            }
        }
        mutex_lock(&artwork.lock);
        unsigned drawn = artwork.drawn;
        mutex_unlock(&artwork.lock);
        bool dirty = native_chrome_poll(&chrome) || redraw ||
                     (!message && drawn != displayed);
        if (dirty)
        {
            draw_page(&chrome, font, tiles, message);
            displayed = drawn;
            redraw = false;
        }
        if (pan.capturing)
            ui_scene_start(&pan);
        else if (pan.handle > 0)
            ui_scene_update(&pan, dirty);
        else if (dirty)
            lcd_update();

        int timeout = ui_scene_timeout(&pan, HZ / 5);
        int action = get_custom_action(CONTEXT_PLUGIN | CONTEXT_LOCAL_LEFT_RIGHT,
                                       timeout, genius_keymap);
        if (action == ACTION_WPS_PLAY)
        {
            wps_wheel_transport(action);
            continue;
        }
        if (action == ACTION_STD_CANCEL || action == ACTION_STD_MENU)
        {
            if (action == ACTION_STD_MENU)
                result = GO_TO_ROOT;
            break;
        }
        if (!message && action == ACTION_STD_OK)
        {
            ui_scene_cancel(&pan);
            result = play_mix();
            if (result != GO_TO_PREVIOUS)
                break;
            redraw = true;
            continue;
        }
        int previous = page;
        if (!message && (action == ACTION_STD_NEXT ||
                         action == ACTION_STD_NEXTREPEAT))
            page = MIN(page + 1, catalog.count - 1);
        if (!message && (action == ACTION_STD_PREV ||
                         action == ACTION_STD_PREVREPEAT))
            page = MAX(page - 1, 0);
        if (previous != page)
        {
            ui_scene_capture_pan(&pan, &catalog.mixes[previous],
                                 &catalog.mixes[page],
                                 page > previous ? 1 : -1, 20);
            set_art_request(true);
            redraw = true;
        }
        if (default_event_handler(action) == SYS_USB_CONNECTED)
        {
            result = GO_TO_ROOT;
            break;
        }
        redraw |= action == ACTION_REDRAW;
    }
    ui_scene_cancel(&pan);
    set_art_request(false);
    native_chrome_close(&chrome);
    if (font >= 0)
        font_unload(font);
    action_set_wheel_transport(transport);
    action_set_menu_back(back);
    if (!tiles && !message && result == GO_TO_PREVIOUS)
        return show_list();
    return result;
}
