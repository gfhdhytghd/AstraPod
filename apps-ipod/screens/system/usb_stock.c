/***************************************************************************
 * PodBox: the stock look's USB screen. GPL version 2+.
 *
 * Under style.art the disk-mode screen renders the stock iPod Classic
 * connection states over Background_LightBlue_Image with the black Extras
 * status bar:
 *  - Connected: orange sphere + dock connector, "Connected" /
 *    "Eject before disconnecting."
 *  - Synchronizing (active disk I/O): orange sphere + rotating black sync
 *    arrows, "Do not disconnect." / "Eject before disconnecting."
 *  - Ejected: green sphere + white checkmark, "OK to disconnect."
 *
 * All glyphs and stock art are loaded into RAM before font_disable_all()
 * so live redraws during a USB session never touch storage.
 * Everything is composed at 8 bits a channel in bands and truncated once.
 ****************************************************************************/

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
#include "usb_ultra.h"

#define ART_X       100     /* the pictures' box, centred in it */
#define ART_W       120
#define SPHERE_Y    46
#define SYNC_Y      62
#define SYNC_FRAMES 18
#define SYNC_STEP   MAX(1, HZ / 20)
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
static long next_sync_frame;
/* Fruit Company Ultra brings the picture in once the host is answered. */
static bool entering;

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
           ((unsigned)(charger_inserted() ? 1 : 0) << 17);
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
    next_sync_frame = current_tick + SYNC_STEP;
    last_stamp = 0;
    const struct stock_art *sync = &stock_art[STOCK_ART_DISKMODEIMAGE_SYNCICON];
    const struct stock_art *plug =
        &stock_art[STOCK_ART_DISKMODEIMAGE_CONNECTEDICON];
    int width = MAX(sync->width, plug->width);
    entering = usb_ultra_open((struct ui_rect){ ART_X + (ART_W - width) / 2,
        SPHERE_Y, width, MAX(sync->height, plug->height) });
}

void usb_stock_close(void)
{
    entering = false;
    usb_ultra_close();
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

/* Thick line segment for the eject checkmark. */
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

static void draw_rows(int top, int bottom, bool picture,
                      enum usb_stock_state st)
{
    const char *title = (const char *)str(st == USB_STOCK_EJECTED ?
                            LANG_USB_STOCK_OK_TO_DISCONNECT :
                            st == USB_STOCK_SYNCING ?
                            LANG_USB_STOCK_DO_NOT_DISCONNECT :
                            LANG_USB_STOCK_CONNECTED);
    const char *detail = st == USB_STOCK_EJECTED ? "" :
        (const char *)str(LANG_USB_STOCK_EJECT_BEFORE_DISCONNECT);

    static uint32_t band_pixels[LCD_WIDTH * BAND_ROWS];
    for (int y = top; y < bottom; y += BAND_ROWS)
    {
        struct stock_canvas band = {
            band_pixels, 0, y, LCD_WIDTH, MIN(BAND_ROWS, bottom - y),
        };
        stock_art_put(&band, STOCK_ART_BACKGROUND_LIGHTBLUE, 0, 0);
        if (picture)
        {
            if (st == USB_STOCK_EJECTED)
            {
                put_green_sphere(&band, SPHERE_Y);
                put_checkmark(&band);
            }
            else
            {
                put_centred(&band, STOCK_ART_DISKMODEIMAGE_SYNCICON, SPHERE_Y);
                if (st == USB_STOCK_SYNCING)
                {
                    /* Stock 2.0.5 provides a 4-bit mask for each pose. */
                    enum stock_art_id id = STOCK_ART_DISKMODEIMAGE_SYNCARROW01
                                         + sync_phase;
                    int x = ART_X + (ART_W - stock_art[id].width) / 2;
                    stock_art_mask(&band, id, x, SYNC_Y, 0);
                }
                else
                    put_centred(&band, STOCK_ART_DISKMODEIMAGE_CONNECTEDICON,
                                SPHERE_Y);
            }
        }
        put_text_line(&band, title_font, title, TITLE_Y);
        put_text_line(&band, detail_font, detail, DETAIL_Y);
        extras_bar_put(&band, str(LANG_ROCKBOX_TITLE));
        stock_canvas_show(&band, (struct ui_rect){ 0, y, LCD_WIDTH,
                                                   band.height });
        /* The host may be waiting on SET_ADDRESS (usb_screen.c). */
        yield();
    }
}

static void draw_picture_rows(int top, int bottom)
{
    draw_rows(top, bottom, true, current_state());
}

bool usb_stock_draw(void)
{
    if (title_font < 0 || detail_font < 0 || !stock_art_ready())
    {
        entering = false;
        usb_ultra_close();
        return false;
    }

    enum usb_stock_state st = current_state();
    draw_rows(0, LCD_HEIGHT, !entering, st);
    if (entering)
        usb_ultra_keep_backdrop();
    lcd_update();
    last_stamp = screen_stamp(st);
    return true;
}

int usb_stock_timeout(int timeout)
{
    return title_font >= 0 && detail_font >= 0 ? MIN(timeout, SYNC_STEP)
                                               : timeout;
}

void usb_stock_tick(void)
{
    if (title_font < 0 || detail_font < 0 || !stock_art_ready())
        return;
    enum usb_stock_state st = current_state();
    unsigned stamp = screen_stamp(st);
    if (stamp != last_stamp)
    {
        if (st == USB_STOCK_SYNCING && (last_stamp & 3) != USB_STOCK_SYNCING)
        {
            sync_phase = 0;
            next_sync_frame = current_tick + SYNC_STEP;
        }
        usb_stock_draw();
    }
    else if (st == USB_STOCK_SYNCING && !TIME_BEFORE(current_tick, next_sync_frame))
    {
        unsigned steps = 1 + (current_tick - next_sync_frame) / SYNC_STEP;
        next_sync_frame += steps * SYNC_STEP;
        sync_phase = (sync_phase + steps) % SYNC_FRAMES;
        int height = stock_art[STOCK_ART_DISKMODEIMAGE_SYNCARROW01].height;
        draw_rows(SYNC_Y, SYNC_Y + height, true, st);
        lcd_update_rect(0, SYNC_Y, LCD_WIDTH, height);
    }
}

void usb_stock_enter(void)
{
    if (!entering)
        return;
    entering = false;
    usb_ultra_enter(draw_picture_rows);
}
