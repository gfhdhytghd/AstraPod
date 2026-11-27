/* Shared native sheet transitions. GPL version 2+. */
#include "config.h"
#include "button.h"
#include "frame.h"
#include "kernel.h"
#include "perf.h"
#include "native_theme_runtime.h"
#include "sheet.h"
#include "system.h"
#include <stdio.h>
#include <string.h>

#define SHEET_OPEN_FRAMES ANIM_F(16)
#define SHEET_CLOSE_FRAMES ANIM_F(12)
#define SHEET_PROGRESS 1024
#define SHEET_DIM (UI_FULL_OPACITY * 35 / 100)
/* The HUD fades in over 180 ms while it grows from 88% of its size, and
 * out over 135 ms while it shrinks to 94%. */
#define ZOOM_OPEN_FRAMES ANIM_F(12)
#define ZOOM_CLOSE_FRAMES ANIM_F(9)
#define ZOOM_OPEN_FROM 901
#define ZOOM_CLOSE_TO 963

void ui_sheet_cancel(struct ui_sheet *sheet)
{
    if (!sheet->active)
        return;
    sheet->active = sheet->starting = false;
    sheet->tween.frames = 0;
    cpu_boost(false);
    ui_perf_end(sheet->trace);
    sheet->trace = 0;
}

void ui_sheet_begin(struct ui_sheet *sheet, bool open, const char *route)
{
    uint32_t now = USEC_TIMER;
    sheet->tint = ui_theme_rgb565(UI_COLOR_DIM);
    int from = sheet->starting ? sheet->tween.from :
        ui_tween_sample(&sheet->tween, ui_clock_frame(now - sheet->tween.start_clock));
    int to = open ? SHEET_PROGRESS : 0;
    ui_sheet_cancel(sheet);
    sheet->tween = (struct ui_tween){
        .from = from, .to = to,
        .frames = lcd_active() && from != to
                    ? open ? ui_theme_motion(UI_MOTION_SHEET_IN,
                                             sheet->zoom ? ZOOM_OPEN_FRAMES
                                                         : SHEET_OPEN_FRAMES,
                                             SHEET_OPEN_FRAMES)
                           : ui_theme_motion(UI_MOTION_SHEET_OUT,
                                             sheet->zoom ? ZOOM_CLOSE_FRAMES
                                                         : SHEET_CLOSE_FRAMES,
                                             SHEET_CLOSE_FRAMES)
                    : 0,
        .ease_in = !open,
    };
    if (sheet->tween.frames)
    {
        sheet->active = sheet->starting = true;
        sheet->requested_at = now;
        cpu_boost(true);
        char trace_route[32];
        snprintf(trace_route, sizeof(trace_route), "%s-%s", route, open ? "up" : "down");
        sheet->trace = ui_perf_begin(trace_route);
    }
}

int ui_sheet_timeout(const struct ui_sheet *sheet, int timeout)
{
    if (!sheet->active || timeout == TIMEOUT_NOBLOCK)
        return timeout;
    if (sheet->starting)
        return TIMEOUT_NOBLOCK;
    int next = ui_clock_poll_timeout((uint32_t)USEC_TIMER - sheet->tween.start_clock,
                                sheet->tween.frames, HZ);
    return timeout == TIMEOUT_BLOCK ? next : MIN(timeout, next);
}

bool ui_sheet_wait(const struct ui_sheet *sheet)
{
    if (!sheet->active || ui_flush_inhibited())
        return false;
    int timeout = ui_sheet_timeout(sheet, TIMEOUT_BLOCK);
#if defined(HAVE_USB_PODLINK) || (defined(SIMULATOR) && defined(IPOD_6G))
    if (!timeout)
        yield();
    return !button_queue_wait_pending(timeout);
#else
    ui_clock_sleep(timeout);
    return true;
#endif
}

static struct ui_rect clip(struct ui_rect box, int width, int height)
{
    int x = MAX(box.x, 0);
    int y = MAX(box.y, 0);
    return (struct ui_rect){ x, y, MAX(0, MIN(box.x + box.width, width) - x),
                                   MAX(0, MIN(box.y + box.height, height) - y) };
}

static struct ui_rect combined(struct ui_rect a, struct ui_rect b)
{
    if (!a.width || !a.height)
        return b;
    if (!b.width || !b.height)
        return a;
    int x = MIN(a.x, b.x), y = MIN(a.y, b.y);
    return (struct ui_rect){ x, y, MAX(a.x + a.width, b.x + b.width) - x,
                                   MAX(a.y + a.height, b.y + b.height) - y };
}

void ui_sheet_paint(struct ui_sheet *sheet, const fb_data *backdrop,
                    const fb_data *body, struct ui_rect bounds, bool changed)
{
    ui_sheet_paint_in(sheet, backdrop, body, bounds, changed,
                      lcd_framebuffer_default.fb_ptr, LCD_WIDTH, LCD_HEIGHT, NULL);
}

