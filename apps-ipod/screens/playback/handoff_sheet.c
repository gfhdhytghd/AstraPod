/* PodBox continuity handoff. GNU General Public License, version 2+. */
#include "config.h"
#ifdef HAVE_USB_PODLINK
#include "handoff_sheet.h"
#include "audio.h"
#include "backlight.h"
#include "button.h"
#include "cider_wps.h"
#include "draw/line.h"
#include "draw/screen_access.h"
#include "draw/viewport.h"
#include "font.h"
#include "input/action.h"
#include "kernel.h"
#include "lang.h"
#include "metadata.h"
#include "rbpaths.h"
#include "settings/settings.h"
#include "skin/skin_albumart_color.h"
#include "skin/skin_engine.h"
#include "skin/statusbar_skinned.h"
#include "core_alloc.h"
#include "system/shutdown.h"
#include "system.h"
#include "widgets/dialog.h"
#include <stdio.h>
#include <string.h>

#define ART_SIZE 96
#define MARGIN 16
#define BUTTON_HEIGHT 36
#define BUTTON_GAP 8
#define ENTER_TICKS (HZ / 5)
#define EXIT_TICKS (16 * HZ / 100)
#define FRAME_TICKS MAX(1, HZ / 33)
#define BUTTON_Y (LCD_HEIGHT - MARGIN - BUTTON_HEIGHT)
#define BAR_Y (BUTTON_Y + BUTTON_HEIGHT + 5)

