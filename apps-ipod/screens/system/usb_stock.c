/***************************************************************************
 * PodBox: the stock look's USB screen. GPL version 2+.
 *
 * Under style.art the disk-mode screen renders the stock iPod Classic
 * connection states over Background_LightBlue_Image with the black Extras
 * status bar:
 *  - Connected: orange sphere + dock connector, "Connected" /
 *    "Eject before disconnecting."
 *  - Synchronizing (active disk I/O): orange sphere + rotating white sync
 *    arrows, "Do not disconnect." / "Eject before disconnecting."
 *  - Ejected: green sphere + white checkmark, "OK to disconnect."
 *
 * All glyphs and stock art are loaded into RAM before font_disable_all()
 * so live redraws during a USB session never touch storage.
 * Everything is composed at 8 bits a channel in bands and truncated once.
 ****************************************************************************/

#include <stdlib.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "button.h"
#include "lcd.h"
#include "led.h"
#include "font.h"
#include "lang.h"
#include "power.h"
#include "powermgmt.h"
#include "storage.h"
#include "usb.h"
#include "settings/settings.h"
#include "ui/native_theme_runtime.h"
#include "ui/stock_art.h"
#include "screens/extras/extras_bar.h"
#include "usb_stock.h"

#define ART_X       100     /* the pictures' box, centred in it */
#define ART_W       120
#define SPHERE_Y    46
#define SPHERE_CX   160
#define SPHERE_CY   106
#define TEXT_X      10      /* the text's box */
#define TEXT_W      300
#define TITLE_Y     174
#define DETAIL_Y    196
#define TEXT_RGB    0xffffff
#define BAND_ROWS   4

enum usb_stock_state {
    USB_STOCK_CONNECTED = 0,
    USB_STOCK_SYNCING,
    USB_STOCK_EJECTED,
};

static int title_font = -1, detail_font = -1;
static unsigned last_stamp;
static unsigned sync_phase;

static bool stock_look(void)
{
    return ui_theme_style(UI_STYLE_ART) == UI_ART_PLASTIC;
}

static enum usb_stock_state current_state(void)
{
    if (usb_storage_is_ejected())
        return USB_STOCK_EJECTED;
    if (led_read(HZ / 2) || storage_disk_is_active())
        return USB_STOCK_SYNCING;
    return USB_STOCK_CONNECTED;
}

static unsigned screen_stamp(enum usb_stock_state st)
{
    return (unsigned)st |
           ((unsigned)(battery_level() & 0xff) << 4) |
           ((unsigned)(charge_state & 0x0f) << 12) |
           ((unsigned)(button_hold() ? 1 : 0) << 16) |
           ((unsigned)(charger_inserted() ? 1 : 0) << 17) |
           ((st == USB_STOCK_SYNCING ? (sync_phase & 3) : 0) << 18);
}

static void precache_text(int font, const char *text)
{
    static uint32_t scratch[1];
    struct stock_canvas dummy = { scratch, 0, -100, 1, 1 };
    if (font >= 0 && text && *text)
        stock_canvas_text(&dummy, font, text, 0, 0, TEXT_RGB);
}

void usb_stock_open(void)
{
    if (!stock_look())
        return;
    int glyphs = global_settings.glyphs_to_cache;
    title_font = ui_theme_current_font_load(UI_FONT_WPS_TITLE, glyphs);
    detail_font = ui_theme_current_font_load(UI_FONT_BODY, glyphs);
    stock_art_ready();
    /* Pre-cache every glyph while storage is still ours, before
     * font_disable_all() closes font file descriptors. */
    precache_text(title_font, str(LANG_USB_STOCK_CONNECTED));
    precache_text(title_font, str(LANG_USB_STOCK_DO_NOT_DISCONNECT));
    precache_text(title_font, str(LANG_USB_STOCK_OK_TO_DISCONNECT));
    precache_text(detail_font, str(LANG_USB_STOCK_EJECT_BEFORE_DISCONNECT));
    static uint32_t bar_scratch[1];
    struct stock_canvas bar_dummy = { bar_scratch, 0, 0, 1, 1 };
    extras_bar_put(&bar_dummy, str(LANG_ROCKBOX_TITLE));
    sync_phase = 0;
    last_stamp = 0;
}

void usb_stock_close(void)
{
    if (title_font >= 0)
        font_unload(title_font);
    if (detail_font >= 0)
        font_unload(detail_font);
    if (title_font >= 0 || detail_font >= 0)
        extras_bar_release();
    title_font = detail_font = -1;
}

