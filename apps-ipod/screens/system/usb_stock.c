/***************************************************************************
 * PodBox: the stock look's USB screen. GPL version 2+.
 *
 * Under style.art the disk-mode screen is the stock 1.1.2 firmware's
 * "Connected" layout (its DiskMode_View resource): black, the orange sphere
 * with the dock connector over it, and two white lines of text centred
 * between x 10 and 310. Stock writes each line with the top of its line box
 * at the y below, which the theme's fonts reproduce, their ascent being
 * stock's rounded up.
 *
 * The sphere and the connector overlap, so they are composed at 8 bits a
 * channel in bands and truncated once; the text lies on black only and goes
 * straight to the framebuffer.
 ****************************************************************************/

#include <string.h>
#include "config.h"
#include "system.h"
#include "lcd.h"
#include "font.h"
#include "lang.h"
#include "settings/settings.h"
#include "draw/viewport.h"
#include "ui/native_theme_runtime.h"
#include "ui/stock_art.h"
#include "usb_stock.h"

#define ART_X       100     /* the pictures' box, centred in it */
#define ART_W       120
#define SPHERE_Y    46
#define TEXT_X      10      /* the text's box */
#define TEXT_W      300
#define TITLE_Y     174
#define DETAIL_Y    196
#define BAND_ROWS   8

static int title_font = -1, detail_font = -1;

static bool stock_look(void)
{
    return ui_theme_style(UI_STYLE_ART) == UI_ART_PLASTIC;
}

void usb_stock_open(void)
{
    if (!stock_look())
        return;
    int glyphs = global_settings.glyphs_to_cache;
    title_font = ui_theme_current_font_load(UI_FONT_WPS_TITLE, glyphs);
    detail_font = ui_theme_current_font_load(UI_FONT_BODY, glyphs);
}

void usb_stock_close(void)
{
    if (title_font >= 0)
        font_unload(title_font);
    if (detail_font >= 0)
        font_unload(detail_font);
    title_font = detail_font = -1;
}

static void put_centred(struct stock_canvas *band, enum stock_art_id id, int y)
{
    stock_art_put(band, id, ART_X + (ART_W - stock_art[id].width) / 2, y);
}

static void draw_line(struct viewport *vp, int font, const char *text, int y)
{
    int width;
    vp->font = font;
    font_getstringsize((const unsigned char *)text, &width, NULL, font);
    lcd_putsxy(TEXT_X + (TEXT_W - width) / 2, y, (const unsigned char *)text);
}

bool usb_stock_draw(void)
{
    if (title_font < 0 || detail_font < 0 || !stock_art_ready())
        return false;

    memset(lcd_framebuffer_default.fb_ptr, 0,
           LCD_WIDTH * LCD_HEIGHT * sizeof(fb_data));

    static uint32_t band_pixels[ART_W * BAND_ROWS];
    int bottom = SPHERE_Y +
        MAX(stock_art[STOCK_ART_DISKMODEIMAGE_SYNCICON].height,
            stock_art[STOCK_ART_DISKMODEIMAGE_CONNECTEDICON].height);
    for (int y = SPHERE_Y; y < bottom; y += BAND_ROWS)
    {
        struct stock_canvas band = {
            band_pixels, ART_X, y, ART_W, MIN(BAND_ROWS, bottom - y),
        };
        memset(band_pixels, 0, sizeof(band_pixels));
        put_centred(&band, STOCK_ART_DISKMODEIMAGE_SYNCICON, SPHERE_Y);
        put_centred(&band, STOCK_ART_DISKMODEIMAGE_CONNECTEDICON, SPHERE_Y);
        stock_canvas_show(&band, (struct ui_rect){ 0, 0, LCD_WIDTH,
                                                   LCD_HEIGHT });
    }

    struct viewport vp;
    viewport_set_defaults(&vp, SCREEN_MAIN);
    vp.x = vp.y = 0;
    vp.width = LCD_WIDTH;
    vp.height = LCD_HEIGHT;
    vp.fg_pattern = LCD_WHITE;
    vp.bg_pattern = LCD_BLACK;
    vp.drawmode = DRMODE_FG;
    struct viewport *old = lcd_set_viewport(&vp);
    draw_line(&vp, title_font, str(LANG_USB_CONNECTED), TITLE_Y);
    draw_line(&vp, detail_font, str(LANG_USB_EJECT_BEFORE_DISCONNECT),
              DETAIL_Y);
    lcd_set_viewport(old);
    lcd_update();
    return true;
}
