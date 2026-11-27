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
 * out over 135 ms while it shrinks to 94%. 2009-style modal alerts pop in
 * over ~300 ms with a brief scale overshoot (0.62 -> 1.07 -> 0.97 -> 1.00). */
#define ZOOM_OPEN_FRAMES ANIM_F(12)
#define ZOOM_CLOSE_FRAMES ANIM_F(9)
#define ALERT_OPEN_FRAMES ANIM_F(20)
#define ZOOM_OPEN_FROM 901
#define ZOOM_CLOSE_TO 963
#define SLIDE_FRAMES ANIM_F(18)

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
    unsigned frames = 0;
    if (lcd_active() && from != to)
    {
        if (sheet->slide)
            frames = ui_theme_motion(open ? UI_MOTION_PUSH : UI_MOTION_POP,
                                     SLIDE_FRAMES, SLIDE_FRAMES);
        else
            frames = open ? ui_theme_motion(UI_MOTION_SHEET_IN,
                                            sheet->zoom
                                                ? (sheet->bounce ? ALERT_OPEN_FRAMES
                                                                 : ZOOM_OPEN_FRAMES)
                                                : SHEET_OPEN_FRAMES,
                                            SHEET_OPEN_FRAMES)
                          : ui_theme_motion(UI_MOTION_SHEET_OUT,
                                            sheet->zoom ? ZOOM_CLOSE_FRAMES
                                                        : SHEET_CLOSE_FRAMES,
                                            SHEET_CLOSE_FRAMES);
    }
    sheet->tween = (struct ui_tween){
        .from = from, .to = to,
        .frames = frames,
        .ease_in = !open && !sheet->slide,
        .easing = sheet->slide ? ui_theme_easing() : UI_EASE_CUBIC,
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

/* Smoothstep on u in [0, 1024] -> [0, 1024]. */
static int sheet_smoothstep(int u)
{
    int64_t x = u;
    return (int)((x * x * (3 * SHEET_PROGRESS - 2 * x)) /
                 ((int64_t)SHEET_PROGRESS * SHEET_PROGRESS));
}

/* 2009 UIAlertView pop-in bounce scale curve (1024 == 1.00x):
 *   0..580  : ease-out  0.62 (635) -> 1.07 (1096) overshoot
 *   580..820: smooth    1.07 (1096) -> 0.97 (993) rebound
 *   820..1024: smooth   0.97 (993) -> 1.00 (1024) settle */
static int alert_bounce_scale(int t)
{
    if (t <= 580)
    {
        int u = t * SHEET_PROGRESS / 580;
        int inv = SHEET_PROGRESS - u;
        int e = SHEET_PROGRESS - (int)(((int64_t)inv * inv * inv) /
                                       ((int64_t)SHEET_PROGRESS * SHEET_PROGRESS));
        return 635 + (1096 - 635) * e / SHEET_PROGRESS;
    }
    if (t <= 820)
    {
        int e = sheet_smoothstep((t - 580) * SHEET_PROGRESS / 240);
        return 1096 + (993 - 1096) * e / SHEET_PROGRESS;
    }
    int e = sheet_smoothstep((t - 820) * SHEET_PROGRESS / 204);
    return 993 + (SHEET_PROGRESS - 993) * e / SHEET_PROGRESS;
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
    bool bouncing = sheet->bounce && sheet->tween.to > 0;
    int progress, scale;
    unsigned ink, light;
    if (bouncing)
    {
        int t = (!sheet->active || !sheet->tween.frames || frame >= sheet->tween.frames)
              ? SHEET_PROGRESS
              : (int)(frame * SHEET_PROGRESS / sheet->tween.frames);
        progress = t;
        scale = t >= SHEET_PROGRESS ? SHEET_PROGRESS : alert_bounce_scale(t);
        ink = t >= 420 ? UI_FULL_OPACITY : UI_FULL_OPACITY * t / 420;
        int dim_p = t >= 580 ? SHEET_PROGRESS : t * SHEET_PROGRESS / 580;
        light = UI_FULL_OPACITY - SHEET_DIM * dim_p / SHEET_PROGRESS;
    }
    else
    {
        progress = ui_tween_sample(&sheet->tween, frame);
        int from = sheet->tween.to ? ZOOM_OPEN_FROM : ZOOM_CLOSE_TO;
        scale = from + (SHEET_PROGRESS - from) * progress / SHEET_PROGRESS;
        ink = UI_FULL_OPACITY * progress / SHEET_PROGRESS;
        light = UI_FULL_OPACITY - SHEET_DIM * progress / SHEET_PROGRESS;
    }
    int frame_key = bouncing ? ((scale << 9) | (int)light) : (int)light;
    if (frame_key == sheet->light)
    {
        /* This frame is on screen already. */
        if (frame >= sheet->tween.frames)
            ui_sheet_cancel(sheet);
        return;
    }

    fb_data *output = lcd_framebuffer_default.fb_ptr;
    if (progress >= SHEET_PROGRESS)
    {
        memcpy(output, body, FRAMEBUFFER_SIZE);
    }
    else
    {
        /* The panel at this scale about its centre, and the source column of
         * each of its screen columns. Clamped so bounce overshoot (> 1.0x)
         * never samples outside bounds. */
        int width = bounds.width * scale / SHEET_PROGRESS;
        int height = bounds.height * scale / SHEET_PROGRESS;
        struct ui_rect panel = { bounds.x + (bounds.width - width) / 2,
                                 bounds.y + (bounds.height - height) / 2,
                                 width, height };
        struct ui_rect visible = clip(panel, LCD_WIDTH, LCD_HEIGHT);
        static uint16_t column[LCD_WIDTH];
        static fb_data sampled[LCD_WIDTH];
        int max_sx = bounds.x + MAX(0, bounds.width - 1);
        int max_sy = bounds.y + MAX(0, bounds.height - 1);
        for (int x = 0; x < visible.width; x++)
        {
            int sx = bounds.x +
                (visible.x - panel.x + x) * bounds.width / MAX(1, width);
            column[x] = (uint16_t)MIN(max_sx, MAX(bounds.x, sx));
        }

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
            source = MIN(max_sy, MAX(bounds.y, source));
            const fb_data *row = body + source * LCD_WIDTH;
            for (int x = 0; x < visible.width; x++)
                sampled[x] = row[column[x]];
            ui_blit_mix(dst + visible.x, sampled, dst + visible.x, visible.width,
                        ink, UI_FULL_OPACITY - ink);
        }
    }
    sheet->light = frame_key;
    ui_note_render((uint32_t)USEC_TIMER - start);
    ui_mark_dirty(SCREEN_MAIN);
    ui_flush_dirty();
    if (frame >= sheet->tween.frames)
        ui_sheet_cancel(sheet);
}

