/* PodBox Fruit Company Ultra: sheet motion. GPL version 2+.
 *
 * A sheet rises from the bottom as Fruit Company's does, but on a spring
 * that carries it a little past its place and settles by 80% of the time,
 * growing from 92% to full size about its centre as it comes, while the
 * backdrop darkens. It leaves faster, accelerating down and shrinking a
 * little. The dim keeps a linear part so that every frame changes. The last
 * frame of either is left to the stock compositor, so a sheet at rest is
 * Fruit Company's. Scaling samples the body through a column map built
 * once a frame; each output row costs one division. */
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "kernel.h"
#include "lcd.h"
#include "system.h"

#include "frame.h"
#include "native_theme_runtime.h"
#include "perf.h"
#include "ultra.h"
#include "ultra_sheet.h"

#define OPEN_FRAMES ANIM_F(20)
#define CLOSE_FRAMES ANIM_F(10)
/* Fruit Company's own durations, which a theme's motion keys scale. */
#define STOCK_OPEN_FRAMES ANIM_F(16)
#define STOCK_CLOSE_FRAMES ANIM_F(12)
#define DIM (UI_FULL_OPACITY * 35 / 100)
#define SMALLEST (UI_ULTRA_ONE * 92 / 100)

void ultra_sheet_begin(struct ui_sheet *sheet, bool open, const char *route)
{
    uint32_t now = USEC_TIMER;
    sheet->tint = ui_theme_rgb565(UI_COLOR_DIM);
    int from = sheet->starting ? sheet->tween.from :
               sheet->active ? MIN(MAX(sheet->shown, 0), UI_ULTRA_ONE) :
               sheet->tween.to;
    int to = open ? UI_ULTRA_ONE : 0;
    ui_sheet_cancel(sheet);
    sheet->tween = (struct ui_tween){
        .from = from, .to = to,
        .frames = lcd_active() && from != to
                    ? open ? ui_theme_motion(UI_MOTION_SHEET_IN, OPEN_FRAMES,
                                             STOCK_OPEN_FRAMES)
                           : ui_theme_motion(UI_MOTION_SHEET_OUT,
                                             CLOSE_FRAMES, STOCK_CLOSE_FRAMES)
                    : 0,
        .ease_in = !open,
    };
    sheet->shown = from;
    if (sheet->tween.frames)
    {
        sheet->active = sheet->starting = true;
        sheet->requested_at = now;
        cpu_boost(true);
        char trace_route[32];
        snprintf(trace_route, sizeof(trace_route), "%s-%s", route,
                 open ? "up" : "down");
        sheet->trace = ui_perf_begin(trace_route);
    }
}

static int ease_in(unsigned u, int power)
{
    int v = u;
    for (int i = 1; i < power; ++i)
        v = v * (int)u / UI_ULTRA_ONE;
    return v;
}

static struct ui_rect unite(struct ui_rect a, struct ui_rect b)
{
    if (a.width <= 0 || a.height <= 0)
        return b;
    if (b.width <= 0 || b.height <= 0)
        return a;
    int x = MIN(a.x, b.x), y = MIN(a.y, b.y);
    return (struct ui_rect){ x, y, MAX(a.x + a.width, b.x + b.width) - x,
                                   MAX(a.y + a.height, b.y + b.height) - y };
}

static struct ui_rect clip(struct ui_rect box, int width, int height)
{
    int x = MAX(box.x, 0), y = MAX(box.y, 0);
    return (struct ui_rect){ x, y,
        MAX(0, MIN(box.x + box.width, width) - x),
        MAX(0, MIN(box.y + box.height, height) - y) };
}

bool ultra_sheet_paint(struct ui_sheet *sheet, const fb_data *backdrop,
                       const fb_data *body, struct ui_rect bounds,
                       bool changed, fb_data *output, int width, int height,
                       void (*present)(void), unsigned frame)
{
    if (bounds.width <= 0 || bounds.height <= 0 || bounds.width > LCD_WIDTH)
        return false;
    uint32_t start = USEC_TIMER;
    const struct ui_tween *tween = &sheet->tween;
    unsigned u = frame * UI_ULTRA_ONE / tween->frames;
    bool open = !tween->ease_in;
    int place = ui_ultra_mix(tween->from, tween->to,
                             open ? ui_ultra_spring(u * 5 / 4) :
                                    ease_in(u, 3));
    int level = ui_ultra_mix(tween->from, tween->to,
                             ((open ? ui_ultra_out(u) : ease_in(u, 2)) +
                              (int)u) / 2);
    level = MIN(MAX(level, 0), UI_ULTRA_ONE);
    int light = UI_FULL_OPACITY - DIM * level / UI_ULTRA_ONE;
    /* A frame that moves nothing sends nothing, keeping its refresh for the
     * next. */
    if (!changed && place == sheet->shown && light == sheet->light)
        return true;
    int grown = MIN(MAX(place, 0), UI_ULTRA_ONE);
    int scale = SMALLEST + (UI_ULTRA_ONE - SMALLEST) * grown / UI_ULTRA_ONE;
    sheet->shown = place;

    int lift = (height - bounds.y) * (UI_ULTRA_ONE - place) / UI_ULTRA_ONE;
    int sw = MAX(1, bounds.width * scale / UI_ULTRA_ONE);
    int sh = MAX(1, bounds.height * scale / UI_ULTRA_ONE);
    struct ui_rect panel = { bounds.x + (bounds.width - sw) / 2,
                             bounds.y + lift + (bounds.height - sh) / 2,
                             sw, sh };
    struct ui_rect visible = clip(panel, width, height);
    struct ui_rect damage = light != sheet->light
        ? (struct ui_rect){ 0, 0, width, height }
        : unite(visible, sheet->previous);

    uint16_t column[LCD_WIDTH];
    unsigned step = ((unsigned)bounds.width << 16) / sw;
    for (int x = visible.x; x < visible.x + visible.width; ++x)
        column[x - visible.x] = ((x - panel.x) * step) >> 16;

    int right = damage.x + damage.width;
    for (int y = damage.y; damage.width && y < damage.y + damage.height; y++)
    {
        fb_data *row = output + y * width;
        int split = right, end = right;
        if (visible.width > 0 && y >= visible.y &&
            y < visible.y + visible.height)
        {
            split = visible.x;
            end = split + visible.width;
            const fb_data *source = body +
                (y - panel.y) * bounds.height / sh * bounds.width;
            fb_data *dst = row + split;
            for (int x = 0; x < visible.width; ++x)
                dst[x] = source[column[x]];
        }
        if (split > damage.x)
            ui_blit_tint(row + damage.x, backdrop + y * width + damage.x,
                         split - damage.x, light, sheet->tint);
        if (right > end)
            ui_blit_tint(row + end, backdrop + y * width + end, right - end,
                         light, sheet->tint);
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
            ui_mark_dirty_rect(SCREEN_MAIN, damage.x, damage.y, damage.width,
                               damage.height);
            ui_flush_dirty();
        }
    }
    return true;
}
