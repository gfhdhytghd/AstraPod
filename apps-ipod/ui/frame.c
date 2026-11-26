/* Framebuffer damage and submission for native UI. GPL version 2+. */
#include "config.h"
#include "plugins/rock_compat.h"
#include "frame.h"
#include "hwscroll.h"
#include "perf.h"
#include "system.h"
#include <stddef.h>

static struct ui_dirty_rect damage[NB_SCREENS];
static bool flush_inhibited;
static unsigned int scroll_holds;
static unsigned int flush_count, render_usec, flush_usec;

void ui_inhibit_flush(bool inhibit) { flush_inhibited = inhibit; }
bool ui_flush_inhibited(void) { return flush_inhibited; }

bool ui_hold_scrolling(bool hold)
{
    if (hold)
        ++scroll_holds;
    else if (scroll_holds)
        --scroll_holds;
    return scroll_holds == 0;
}

bool ui_scrolling_held(void) { return scroll_holds != 0; }

void ui_mark_dirty(enum screen_type screen)
{
    damage[screen] = (struct ui_dirty_rect) {
        0, 0, screens[screen].lcdwidth, screens[screen].lcdheight,
    };
}

void ui_mark_dirty_rect(enum screen_type screen, int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0)
        return;
    int left = MAX(x, 0);
    int top = MAX(y, 0);
    int right = MIN(x + w, screens[screen].lcdwidth);
    int bottom = MIN(y + h, screens[screen].lcdheight);
    if (right <= left || bottom <= top)
        return;

    struct ui_dirty_rect *region = &damage[screen];
    if (region->w > 0)
    {
        right = MAX(right, region->x + region->w);
        bottom = MAX(bottom, region->y + region->h);
        left = MIN(left, region->x);
        top = MIN(top, region->y);
    }
    *region = (struct ui_dirty_rect) { left, top, right - left, bottom - top };
}

bool ui_take_dirty(enum screen_type screen)
{
    bool dirty = damage[screen].w > 0;
    damage[screen].w = 0;
    return dirty;
}

int ui_take_dirty_rects(enum screen_type screen, struct ui_dirty_rect *out)
{
    *out = damage[screen];
    return ui_take_dirty(screen);
}

void ui_note_render(unsigned int usec)
{
    render_usec += usec;
    ui_perf_note_render(usec);
}
void ui_note_flush(unsigned int usec)
{
    flush_usec += usec;
    ++flush_count;
    ui_perf_note_submit(usec);
    ui_hwscroll_flushed();
}
unsigned int ui_flush_count(void) { return flush_count; }
unsigned int ui_render_usec(void) { return render_usec; }
unsigned int ui_flush_usec(void) { return flush_usec; }

void ui_flush_dirty(void)
{
    if (flush_inhibited || rock_ui_raw())
        return;

    FOR_NB_SCREENS(i)
    {
        if (i == SCREEN_MAIN && (lcd_updates_held() || !lcd_active()))
            continue;
        struct ui_dirty_rect region;
        if (ui_take_dirty_rects(i, &region))
        {
            struct viewport *previous = screens[i].set_viewport(NULL);
            unsigned int start = USEC_TIMER;
            ui_hwscroll_note_update();
            screens[i].update_rect(region.x, region.y, region.w, region.h);
            ui_note_flush(USEC_TIMER - start);
            screens[i].set_viewport(previous);
        }
    }
}