void ui_sheet_paint_slide(struct ui_sheet *sheet, const fb_data *backdrop,
                          const fb_data *body, int top)
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
    if (progress == sheet->light)
    {
        if (frame >= sheet->tween.frames)
            ui_sheet_cancel(sheet);
        return;
    }

    fb_data *output = lcd_framebuffer_default.fb_ptr;
    unsigned ink = UI_FULL_OPACITY * progress / SHEET_PROGRESS;
    int offset = LCD_WIDTH * progress / SHEET_PROGRESS;
    if (top < 0)
        top = 0;
    if (top > LCD_HEIGHT)
        top = LCD_HEIGHT;

    for (int y = 0; y < top; y++)
        ui_blit_mix(output + y * LCD_WIDTH, body + y * LCD_WIDTH,
                    backdrop + y * LCD_WIDTH, LCD_WIDTH, ink,
                    UI_FULL_OPACITY - ink);

    for (int y = top; y < LCD_HEIGHT; y++)
    {
        fb_data *dst = output + y * LCD_WIDTH;
        const fb_data *under = backdrop + y * LCD_WIDTH;
        const fb_data *page = body + y * LCD_WIDTH;
        if (offset <= 0)
            memcpy(dst, under, LCD_WIDTH * sizeof(fb_data));
        else if (offset >= LCD_WIDTH)
            memcpy(dst, page, LCD_WIDTH * sizeof(fb_data));
        else
        {
            int back_w = LCD_WIDTH - offset;
            memcpy(dst, under + offset, back_w * sizeof(fb_data));
            memcpy(dst + back_w, page, offset * sizeof(fb_data));
        }
    }
    sheet->light = progress;
    ui_note_render((uint32_t)USEC_TIMER - start);
    ui_mark_dirty(SCREEN_MAIN);
    ui_flush_dirty();
    if (frame >= sheet->tween.frames)
        ui_sheet_cancel(sheet);
}