void ui_sheet_paint_in(struct ui_sheet *sheet, const fb_data *backdrop,
                       const fb_data *body, struct ui_rect bounds, bool changed,
                       fb_data *output, int width, int height, void (*present)(void))
{
    if (ui_flush_inhibited())
        return;
    if (!lcd_active())
    {
        ui_sheet_cancel(sheet);
        return;
    }

    if (sheet->starting)
    {
        ui_perf_ready(sheet->trace, sheet->requested_at);
        sheet->starting = false;
        sheet->tween.start_clock = ui_clock_start();
    }
    uint32_t start = USEC_TIMER;
    unsigned frame = sheet->active
                   ? ui_clock_frame(start - sheet->tween.start_clock) : 0;
    int progress = ui_tween_sample(&sheet->tween, frame);
    bounds.y += (height - bounds.y) * (SHEET_PROGRESS - progress) / SHEET_PROGRESS;
    struct ui_rect visible = clip(bounds, width, height);
    int light = UI_FULL_OPACITY - SHEET_DIM * progress / SHEET_PROGRESS;
    struct ui_rect damage = {0};
    if (light != sheet->light)
        damage = (struct ui_rect){0, 0, width, height};
    else if (changed || memcmp(&visible, &sheet->previous, sizeof(visible)))
        damage = combined(visible, sheet->previous);

    int right = damage.x + damage.width;
    for (int y = damage.y; damage.width && y < damage.y + damage.height; y++)
    {
        int row = y * width;
        int split = right, end = right;
        if (visible.height > 0 && visible.width > 0 && y >= visible.y &&
            y < visible.y + visible.height)
        {
            split = visible.x;
            end = split + visible.width;
            const fb_data *source = body + (y - bounds.y) * bounds.width + split - bounds.x;
            memcpy(output + row + split, source, visible.width * sizeof(*source));
        }
        if (split > damage.x)
            ui_blit_tint(output + row + damage.x, backdrop + row + damage.x,
                         split - damage.x, light, sheet->tint);
        if (right > end)
            ui_blit_tint(output + row + end, backdrop + row + end, right - end, light,
                         sheet->tint);
    }
    sheet->previous = visible;
    sheet->light = light;
    if (damage.width > 0 && damage.height > 0)
    {
        ui_note_render((uint32_t)USEC_TIMER - start);
        if (present)
        {
            uint32_t submit = USEC_TIMER;
            present();
            ui_note_flush((uint32_t)USEC_TIMER - submit);
        }
        else
        {
            ui_mark_dirty_rect(SCREEN_MAIN, damage.x, damage.y, damage.width, damage.height);
            ui_flush_dirty();
        }
    }
    if (frame >= sheet->tween.frames)
        ui_sheet_cancel(sheet);
}

void ui_sheet_dim(fb_data *dst, const fb_data *src, int pixels)
{
    ui_blit_tint(dst, src, pixels, UI_FULL_OPACITY - SHEET_DIM,
                 ui_theme_rgb565(UI_COLOR_DIM));
}

void ui_sheet_paint_zoom(struct ui_sheet *sheet, const fb_data *backdrop,
                         const fb_data *body, struct ui_rect bounds, int top)
{
    if (ui_flush_inhibited())
        return;
    if (!lcd_active())
    {
        ui_sheet_cancel(sheet);
        return;
    }
    if (sheet->starting)
    {
        ui_perf_ready(sheet->trace, sheet->requested_at);
        sheet->starting = false;
        sheet->tween.start_clock = ui_clock_start();
    }
    uint32_t start = USEC_TIMER;
    unsigned frame = sheet->active
                   ? ui_clock_frame(start - sheet->tween.start_clock) : 0;
    int progress = ui_tween_sample(&sheet->tween, frame);
    int from = sheet->tween.to ? ZOOM_OPEN_FROM : ZOOM_CLOSE_TO;
    int scale = from + (SHEET_PROGRESS - from) * progress / SHEET_PROGRESS;
    unsigned ink = UI_FULL_OPACITY * progress / SHEET_PROGRESS;
    unsigned light = UI_FULL_OPACITY - SHEET_DIM * progress / SHEET_PROGRESS;
    if ((int)light == sheet->light)
    {
        /* This frame is on screen already. */
        if (frame >= sheet->tween.frames)
            ui_sheet_cancel(sheet);
        return;
    }

    /* The panel at this scale about its centre, and the source column of
     * each of its screen columns. */
    int width = bounds.width * scale / SHEET_PROGRESS;
    int height = bounds.height * scale / SHEET_PROGRESS;
    struct ui_rect panel = { bounds.x + (bounds.width - width) / 2,
                             bounds.y + (bounds.height - height) / 2,
                             width, height };
    struct ui_rect visible = clip(panel, LCD_WIDTH, LCD_HEIGHT);
    static uint16_t column[LCD_WIDTH];
    static fb_data sampled[LCD_WIDTH];
    for (int x = 0; x < visible.width; x++)
        column[x] = bounds.x +
            (visible.x - panel.x + x) * bounds.width / MAX(1, width);

    fb_data *output = lcd_framebuffer_default.fb_ptr;
    for (int y = 0; y < LCD_HEIGHT; y++)
    {
        fb_data *dst = output + y * LCD_WIDTH;
        const fb_data *under = backdrop + y * LCD_WIDTH;
        if (y < top)
        {
            ui_blit_mix(dst, body + y * LCD_WIDTH, under, LCD_WIDTH, ink,
                        UI_FULL_OPACITY - ink);
            continue;
        }
        ui_blit_tint(dst, under, LCD_WIDTH, light, sheet->tint);
        if (y < visible.y || y >= visible.y + visible.height || !ink)
            continue;
        int source = bounds.y + (y - panel.y) * bounds.height / MAX(1, height);
        const fb_data *row = body + source * LCD_WIDTH;
        for (int x = 0; x < visible.width; x++)
            sampled[x] = row[column[x]];
        ui_blit_mix(dst + visible.x, sampled, dst + visible.x, visible.width,
                    ink, UI_FULL_OPACITY - ink);
    }
    sheet->light = light;
    ui_note_render((uint32_t)USEC_TIMER - start);
    ui_mark_dirty(SCREEN_MAIN);
    ui_flush_dirty();
    if (frame >= sheet->tween.frames)
        ui_sheet_cancel(sheet);
}
