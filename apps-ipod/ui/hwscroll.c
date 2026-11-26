/* Panel hardware scroll for native screens. GPL version 2+.
 *
 * ui_hwscroll_submit() sends the damage above the area and then the frame,
 * and accounts for both as one submission. The panel behind it is the
 * LCD driver on the iPod Classic, an emulation over the SDL window in the
 * simulator (apps-ipod/sim/shim-hwscroll.c), and nothing on other
 * players. */
#include "config.h"
#include "hwscroll.h"
#include "frame.h"
#include "perf.h"
#include "kernel.h"
#include "lcd.h"
#include "lcd-vsync.h"
#include "system.h"
#include "draw/screen_access.h"
#include "plugins/rock_compat.h"
#ifdef HAVE_LCD_VSYNC
#include "lcd-hwscroll.h"
#endif

bool hwscroll_panel_present(const struct ui_hwscroll_frame *frame);
bool hwscroll_panel_presented(uint32_t *rise);
unsigned int hwscroll_panel_writes(void);
int hwscroll_panel_offset(void);
void hwscroll_panel_flushed(void);

#ifdef HAVE_LCD_VSYNC
#if UI_HWSCROLL_BANDS > LCD_HWSCROLL_BANDS
#error "more bands than the LCD driver takes"
#endif

bool hwscroll_panel_present(const struct ui_hwscroll_frame *frame)
{
    struct lcd_hwscroll_frame panel = {
        .top = frame->top, .shift = frame->shift, .count = frame->count,
    };
    for (int i = 0; i < frame->count; ++i)
    {
        const struct ui_rect *band = &frame->band[i];
        panel.band[i] = (struct lcd_hwscroll_band){
            band->x, band->y, band->width, band->height,
        };
    }
    /* The driver holds one frame at a time. */
    uint32_t start = USEC_TIMER, limit = 2 * ui_frame_period();
    while (lcd_hwscroll_pending())
    {
        if (USEC_TIMER - start > limit)
            return false;
        yield();
    }
    return lcd_hwscroll_present(&panel);
}

bool hwscroll_panel_presented(uint32_t *rise)
{
    return lcd_hwscroll_presented(rise);
}

unsigned int hwscroll_panel_writes(void)
{
    return lcd_hwscroll_writes();
}

int hwscroll_panel_offset(void)
{
    return lcd_hwscroll_offset();
}

void hwscroll_panel_flushed(void)
{
}
#elif CONFIG_PLATFORM & PLATFORM_NATIVE
bool hwscroll_panel_present(const struct ui_hwscroll_frame *frame)
{
    (void)frame;
    return false;
}

bool hwscroll_panel_presented(uint32_t *rise)
{
    (void)rise;
    return false;
}

unsigned int hwscroll_panel_writes(void)
{
    return 0;
}

int hwscroll_panel_offset(void)
{
    return 0;
}

void hwscroll_panel_flushed(void)
{
}
#endif

static bool latest;
static unsigned int latest_writes;
static unsigned int framebuffers, framebuffer_writes;

bool ui_hwscroll_submit(const struct ui_hwscroll_frame *frame,
                        const struct ui_rect *other)
{
    if (ui_flush_inhibited() || rock_ui_raw() || lcd_updates_held() ||
        !lcd_active())
        return false;
    bool before = other->width > 0 && other->height > 0;
    if (before && other->y + other->height > frame->top)
        return false;

    uint32_t start = USEC_TIMER;
    latest = false;
    if (before)
    {
        struct viewport *previous = screens[SCREEN_MAIN].set_viewport(NULL);
        screens[SCREEN_MAIN].update_rect(other->x, other->y, other->width,
                                         other->height);
        screens[SCREEN_MAIN].set_viewport(previous);
    }
    if (!hwscroll_panel_present(frame))
    {
        if (before)
            ui_note_flush(USEC_TIMER - start);
        return false;
    }
    latest = true;
    latest_writes = hwscroll_panel_writes();

    unsigned int pixels = 0;
    for (int i = 0; i < frame->count; ++i)
        pixels += frame->band[i].width * frame->band[i].height;
    ui_perf_note_hwscroll(hwscroll_panel_offset(),
                          (pixels + LCD_WIDTH - 1) / LCD_WIDTH);
    ui_note_flush(USEC_TIMER - start);
    return true;
}

bool ui_hwscroll_latest(uint32_t *rise)
{
    return latest && hwscroll_panel_writes() == latest_writes &&
           hwscroll_panel_presented(rise);
}

void ui_hwscroll_note_update(void)
{
    latest = false;
}

unsigned int ui_hwscroll_writes(void)
{
    return hwscroll_panel_writes();
}

int ui_hwscroll_offset(void)
{
    return hwscroll_panel_offset();
}

void ui_hwscroll_flushed(void)
{
    hwscroll_panel_flushed();
}

void ui_hwscroll_note_framebuffer(void)
{
    ++framebuffers;
    framebuffer_writes = hwscroll_panel_writes();
}

unsigned int ui_hwscroll_framebuffers(unsigned int *writes)
{
    *writes = framebuffer_writes;
    return framebuffers;
}
