/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The stock black status bar of the full-screen Extras pages.
 ****************************************************************************/
#include "config.h"
#include "core_alloc.h"
#include "system.h"
#include "audio.h"
#include "button.h"
#include "font.h"
#include "lcd.h"
#include "power.h"
#include "powermgmt.h"
#include "rbpaths.h"
#include "audio/play_status.h"
#include "screens/fruit_menu.h"
#include "input/action.h"
#include "screens/playback/wps.h"
#include "settings/settings.h"
#include "ui/frame.h"
#include "ui/theme_art.h"
#include "widgets/list.h"
#include "extras_bar.h"

/* 20 KiB the pages hold only while one is open: .bss has no room for it */
#define BAND_PIXELS (LCD_WIDTH * EXTRAS_BAND_ROWS)
static int band_handle;
static uint32_t *band_pixels;

uint32_t *extras_band(int i)
{
    if (!band_pixels)
    {
        band_handle = core_alloc(2 * BAND_PIXELS * sizeof(uint32_t));
        if (band_handle <= 0)
            return NULL;
        band_pixels = core_get_data_pinned(band_handle);
    }
    return band_pixels + i * BAND_PIXELS;
}

#define TITLE_FONT FONT_DIR "/13-helvetica-bold.pbf"
#define TITLE_BASELINE 15
#define BATTERY_X 289
#define BATTERY_Y 4
#define PLAY_X 271
#define PLAY_Y 5
#define HOLD_X 7
#define HOLD_Y 4
#define TITLE_GREY 0xd2d2d2 /* stock's title is not quite white */

/* Stock lays the bar more opaque than its bitmap's alpha: each row
 * composes with this alpha instead, measured on the 1.1.2 pages. */
static const uint8_t row_alpha[EXTRAS_BAR_HEIGHT] = {
    221, 220, 221, 221, 224, 225, 222, 225, 226, 227, 227, 226,
    226, 227, 228, 230, 230, 231, 233, 231, 233, 235, 68, 23,
};

static int title_font = -1;

static void put_background(struct stock_canvas *canvas)
{
    const struct stock_art *art =
        &stock_art[STOCK_ART_STATUSBARBLACK_BACKGROUND];
    int y1 = MIN(canvas->y + canvas->height, EXTRAS_BAR_HEIGHT);
    int x1 = MIN(canvas->x + canvas->width, (int)art->width);
    for (int y = MAX(canvas->y, 0); y < y1; y++)
        for (int x = MAX(canvas->x, 0); x < x1; x++)
        {
            int i = y * art->width + x;
            unsigned n = art->colors <= 256 ?
                ((const uint8_t *)art->pixels)[i] :
                ((const uint16_t *)art->pixels)[i];
            stock_canvas_blend(canvas, x, y, art->palette[n] & 0xffffff,
                               row_alpha[y]);
        }
}

/* The black status bar uses the same battery frame as the white bar. */
static void put_battery(struct stock_canvas *canvas)
{
    int level = battery_level();
    if (level < 0)
        return;
    enum stock_art_id id = STOCK_ART_STATUSBARBLACK_BATTERY_IMAGE_0 +
        ui_art_battery_index(level);
    stock_art_put(canvas, id, BATTERY_X, BATTERY_Y);
    if (charge_state == CHARGING || charge_state == TOPOFF)
        stock_art_put(canvas, STOCK_ART_STATUSBARBLACK_BATTERY_IMAGE_CHARGE,
                      BATTERY_X, BATTERY_Y);
#if CONFIG_CHARGING
    else if (charger_inserted())
        stock_art_put(canvas, STOCK_ART_STATUSBARBLACK_BATTERY_IMAGE_PLUG,
                      BATTERY_X, BATTERY_Y);
#endif
}

