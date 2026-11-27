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
/* Stock page slides run ~270 ms (18 refreshes on the 67 Hz panel). */
#define PAGE_SLIDE_FRAMES ANIM_F(18)

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
            frames = open ? ui_theme_motion(UI_MOTION_PUSH, PAGE_SLIDE_FRAMES,
                                            PAGE_SLIDE_FRAMES)
                          : ui_theme_motion(UI_MOTION_POP, PAGE_SLIDE_FRAMES,
                                            PAGE_SLIDE_FRAMES);
        else
            frames = open ? ui_theme_motion(UI_MOTION_SHEET_IN,
                                            SHEET_OPEN_FRAMES, SHEET_OPEN_FRAMES)
                          : ui_theme_motion(UI_MOTION_SHEET_OUT,
                                            SHEET_CLOSE_FRAMES, SHEET_CLOSE_FRAMES);
    }
    sheet->tween = (struct ui_tween){
        .from = from, .to = to, .frames = frames,
        .ease_in = !open && !sheet->slide,
        .easing = sheet->slide ? ui_theme_easing() : 0,
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

void ui_sheet_paint_slide(struct ui_sheet *sheet, const fb_data *backdrop,
                          const fb_data *body, bool changed)
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
    int offset = (LCD_WIDTH * (SHEET_PROGRESS - progress) / SHEET_PROGRESS) & ~1;
    if (frame < sheet->tween.frames && sheet->tween.from != sheet->tween.to)
    {
        if (offset <= 0 && sheet->tween.to == SHEET_PROGRESS)
            offset = 2;
        else if (offset >= LCD_WIDTH && sheet->tween.to == 0)
            offset = LCD_WIDTH - 2;
    }
    if (offset == sheet->light && !changed)
    {
        /* This frame is on screen already. */
        if (frame >= sheet->tween.frames)
            ui_sheet_cancel(sheet);
        return;
    }

    fb_data *output = lcd_framebuffer_default.fb_ptr;
    if (offset <= 0)
        memcpy(output, body, FRAMEBUFFER_SIZE);
    else if (offset >= LCD_WIDTH)
        memcpy(output, backdrop, FRAMEBUFFER_SIZE);
    else
    {
        int shift = LCD_WIDTH - offset;
        for (int y = 0; y < LCD_HEIGHT; y++)
        {
            fb_data *dst = output + y * LCD_WIDTH;
            const fb_data *under = backdrop + y * LCD_WIDTH;
            const fb_data *src = body + y * LCD_WIDTH;
            memcpy(dst, under + shift, offset * sizeof(*dst));
            memcpy(dst + offset, src, shift * sizeof(*dst));
        }
    }
    sheet->light = offset;
    ui_note_render((uint32_t)USEC_TIMER - start);
    ui_mark_dirty(SCREEN_MAIN);
    ui_flush_dirty();
    if (frame >= sheet->tween.frames)
        ui_sheet_cancel(sheet);
}