struct sheet_ui {
    bool leave;
    int small_font, button_font, large_font;
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

static void sheet_button(int x, int width, bool focus,
                         int font, const unsigned char *text)
{
    lcd_set_foreground(LCD_WHITE);
    if (focus) lcd_fillrect(x, BUTTON_Y, width, BUTTON_HEIGHT);
    else
    {
        lcd_blendrect(x, BUTTON_Y, width, 1, 5);
        lcd_blendrect(x, BUTTON_Y + BUTTON_HEIGHT - 1, width, 1, 5);
        lcd_blendrect(x, BUTTON_Y + 1, 1, BUTTON_HEIGHT - 2, 5);
        lcd_blendrect(x + width - 1, BUTTON_Y + 1, 1, BUTTON_HEIGHT - 2, 5);
    }
    center_text(x, BUTTON_Y, width, BUTTON_HEIGHT, font,
                focus ? LCD_BLACK : LCD_RGBPACK(204, 204, 204), text);
}

static void sheet_draw(const struct handoff_sheet *view, const struct sheet_ui *ui)
{
    const int small = ui->small_font >= 0 ? ui->small_font : screens[0].getuifont();
    const int button = ui->button_font >= 0 ? ui->button_font : small;
    const int large = ui->large_font >= 0 ? ui->large_font : font_get_ui_bold();
    const int small_h = font_get(small)->height, large_h = font_get(large)->height;
    const int button_h = font_get(button)->height;
    const unsigned secondary = LCD_RGBPACK(153, 153, 153);
    const int inner = LCD_WIDTH - 2 * MARGIN;
    const int y = (BUTTON_Y - (ART_SIZE + 12 + 2 * small_h + large_h + 6)) / 2;
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
    char subtitle[112];
    struct mp3entry *track = audio_current_track();
    const char *artist = track && track->artist ? track->artist : "";
    snprintf(subtitle, sizeof subtitle, "%s%s%.96s", view->target,
             *view->target && *artist ? " · " : "", artist);
    center_text(MARGIN, line, inner, small_h, small, secondary,
                text_fit(subtitle, fitted, sizeof fitted, small, inner));

    if (view->reason)
    {
        center_text(MARGIN, BUTTON_Y - 4, inner, large_h, large, LCD_WHITE,
                    str(LANG_HANDOFF_FAILED));
        center_text(MARGIN, BUTTON_Y + large_h, inner, small_h, small, secondary,
                    str(view->reason));
        return;
    }

    if (view->preparing)
    {
        center_text(MARGIN, BUTTON_Y, inner, button_h, button, LCD_WHITE,
                    str(LANG_HANDOFF_PREPARING));
        lcd_set_foreground(LCD_WHITE);
        lcd_blendrect(MARGIN, BAR_Y, inner, 2, 4);
        return;
    }
    int width = (inner - BUTTON_GAP) / 2;
    sheet_button(MARGIN, width, !ui->leave, button, str(LANG_HANDOFF_ACCEPT));
    sheet_button(MARGIN + width + BUTTON_GAP, width, ui->leave, button,
                 str(LANG_HANDOFF_LEAVE));
}

static int sheet_action(struct sheet_ui *ui, const struct handoff_sheet *view, int action)
{
    if (IS_SYSEVENT(action) && action != SYS_USB_CONFIRM)
        default_event_handler(action);
    if (action == ACTION_STD_MENU) return DIALOG_CANCEL;
    if (view->reason) return DIALOG_CONTINUE;
    if (view->preparing) return DIALOG_CONTINUE;
    int button;
    get_action_statuscode(&button);
    if (action == ACTION_STD_OK && button != BUTTON_RIGHT)
        return ui->leave ? DIALOG_CANCEL : DIALOG_ACCEPT;
    if (action == ACTION_STD_NEXT || action == ACTION_STD_NEXTREPEAT ||
        (action == ACTION_STD_OK && button == BUTTON_RIGHT))
        ui->leave = true;
    if (action == ACTION_STD_PREV || action == ACTION_STD_PREVREPEAT ||
        (action == ACTION_STD_CANCEL && button == BUTTON_LEFT))
        ui->leave = false;
    return DIALOG_CONTINUE;
}

static int slide_offset(unsigned long age, int from, bool exit)
{
    int duration = exit ? EXIT_TICKS : ENTER_TICKS;
    int t = MIN(age, (unsigned long)duration);
    if (exit) from = LCD_HEIGHT - from;
    else t = duration - t;
    int scale = duration * duration * duration;
    int distance = (from * t * t * t + (exit ? 0 : scale - 1)) / scale;
    return exit ? LCD_HEIGHT - from + distance : distance;
}

static void slide_frame(fb_data *fb, const fb_data *back, const fb_data *card,
                        int previous, int offset)
{
    if (offset > previous)
        memcpy(fb + previous * LCD_WIDTH, back + previous * LCD_WIDTH,
               (offset - previous) * LCD_WIDTH * sizeof(*fb));
    if (offset < LCD_HEIGHT)
        memcpy(fb + offset * LCD_WIDTH, card,
               (LCD_HEIGHT - offset) * LCD_WIDTH * sizeof(*fb));
    int top = MIN(previous, offset);
    if (top < LCD_HEIGHT)
        lcd_update_rect(0, top, LCD_WIDTH, LCD_HEIGHT - top);
}

static int draw_progress(const struct handoff_sheet *view, fb_data *card, int painted)
{
    if (!view->armed || TIME_BEFORE(current_tick, view->start)) return painted;
    unsigned long age = MIN(current_tick - view->start, 3 * HZ);
    int width = (LCD_WIDTH - 2 * MARGIN) * age / (3 * HZ);
    if (width <= painted) return painted;
    lcd_set_foreground(LCD_WHITE);
    lcd_fillrect(MARGIN, BAR_Y, width, 2);
    if (card)
    {
        for (int y = BAR_Y; y < BAR_Y + 2; ++y)
            memcpy(card + y * LCD_WIDTH + MARGIN, FBADDR(MARGIN, y), width * sizeof(*card));
    }
    lcd_update_rect(MARGIN + painted, BAR_Y, width - painted, 2);
    return width;
}

int handoff_sheet_run(struct handoff_sheet *view, int (*advance)(int))
{
    struct sheet_ui ui = {
        .small_font = font_load_ex(FONT_DIR "/12-noto-sans.fnt", 0, 48),
        .button_font = font_load_ex(FONT_DIR "/18-noto-sans.fnt", 0, 48),
        .large_font = font_load_ex(FONT_DIR "/26-noto-sans-medium.fnt", 0, 48),
    };
    int handle = core_allocatable() >= HANDOFF_FRAME_BYTES ?
                 core_alloc(HANDOFF_FRAME_BYTES) : -1;
    fb_data *back = NULL, *card = NULL;
    if (handle > 0)
    {
        core_pin(handle);
        back = core_get_data(handle);
        card = back + LCD_WIDTH * LCD_HEIGHT;
    }
    struct viewport vp = {0};
    viewport_set_fullscreen(&vp, SCREEN_MAIN);
    struct viewport *outer = lcd_set_viewport(&vp);
    fb_data *fb = FBADDR(0, 0);
    if (back) memcpy(back, fb, FRAMEBUFFER_SIZE);
    bool inhibited = skin_flush_inhibited();
    skin_inhibit_flush(true);
    screens[0].scroll_stop();
    skin_hold_scrolling(true);
    backlight_on();
    action_handoff_enter();

    int result = DIALOG_CONTINUE;
    int offset = card ? LCD_HEIGHT : 0;
    long start_tick = current_tick;
    int painted = 0;
    bool dirty = true, entering = true, boosted = true;
    cpu_boost(true);
    while (result == DIALOG_CONTINUE)
    {
        lcd_set_viewport(&vp);
        if (dirty)
        {
            sheet_draw(view, &ui);
            painted = 0;
            if (card) memcpy(card, fb, FRAMEBUFFER_SIZE);
            else lcd_update_rect(0, 0, LCD_WIDTH, LCD_HEIGHT);
        }
        if (entering)
        {
            start_tick = current_tick;
            entering = false;
        }
        long elapsed = current_tick - start_tick;
        if (card)
        {
            int next = slide_offset(elapsed, LCD_HEIGHT, false);
            if (dirty) memcpy(fb, back, next * LCD_WIDTH * sizeof(*fb));
            if (offset != next || dirty) slide_frame(fb, back, card, offset, next);
            offset = next;
        }

        int ticks = HZ / 10;
        if (offset)
        {
            long spent = current_tick - start_tick - elapsed;
            ticks = MAX(0, MIN(FRAME_TICKS, ENTER_TICKS - elapsed) - spent);
        }
        else
        {
            if (boosted)
            {
                cpu_boost(false);
                boosted = false;
            }
            if (!view->reason) painted = draw_progress(view, card, painted);
        }
        bool leave = ui.leave, preparing = view->preparing;
        const char *target = view->target;
        int reason = view->reason;
        int action = get_action(CONTEXT_STD, ticks);
        result = advance(sheet_action(&ui, view, action));
        dirty = leave != ui.leave || preparing != view->preparing ||
                target != view->target || reason != view->reason;
    }
    int from = offset;
    if (!boosted)
    {
        cpu_boost(true);
    }
    start_tick = current_tick;
    long frame_tick = 0;
    while (card && offset < LCD_HEIGHT)
    {
        long wake = MIN(frame_tick + FRAME_TICKS, EXIT_TICKS);
        long ticks = wake - (current_tick - start_tick);
        /* sleep(0) waits for the next tick. */
        if (ticks > 0) sleep(ticks - 1);
        frame_tick = current_tick - start_tick;
        int next = slide_offset(frame_tick, from, true);
        if (offset != next) slide_frame(fb, back, card, offset, next);
        offset = next;
    }
    if (card)
    {
        memcpy(fb, back, FRAMEBUFFER_SIZE);
        core_unpin(handle);
        core_free(handle);
    }
    cpu_boost(false);
    lcd_set_viewport(outer);
    if (skin_hold_scrolling(false))
    {
        skin_request_full_update(CUSTOM_STATUSBAR);
        sb_skin_force_next_update();
    }
    skin_inhibit_flush(inhibited);
    if (ui.large_font >= 0) font_unload(ui.large_font);
    if (ui.button_font >= 0) font_unload(ui.button_font);
    if (ui.small_font >= 0) font_unload(ui.small_font);
    action_wait_for_release();
    return result;
}

void handoff_present_remote(void)
{
    lcd_update_rect(0, 0, LCD_WIDTH, LCD_HEIGHT);
}
#endif