/* White play triangle or pause bars in stock's black bar slot (x 271..281). */
static void put_play(struct stock_canvas *canvas)
{
    int mode = current_playmode();
    if (mode == STATUS_PLAY || mode == STATUS_FASTFORWARD ||
        mode == STATUS_FASTBACKWARD)
    {
        for (int r = 0; r < 10; r++)
        {
            int w = r < 5 ? (r * 2 + 2) : ((9 - r) * 2 + 2);
            if (w > 11)
                w = 11;
            for (int c = 0; c < w; c++)
                stock_canvas_blend(canvas, PLAY_X + c, PLAY_Y + r,
                                   TITLE_GREY, 255);
        }
    }
    else if (mode == STATUS_PAUSE)
    {
        for (int r = 0; r < 10; r++)
        {
            for (int c = 0; c < 3; c++)
            {
                stock_canvas_blend(canvas, PLAY_X + 2 + c, PLAY_Y + r,
                                   TITLE_GREY, 255);
                stock_canvas_blend(canvas, PLAY_X + 7 + c, PLAY_Y + r,
                                   TITLE_GREY, 255);
            }
        }
    }
}

/* Miniature padlock for Hold in the black status bar. */
static void put_hold(struct stock_canvas *canvas)
{
    static const uint16_t rows[12] = {
        0x078, /* ...####... */
        0x0cc, /* ..##..##.. */
        0x084, /* ..#....#.. */
        0x084, /* ..#....#.. */
        0x084, /* ..#....#.. */
        0x3ff, /* ########## */
        0x3ff, /* ########## */
        0x3ff, /* ########## */
        0x3ff, /* ########## */
        0x3ff, /* ########## */
        0x3ff, /* ########## */
        0x1fe, /* .########. */
    };
    if (!fruit_menu_active() || !button_hold())
        return;
    for (int r = 0; r < 12; r++)
        for (int c = 0; c < 10; c++)
            if (rows[r] & (1u << (9 - c)))
                stock_canvas_blend(canvas, HOLD_X + c, HOLD_Y + r,
                                   TITLE_GREY, 240);
}

void extras_bar_put_ex(struct stock_canvas *canvas, const char *title,
                       bool show_battery, bool show_play)
{
    if (canvas->y >= EXTRAS_BAR_HEIGHT || !stock_art_ready())
        return;
    put_background(canvas);
    extras_bar_put_marks(canvas, title, show_battery, show_play);
}

void extras_bar_put_marks(struct stock_canvas *canvas, const char *title,
                          bool show_battery, bool show_play)
{
    if (canvas->y >= EXTRAS_BAR_HEIGHT || !stock_art_ready())
        return;
    put_hold(canvas);
    if (title_font < 0)
        title_font = font_load_ex(TITLE_FONT, 0,
                                  global_settings.glyphs_to_cache);
    if (title_font >= 0 && title && *title)
    {
        int width;
        font_getstringsize((const unsigned char *)title, &width, NULL,
                           title_font);
        stock_canvas_text(canvas, title_font, title,
                          (LCD_WIDTH - width + 1) / 2,
                          TITLE_BASELINE - font_get(title_font)->ascent,
                          TITLE_GREY);
    }
    if (show_play)
        put_play(canvas);
    if (show_battery)
        put_battery(canvas);
}

void extras_bar_put(struct stock_canvas *canvas, const char *title)
{
    extras_bar_put_ex(canvas, title, true, false);
}

void extras_bar_draw(const char *title)
{
    enum { ROWS = 4 };
    uint32_t pixels[LCD_WIDTH * ROWS];
    for (int y = 0; y < EXTRAS_BAR_HEIGHT; y += ROWS)
    {
        struct stock_canvas band = { pixels, 0, y, LCD_WIDTH, ROWS };
        stock_art_put(&band, STOCK_ART_BACKGROUND_LIGHTBLUE, 0, 0);
        extras_bar_put(&band, title);
        stock_canvas_show(&band, (struct ui_rect){ 0, y, LCD_WIDTH, ROWS });
    }
}

void extras_bar_release(void)
{
    if (band_pixels)
    {
        core_put_data_pinned(band_pixels);
        band_handle = core_free(band_handle);
        band_pixels = NULL;
    }
    if (title_font >= 0)
        font_unload(title_font);
    title_font = -1;
}