static void put_centred(struct stock_canvas *band, enum stock_art_id id, int y)
{
    stock_art_put(band, id, ART_X + (ART_W - stock_art[id].width) / 2, y);
}

/* Recolour the orange sphere bitmap to stock's emerald green for the
 * "OK to disconnect." state, keeping its exact gloss and drop shadow. */
static void put_green_sphere(struct stock_canvas *band, int y)
{
    const struct stock_art *art = &stock_art[STOCK_ART_DISKMODEIMAGE_SYNCICON];
    int x0 = ART_X + (ART_W - art->width) / 2;
    int ry0 = MAX(band->y, y + art->ink_y);
    int ry1 = MIN(band->y + band->height, y + art->ink_y + art->ink_h);
    int rx0 = MAX(band->x, x0 + art->ink_x);
    int rx1 = MIN(band->x + band->width, x0 + art->ink_x + art->ink_w);

    for (int py = ry0; py < ry1; py++)
    {
        int sy = py - y - art->ink_y;
        for (int px = rx0; px < rx1; px++)
        {
            int sx = px - x0 - art->ink_x;
            int idx = sy * art->ink_w + sx;
            unsigned n = art->colors <= 256 ?
                ((const uint8_t *)art->pixels)[idx] :
                ((const uint16_t *)art->pixels)[idx];
            uint32_t t = art->palette[n];
            unsigned a = t >> 24;
            if (!a)
                continue;
            unsigned r = (t >> 16) & 255;
            unsigned g = (t >> 8) & 255;
            unsigned b = t & 255;
            /* Swap warm orange hue (high R, mid G, low B) to fresh green. */
            unsigned nr = (g * 7 + b * 3) / 10;
            unsigned ng = MIN(255u, (r * 9 + g * 2) / 10);
            unsigned nb = (b * 8 + g * 2) / 10;
            stock_canvas_blend(band, px, py, (nr << 16) | (ng << 8) | nb, a);
        }
    }
}

/* Thick line segment helper for the sync arrows and eject checkmark. */
static void put_thick_line(struct stock_canvas *band, int x0, int y0,
                           int x1, int y1, int radius, uint32_t rgb)
{
    int min_x = MIN(x0, x1) - radius;
    int max_x = MAX(x0, x1) + radius;
    int min_y = MAX(band->y, MIN(y0, y1) - radius);
    int max_y = MIN(band->y + band->height - 1, MAX(y0, y1) + radius);
    int dx = x1 - x0, dy = y1 - y0;
    int len2 = dx * dx + dy * dy;
    int r2 = radius * radius;
    int r2_outer = (radius + 1) * (radius + 1);

    for (int y = min_y; y <= max_y; y++)
    {
        for (int x = min_x; x <= max_x; x++)
        {
            int px = x - x0, py = y - y0;
            int t_num = len2 ? (px * dx + py * dy) : 0;
            int cx, cy;
            if (t_num <= 0)
                cx = x0, cy = y0;
            else if (t_num >= len2)
                cx = x1, cy = y1;
            else
                cx = x0 + (dx * t_num) / len2, cy = y0 + (dy * t_num) / len2;
            int d2 = (x - cx) * (x - cx) + (y - cy) * (y - cy);
            if (d2 <= r2)
                stock_canvas_blend(band, x, y, rgb, 245);
            else if (d2 <= r2_outer)
                stock_canvas_blend(band, x, y, rgb, 120);
        }
    }
}

/* Rotating circular sync arrows drawn over the orange sphere during I/O. */
static void put_sync_arrows(struct stock_canvas *band, unsigned phase)
{
    const int r_in = 19, r_out = 27;
    int y0 = MAX(band->y, SPHERE_CY - r_out - 6);
    int y1 = MIN(band->y + band->height, SPHERE_CY + r_out + 7);
    int rot = (int)(phase & 3);

    for (int y = y0; y < y1; y++)
    {
        int dy = y - SPHERE_CY;
        for (int x = SPHERE_CX - r_out - 6; x <= SPHERE_CX + r_out + 6; x++)
        {
            int dx = x - SPHERE_CX;
            int d2 = dx * dx + dy * dy;
            if (d2 < r_in * r_in || d2 > r_out * r_out)
                continue;
            /* Two opposing 120-degree arcs rotating by 45 degrees per step. */
            int rx = dx, ry = dy;
            if (rot == 1)
                rx = dx - dy, ry = dx + dy;
            else if (rot == 2)
                rx = -dy, ry = dx;
            else if (rot == 3)
                rx = -dx - dy, ry = dx - dy;
            if (abs(ry) * 3 < abs(rx))
                continue;
            stock_canvas_blend(band, x, y, 0x1a140c, 215);
        }
    }
    /* Arrowheads at the leading ends of the two arcs. */
    static const int8_t heads[4][4][2] = {
        { { 20, -10 }, { 12, -16 }, { -20, 10 }, { -12, 16 } },
        { { 16, 15 }, { 19, 5 }, { -16, -15 }, { -19, -5 } },
        { { -10, 20 }, { -16, 12 }, { 10, -20 }, { 16, -12 } },
        { { -15, -16 }, { -5, -19 }, { 15, 16 }, { 5, 19 } },
    };
    put_thick_line(band, SPHERE_CX + heads[rot][0][0],
                   SPHERE_CY + heads[rot][0][1],
                   SPHERE_CX + heads[rot][1][0],
                   SPHERE_CY + heads[rot][1][1], 3, 0x1a140c);
    put_thick_line(band, SPHERE_CX + heads[rot][2][0],
                   SPHERE_CY + heads[rot][2][1],
                   SPHERE_CX + heads[rot][3][0],
                   SPHERE_CY + heads[rot][3][1], 3, 0x1a140c);
}

