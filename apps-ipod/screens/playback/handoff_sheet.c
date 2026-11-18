/* PodBox continuity handoff. GNU General Public License, version 2+. */
#include "config.h"
#ifdef HAVE_USB_PODLINK
#include "handoff_sheet.h"
#include "audio.h"
#include "backlight.h"
#include "button.h"
#include "cider_wps.h"
#include "draw/color.h"
#include "draw/line.h"
#include "draw/screen_access.h"
#include "font.h"
#include "input/action.h"
#include "kernel.h"
#include "lang.h"
#include "metadata.h"
#include "rbpaths.h"
#include "settings/settings.h"
#include "skin/skin_albumart_color.h"
#include "skin/skin_engine.h"
#include "system/shutdown.h"
#include "widgets/dialog.h"
#include <stdio.h>

#define ART_SIZE 96
#define MARGIN 16
#define BUTTON_HEIGHT 32
#define BUTTON_GAP 8
#define TRANSITION (12 * HZ / 100)

struct sheet_ui {
    struct handoff_sheet *view;
    int (*advance)(int);
    bool leave;
    int focus_from;
    long focus_tick;
    int small_font, large_font;
    char subtitle[112];
};

static void center_text(int x, int y, int width, int height, int font,
                        unsigned color, const unsigned char *text)
{
    int w, h;
    lcd_setfont(font);
    lcd_getstringsize(text, &w, &h);
    lcd_set_foreground(color);
    lcd_putsxy(x + (width - w) / 2, y + (height - h) / 2, text);
}

static void art_square(int handle, int x, int y)
{
    struct bitmap *bm;
    bool drawn = false;
    if (wps_art_pin(handle, true))
    {
        if (wps_art_bitmap(handle, &bm) && bm->width > 0 && bm->height > 0)
        {
            /* Square center crop. A native pixel row is the only scratch. */
            const fb_data *source = (const fb_data *)bm->data;
            int side = MIN(bm->width, bm->height);
            source += (bm->height - side) / 2 * bm->width + (bm->width - side) / 2;
            fb_data row[ART_SIZE];
            for (int dy = 0; dy < ART_SIZE; ++dy)
            {
                const fb_data *src = source + dy * side / ART_SIZE * bm->width;
                for (int dx = 0; dx < ART_SIZE; ++dx)
                    row[dx] = src[dx * side / ART_SIZE];
                lcd_bitmap(row, x, y + dy, ART_SIZE, 1);
            }
            drawn = true;
        }
        wps_art_pin(handle, false);
    }
    if (!drawn)
    {
        lcd_set_foreground(LCD_RGBPACK(24, 24, 24));
        lcd_fillrect(x, y, ART_SIZE, ART_SIZE);
        lcd_set_foreground(LCD_RGBPACK(153, 153, 153));
        /* Two note heads and stems, drawn inside the square frame. */
        lcd_fillrect(x + 32, y + 56, 10, 6);
        lcd_fillrect(x + 52, y + 52, 10, 6);
        lcd_fillrect(x + 40, y + 38, 2, 20);
        lcd_fillrect(x + 60, y + 34, 2, 20);
        lcd_drawline(x + 40, y + 38, x + 61, y + 34);
        lcd_drawline(x + 40, y + 39, x + 61, y + 35);
    }
    lcd_set_foreground(LCD_WHITE);
    lcd_blendrect(x, y, ART_SIZE, 1, 4);
    lcd_blendrect(x, y + ART_SIZE - 1, ART_SIZE, 1, 4);
    lcd_blendrect(x, y + 1, 1, ART_SIZE - 2, 4);
    lcd_blendrect(x + ART_SIZE - 1, y + 1, 1, ART_SIZE - 2, 4);
}

static int fade_weight(long tick)
{
    return (int)MIN((unsigned long)(current_tick - tick), TRANSITION) * 256 / TRANSITION;
}

static int focus_weight(const struct sheet_ui *ui)
{
    int p = fade_weight(ui->focus_tick);
    return (ui->focus_from * (256 - p) + (ui->leave ? 256 : 0) * p) / 256;
}