/* Common Extras controls. Stopwatch handles PLAY in its own event loop. */
static const struct button_mapping extras_nav_map[] = {
    { ACTION_STD_PREV,       BUTTON_SCROLL_BACK,                BUTTON_NONE },
    { ACTION_STD_PREVREPEAT, BUTTON_SCROLL_BACK|BUTTON_REPEAT,  BUTTON_NONE },
    { ACTION_STD_NEXT,       BUTTON_SCROLL_FWD,                 BUTTON_NONE },
    { ACTION_STD_NEXTREPEAT, BUTTON_SCROLL_FWD|BUTTON_REPEAT,   BUTTON_NONE },
    { ACTION_STD_OK,         BUTTON_SELECT|BUTTON_REL,          BUTTON_SELECT },
    { ACTION_STD_CONTEXT,    BUTTON_SELECT|BUTTON_REPEAT,       BUTTON_SELECT },
    { ACTION_NONE,           BUTTON_SELECT|BUTTON_REPEAT,       BUTTON_NONE },
    { ACTION_STD_CANCEL,     BUTTON_MENU|BUTTON_REL,            BUTTON_MENU },
    { ACTION_NONE,           BUTTON_MENU|BUTTON_REPEAT,         BUTTON_NONE },
    { ACTION_NONE,           BUTTON_LEFT|BUTTON_REPEAT,         BUTTON_NONE },
    { ACTION_NONE,           BUTTON_RIGHT|BUTTON_REPEAT,        BUTTON_NONE },
    { ACTION_WPS_PLAY,       BUTTON_PLAY|BUTTON_REL,            BUTTON_PLAY },
    { ACTION_NONE,           BUTTON_PLAY|BUTTON_REPEAT,         BUTTON_NONE },
    LAST_ITEM_IN_LIST
};

static const struct button_mapping extras_locked_map[] = {
    { ACTION_STD_OK,         BUTTON_SCROLL_BACK,                BUTTON_NONE },
    { ACTION_STD_OK,         BUTTON_SCROLL_FWD,                 BUTTON_NONE },
    { ACTION_STD_OK,         BUTTON_SELECT|BUTTON_REL,          BUTTON_SELECT },
    { ACTION_NONE,           BUTTON_SELECT|BUTTON_REPEAT,       BUTTON_NONE },
    { ACTION_NONE,           BUTTON_MENU|BUTTON_REL,            BUTTON_NONE },
    { ACTION_NONE,           BUTTON_MENU|BUTTON_REPEAT,         BUTTON_NONE },
    { ACTION_NONE,           BUTTON_LEFT|BUTTON_REL,            BUTTON_NONE },
    { ACTION_NONE,           BUTTON_LEFT|BUTTON_REPEAT,         BUTTON_NONE },
    { ACTION_NONE,           BUTTON_RIGHT|BUTTON_REL,           BUTTON_NONE },
    { ACTION_NONE,           BUTTON_RIGHT|BUTTON_REPEAT,        BUTTON_NONE },
    { ACTION_NONE,           BUTTON_PLAY|BUTTON_REL,            BUTTON_NONE },
    { ACTION_NONE,           BUTTON_PLAY|BUTTON_REPEAT,         BUTTON_NONE },
    LAST_ITEM_IN_LIST
};

static const struct button_mapping *get_extras_nav_map(int context)
{
#if BUTTON_REMOTE != 0
    if (context & CONTEXT_REMOTE)
        return get_context_mapping(CONTEXT_STD | CONTEXT_REMOTE);
#else
    (void)context;
#endif
    return extras_nav_map;
}

static const struct button_mapping *get_extras_locked_map(int context)
{
    (void)context;
    return extras_locked_map;
}

int extras_get_action(int timeout, bool local_play)
{
    int action = get_custom_action(CONTEXT_PLUGIN | CONTEXT_CUSTOM, timeout,
                                   get_extras_nav_map);
    if (action == ACTION_WPS_PLAY && !local_play)
        return ACTION_NONE;
    return action;
}

int extras_get_action_locked(int timeout)
{
    return get_custom_action(CONTEXT_PLUGIN | CONTEXT_CUSTOM2, timeout,
                             get_extras_locked_map);
}

bool extras_list_do_action(int timeout, struct gui_synclist *lists,
                           int *action)
{
    timeout = list_do_action_timeout(lists, timeout);
    bool inhibited = ui_flush_inhibited();
    ui_inhibit_flush(true);
    *action = extras_get_action(timeout, false);
    ui_inhibit_flush(inhibited);
    bool handled = gui_synclist_do_button(lists, action);
    if (!inhibited)
        ui_flush_dirty();
    if (*action == ACTION_NONE)
    {
        if (gui_synclist_moving(lists))
            gui_synclist_draw(lists);
        else
            lcd_update();
    }
    return handled;
}
