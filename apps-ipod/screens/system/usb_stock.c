/***************************************************************************
 * PodBox: the stock look's USB screen. GPL version 2+.
 *
 * Under style.art the disk-mode screen is the stock 1.1.2 firmware's
 * "Connected" layout (DiskMode_ScreenLayout_Connected): Background_Template
 * (Background_LightBlue_Image), the black status bar of the Extras pages,
 * and DiskMode_View -- the orange sphere with the dock connector over it,
 * and two white lines of text centred between x 10 and 310. Stock writes
 * each line with the top of its line box at the y below, which the theme's
 * fonts reproduce, their ascent being stock's rounded up. Stock fixes the
 * bar's title to the player's model name; ours shows the firmware's name.
 *
 * Everything is composed at 8 bits a channel in bands and truncated once.
 ****************************************************************************/

#include "config.h"
#include "system.h"
#include "kernel.h"
#include "lcd.h"
#include "font.h"
#include "lang.h"
#include "settings/settings.h"
#include "ui/native_theme_runtime.h"
#include "ui/stock_art.h"
#include "screens/extras/extras_bar.h"
#include "usb_stock.h"
#include "usb_ultra.h"

#define ART_X       100     /* the pictures' box, centred in it */
#define ART_W       120
#define SPHERE_Y    46
#define TEXT_X      10      /* the text's box */
#define TEXT_W      300
#define TITLE_Y     174
#define DETAIL_Y    196
#define TEXT_RGB    0xffffff
#define BAND_ROWS   4

static int title_font = -1, detail_font = -1;
/* Fruit Company Ultra brings the picture in once the host is answered. */
static bool entering;

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

static void put_text_line(struct stock_canvas *band, int font,
                          const char *text, int y)
{
    if (y >= band->y + band->height ||
        y + (int)font_get(font)->height <= band->y)
        return;
    int width;
    font_getstringsize((const unsigned char *)text, &width, NULL, font);
    stock_canvas_text(band, font, text, TEXT_X + (TEXT_W - width) / 2, y,
                      TEXT_RGB);
}

static void draw_rows(int top, int bottom, bool picture)
{
    static uint32_t band_pixels[LCD_WIDTH * BAND_ROWS];
    for (int y = top; y < bottom; y += BAND_ROWS)
    {
        struct stock_canvas band = {
            band_pixels, 0, y, LCD_WIDTH, MIN(BAND_ROWS, bottom - y),
        };
        stock_art_put(&band, STOCK_ART_BACKGROUND_LIGHTBLUE, 0, 0);
        if (picture)
        {
            put_centred(&band, STOCK_ART_DISKMODEIMAGE_SYNCICON, SPHERE_Y);
            put_centred(&band, STOCK_ART_DISKMODEIMAGE_CONNECTEDICON,
                        SPHERE_Y);
        }
        put_text_line(&band, title_font, str(LANG_USB_STOCK_CONNECTED),
                      TITLE_Y);
        put_text_line(&band, detail_font,
                      str(LANG_USB_STOCK_EJECT_BEFORE_DISCONNECT), DETAIL_Y);
        extras_bar_put(&band, str(LANG_ROCKBOX_TITLE));
        stock_canvas_show(&band, (struct ui_rect){ 0, y, LCD_WIDTH,
                                                   band.height });
        /* The host may be waiting on SET_ADDRESS (usb_screen.c). */
        yield();
    }
}

static void draw_picture_rows(int top, int bottom)
{
    draw_rows(top, bottom, true);
}

bool usb_stock_draw(void)
{
    if (title_font < 0 || detail_font < 0 || !stock_art_ready())
    {
        entering = false;
        usb_ultra_close();
        return false;
    }

    draw_rows(0, LCD_HEIGHT, !entering);
    if (entering)
        usb_ultra_keep_backdrop();
    lcd_update();
    return true;
}

void usb_stock_enter(void)
{
    if (!entering)
        return;
    entering = false;
    usb_ultra_enter(draw_picture_rows);
}