static void sheet_button(int x, int y, int width, int focus, int opacity,
                         int font, const unsigned char *text)
{
    unsigned bg = *FBADDR(x + width / 2, y + BUTTON_HEIGHT / 2);
    lcd_set_foreground(LCD_WHITE);
    int fill = opacity * focus * LCD_BLEND_OPAQUE / (256 * 256);
    lcd_blendrect(x, y, width, BUTTON_HEIGHT, fill);
    int edge = opacity * (256 - focus) * 5 / (256 * 256);
    lcd_blendrect(x, y, width, 1, edge);
    lcd_blendrect(x, y + BUTTON_HEIGHT - 1, width, 1, edge);
    lcd_blendrect(x, y + 1, 1, BUTTON_HEIGHT - 2, edge);
    lcd_blendrect(x + width - 1, y + 1, 1, BUTTON_HEIGHT - 2, edge);
    unsigned fg = color_blend(LCD_RGBPACK(204, 204, 204), LCD_BLACK, focus);
    center_text(x, y, width, BUTTON_HEIGHT, font, color_blend(bg, fg, opacity), text);
}

static void sheet_draw(struct dialog *d, struct screen *s, struct viewport *vp, void *data)
{
    (void)d;
    (void)vp;
    struct sheet_ui *ui = data;
    const struct handoff_sheet *view = ui->view;
    const int small = ui->small_font >= 0 ? ui->small_font : s->getuifont();
    const int large = ui->large_font >= 0 ? ui->large_font : font_get_ui_bold();
    const int small_h = font_get(small)->height, large_h = font_get(large)->height;
    const unsigned secondary = LCD_RGBPACK(153, 153, 153);
    const int bottom = LCD_HEIGHT - MARGIN - BUTTON_HEIGHT;
    const int inner = LCD_WIDTH - 2 * MARGIN;
    const int y = (bottom - (ART_SIZE + 12 + 2 * small_h + large_h + 6)) / 2;
    int handle = skin_lyrics_art();
    lcd_set_drawmode(DRMODE_FG);
    lcd_set_foreground(LCD_BLACK);
    if (skin_handoff_draw_blur(handle))
        lcd_blendrect(0, 0, LCD_WIDTH, LCD_HEIGHT, 8);
    else
        lcd_fillrect(0, 0, LCD_WIDTH, LCD_HEIGHT);

    art_square(handle, (LCD_WIDTH - ART_SIZE) / 2, y);
    int line = y + ART_SIZE + 12;
    char fitted[TEXT_FIT_BUF];
    center_text(MARGIN, line, inner, small_h, small, secondary, str(LANG_HANDOFF_TITLE));
    line += small_h + 3;
    center_text(MARGIN, line, inner, large_h, large, LCD_WHITE,
                text_fit(view->title, fitted, sizeof fitted, large, inner));
    line += large_h + 3;
    center_text(MARGIN, line, inner, small_h, small, secondary,
                text_fit(ui->subtitle, fitted, sizeof fitted, small, inner));

    if (view->reason)
    {
        center_text(MARGIN, bottom - 4, inner, large_h, large, LCD_WHITE,
                    str(LANG_HANDOFF_FAILED));
        center_text(MARGIN, bottom + large_h, inner, small_h, small, secondary,
                    str(view->reason));
        return;
    }

    int opacity = view->preparing ? fade_weight(view->accepted) : 0;
    if (opacity < 256)
    {
        int focus = focus_weight(ui);
        int width = (inner - BUTTON_GAP) / 2;
        sheet_button(MARGIN, bottom, width, 256 - focus, 256 - opacity, small,
                     str(LANG_HANDOFF_ACCEPT));
        sheet_button(MARGIN + width + BUTTON_GAP, bottom, width, focus, 256 - opacity,
                     small, str(LANG_HANDOFF_LEAVE));
    }
    if (opacity > 0)
    {
        unsigned bg = *FBADDR(LCD_WIDTH / 2, bottom + small_h / 2);
        center_text(MARGIN, bottom, inner, small_h, small,
                    color_blend(bg, LCD_WHITE, opacity), str(LANG_HANDOFF_PREPARING));
        int bar_y = bottom + small_h + 7;
        int age = view->armed && TIME_AFTER(current_tick, view->start) ?
            MIN(current_tick - view->start, 3 * HZ) : 0;
        lcd_set_foreground(LCD_WHITE);
        lcd_blendrect(MARGIN, bar_y, inner, 2, opacity * 4 / 256);
        lcd_blendrect(MARGIN, bar_y, inner * age / (3 * HZ), 2,
                      opacity * LCD_BLEND_OPAQUE / 256);
    }
}