/* Bold white checkmark centered in the green "OK to disconnect" sphere. */
static void put_checkmark(struct stock_canvas *band)
{
    put_thick_line(band, SPHERE_CX - 22, SPHERE_CY + 2,
                   SPHERE_CX - 6, SPHERE_CY + 18, 5, 0xffffff);
    put_thick_line(band, SPHERE_CX - 6, SPHERE_CY + 18,
                   SPHERE_CX + 24, SPHERE_CY - 16, 5, 0xffffff);
}

static void put_text_line(struct stock_canvas *band, int font,
                          const char *text, int y)
{
    if (!text || !*text)
        return;
    if (y >= band->y + band->height ||
        y + (int)font_get(font)->height <= band->y)
        return;
    int width;
    font_getstringsize((const unsigned char *)text, &width, NULL, font);
    stock_canvas_text(band, font, text, TEXT_X + (TEXT_W - width) / 2, y,
                      TEXT_RGB);
}

static bool draw_state(enum usb_stock_state st)
{
    if (title_font < 0 || detail_font < 0 || !stock_art_ready())
        return false;

    const char *title = (const char *)str(st == USB_STOCK_EJECTED ?
                            LANG_USB_STOCK_OK_TO_DISCONNECT :
                            st == USB_STOCK_SYNCING ?
                            LANG_USB_STOCK_DO_NOT_DISCONNECT :
                            LANG_USB_STOCK_CONNECTED);
    const char *detail = st == USB_STOCK_EJECTED ? "" :
        (const char *)str(LANG_USB_STOCK_EJECT_BEFORE_DISCONNECT);

    static uint32_t band_pixels[LCD_WIDTH * BAND_ROWS];
    for (int y = 0; y < LCD_HEIGHT; y += BAND_ROWS)
    {
        struct stock_canvas band = {
            band_pixels, 0, y, LCD_WIDTH, MIN(BAND_ROWS, LCD_HEIGHT - y),
        };
        stock_art_put(&band, STOCK_ART_BACKGROUND_LIGHTBLUE, 0, 0);
        if (st == USB_STOCK_EJECTED)
        {
            put_green_sphere(&band, SPHERE_Y);
            put_checkmark(&band);
        }
        else
        {
            put_centred(&band, STOCK_ART_DISKMODEIMAGE_SYNCICON, SPHERE_Y);
            if (st == USB_STOCK_SYNCING)
                put_sync_arrows(&band, sync_phase);
            else
                put_centred(&band, STOCK_ART_DISKMODEIMAGE_CONNECTEDICON,
                            SPHERE_Y);
        }
        put_text_line(&band, title_font, title, TITLE_Y);
        put_text_line(&band, detail_font, detail, DETAIL_Y);
        extras_bar_put(&band, str(LANG_ROCKBOX_TITLE));
        stock_canvas_show(&band, (struct ui_rect){ 0, y, LCD_WIDTH,
                                                   band.height });
        /* The host may be waiting on SET_ADDRESS (usb_screen.c). */
        yield();
    }
    lcd_update();
    last_stamp = screen_stamp(st);
    return true;
}

bool usb_stock_draw(void)
{
    return draw_state(current_state());
}

void usb_stock_tick(void)
{
    if (title_font < 0 || detail_font < 0 || !stock_art_ready())
        return;
    enum usb_stock_state st = current_state();
    if (st == USB_STOCK_SYNCING)
        sync_phase++;
    unsigned stamp = screen_stamp(st);
    if (stamp != last_stamp)
        draw_state(st);
}
