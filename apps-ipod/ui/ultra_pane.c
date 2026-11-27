/***************************************************************************
 * PodBox Fruit Company Ultra menu pane motion. GPL version 2+.
 *
 * Under Ultra a new row's still pane dissolves in over the last one rather
 * than replacing it. Both panes wait in one buffer, and each step mixes
 * them afresh over only the rectangle where they differ, so a repaint of
 * the pane in the meantime is mixed again rather than lost.
 ****************************************************************************/

#include <string.h>
#include "config.h"
#include "core_alloc.h"
#include "system.h"
#include "kernel.h"
#include "lcd.h"
#include "animation.h"
#include "frame.h"
#include "perf.h"
#include "ultra_pane.h"
#include "ultra_scene.h"

/* The dissolve's length. It presents at every second refresh, as the
 * pane's other motion does. */
#define DISSOLVE_US 300000u

static struct {
    int handle;
    struct ui_rect pane, area;
    uint32_t started, stepped;
    int level;              /* the mix on screen, of 32; -1 before it */
    unsigned trace;
} fade;

static fb_data *fb_at(int x, int y)
{
    return lcd_framebuffer_default.fb_ptr + y * LCD_WIDTH + x;
}

static void finish(void)
{
    if (fade.handle <= 0)
        return;
    fade.handle = core_free(fade.handle);
    cpu_boost(false);
    ui_perf_end(fade.trace);
    fade.trace = 0;
}

void ultra_pane_cancel(void)
{
    finish();
}

bool ultra_pane_active(void)
{
    return fade.handle > 0;
}

/* The held panes: the old one first, then the new. */
static fb_data *pane_at(fb_data *buffer, bool now, int x, int y)
{
    return buffer + (now ? fade.pane.width * fade.pane.height : 0) +
           (y - fade.pane.y) * fade.pane.width + (x - fade.pane.x);
}

/* Copy the pane on screen into the old or the new buffer. */
static void keep(bool now)
{
    fb_data *buffer = core_get_data_pinned(fade.handle);
    for (int y = fade.pane.y; y < fade.pane.y + fade.pane.height; y++)
        memcpy(pane_at(buffer, now, fade.pane.x, y), fb_at(fade.pane.x, y),
               fade.pane.width * sizeof(fb_data));
    core_put_data_pinned(buffer);
}

bool ultra_pane_hold(struct ui_rect pane)
{
    finish();
    fade.handle = core_alloc_free_space(2 * pane.width * pane.height *
                                        sizeof(fb_data));
    if (fade.handle <= 0)
        return false;
    fade.pane = pane;
    keep(false);
    return true;
}

/* The bounding rectangle of the pixels where the two panes differ, in
 * whole pixel pairs. */
static struct ui_rect changed(fb_data *buffer)
{
    struct ui_rect p = fade.pane;
    int x0 = p.x + p.width, x1 = -1, y0 = p.y + p.height, y1 = -1;
    for (int y = p.y; y < p.y + p.height; y++)
    {
        const fb_data *was = pane_at(buffer, false, p.x, y);
        const fb_data *now = pane_at(buffer, true, p.x, y);
        for (int x = 0; x < p.width; x++)
            if (was[x] != now[x])
            {
                x0 = MIN(x0, p.x + x);
                x1 = MAX(x1, p.x + x);
                y0 = MIN(y0, y);
                y1 = MAX(y1, y);
            }
    }
    if (x1 < x0)
        return (struct ui_rect){ 0 };
    x0 &= ~1;
    x1 |= 1;
    return (struct ui_rect){ x0, y0, x1 - x0 + 1, y1 - y0 + 1 };
}

/* The part of the changed area inside `clip`, `weight` 32nds of the way
 * from the old pane to the new. */
static ULTRA_ARM void blend(struct ui_rect clip, unsigned weight)
{
    struct ui_rect a = fade.area;
    int x0 = MAX(a.x, clip.x & ~1);
    int x1 = MIN(a.x + a.width, (clip.x + clip.width + 1) & ~1);
    int y0 = MAX(a.y, clip.y), y1 = MIN(a.y + a.height, clip.y + clip.height);
    if (x1 <= x0 || y1 <= y0)
        return;
    fb_data *buffer = core_get_data_pinned(fade.handle);
    for (int y = y0; y < y1; y++)
    {
        fb_data *row = fb_at(x0, y);
        const fb_data *was = pane_at(buffer, false, x0, y);
        const fb_data *now = pane_at(buffer, true, x0, y);
        if (weight >= 32)
        {
            memcpy(row, now, (x1 - x0) * sizeof(*row));
            continue;
        }
        for (int x = 0; x < x1 - x0; x += 2)
        {
            uint32_t a = *(const ultra_pair *)(now + x);
            uint32_t b = *(const ultra_pair *)(was + x);
            *(ultra_pair *)(row + x) =
                a == b ? a : ultra_mix_pair(a, b, weight);
        }
    }
    core_put_data_pinned(buffer);
}

struct ui_rect ultra_pane_begin(void)
{
    if (fade.handle <= 0)
        return (struct ui_rect){ 0 };
    keep(true);
    fb_data *buffer = core_get_data_pinned(fade.handle);
    fade.area = changed(buffer);
    core_put_data_pinned(buffer);
    if (fade.area.width <= 0)
    {
        fade.handle = core_free(fade.handle);
        return fade.area;
    }
    fade.started = USEC_TIMER;
    fade.level = -1;
    fade.trace = ui_perf_begin_passive("ultra-pane");
    cpu_boost(true);
    return ultra_pane_step();
}

struct ui_rect ultra_pane_step(void)
{
    if (fade.handle <= 0)
        return (struct ui_rect){ 0 };
    uint32_t now = USEC_TIMER;
    uint32_t t = now - fade.started;
    int level = t >= DISSOLVE_US ? 32 : (int)(t * 32 / DISSOLVE_US);
    if (level == fade.level)
        return (struct ui_rect){ 0 };
    uint32_t start = USEC_TIMER;
    blend(fade.area, level);
    ui_note_render(USEC_TIMER - start);
    fade.level = level;
    fade.stepped = now;
    struct ui_rect area = fade.area;
    if (level >= 32)
        finish();
    return area;
}

void ultra_pane_repaint(struct ui_rect clip)
{
    if (fade.handle > 0 && fade.level >= 0)
        blend(clip, fade.level);
}

int ultra_pane_deadline(void)
{
    return ui_clock_wait_until(fade.stepped + 2 * ui_frame_period());
}