static int sheet_action(struct dialog *d, int action, void *data)
{
    struct sheet_ui *ui = data;
    int answer = DIALOG_CONTINUE;
    if (d->backlight_on && !ui->view->preparing && !ui->view->reason)
    {
        int button;
        get_action_statuscode(&button);
        if (action == ACTION_STD_MENU)
            answer = DIALOG_CANCEL;
        else if (action == ACTION_STD_OK && button != BUTTON_RIGHT)
            answer = ui->leave ? DIALOG_CANCEL : DIALOG_ACCEPT;
        else if (action == ACTION_STD_PREV || action == ACTION_STD_PREVREPEAT ||
                 action == ACTION_STD_NEXT || action == ACTION_STD_NEXTREPEAT ||
                 (action == ACTION_STD_OK && button == BUTTON_RIGHT) ||
                 (action == ACTION_STD_CANCEL && button == BUTTON_LEFT))
        {
            bool leave = action == ACTION_STD_NEXT || action == ACTION_STD_NEXTREPEAT ||
                         button == BUTTON_RIGHT;
            if (leave != ui->leave)
            {
                ui->focus_from = focus_weight(ui);
                ui->focus_tick = current_tick;
                ui->leave = leave;
            }
        }
    }
    if (IS_SYSEVENT(action) && action != SYS_USB_CONFIRM)
        default_event_handler(action);
    int result = ui->advance(answer);
    d->poll_ticks = ui->view->preparing || fade_weight(ui->focus_tick) < 256 ?
        HZ / 50 : HZ / 10;
    return result;
}

int handoff_sheet_run(struct handoff_sheet *view, int (*advance)(int))
{
    static const struct dialog_callbacks callbacks = {
        .draw = sheet_draw,
        .on_action = sheet_action,
    };
    struct sheet_ui ui = { .view = view, .advance = advance,
                           .focus_tick = current_tick - TRANSITION,
                           .small_font = font_load_ex(FONT_DIR "/12-noto-sans.fnt", 0, 48),
                           .large_font = font_load_ex(FONT_DIR "/26-noto-sans-medium.fnt", 0, 48) };
    struct mp3entry *track = audio_current_track();
    const char *artist = track && track->artist ? track->artist : "";
    snprintf(ui.subtitle, sizeof ui.subtitle, "%s%s%.96s", view->target,
             *artist ? " · " : "", artist);
    struct dialog dialog;
    dialog_init(&dialog, CONTEXT_STD, NULL, NULL, &callbacks, &ui);
    backlight_on();
    bool inhibited = skin_flush_inhibited();
    int result = dialog_run(&dialog, HZ / 10);
    skin_inhibit_flush(inhibited);
    if (ui.large_font >= 0) font_unload(ui.large_font);
    if (ui.small_font >= 0) font_unload(ui.small_font);
    action_wait_for_release();
    return result;
}

void handoff_present_remote(void)
{
    /* The new WPS is in the framebuffer while the LCD still shows the
     * sheet. Reveal interleaved regions of that frame without a second copy. */
    const int band = LCD_HEIGHT / 12;
    for (int pass = 0; pass < 4; ++pass)
    {
        for (int y = pass * band; y < LCD_HEIGHT; y += 4 * band)
            lcd_update_rect(0, y, LCD_WIDTH, MIN(band, LCD_HEIGHT - y));
        if (pass != 3) sleep(HZ / 25);
    }
}
#endif
