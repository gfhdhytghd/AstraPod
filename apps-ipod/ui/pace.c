/* PodBox UI frame pacing. GPL version 2+. */
#include "config.h"
#include "pace.h"

#include <string.h>

#include "animation.h"
#include "button.h"
#include "hwscroll.h"
#include "kernel.h"
#include "lcd-vsync.h"
#ifdef HAVE_LCD_VSYNC
#include "lcd-te-s5l8702.h"
#endif
#include "system.h"

#define CLOCK_HZ 1000000u
/* 66.96 Hz, the iPod Classic panel's refresh. */
#define NOMINAL_PERIOD_Q8 3823178u
#define AUTHORED_PERIOD_Q8 ((CLOCK_HZ << 8) / UI_ANIM_FPS)
#define PACE_SITES 10
/* Frames over budget at full rate before an animation drops to half rate. */
#define LATE_LIMIT 2
#define RECOVERY_FRAMES 4
/* Wake so the submit reaches the driver this long before its TE window. */
#define WINDOW_LEAD_US 1500
/* Keeps a wake inside the frame it is for despite wake latency. */
#define WAKE_GUARD_US 2000

#if defined(HAVE_LCD_VSYNC) && defined(HAVE_BUTTON_FRAME_DEADLINE)
#define PRECISE_WAKE
#endif

bool ui_te_pacing = true;

static struct ui_pace sites[PACE_SITES];
static unsigned next_site;
static const struct ui_pace *last_pace;

static uint32_t period_q8(uint32_t *edge, bool *edge_known)
{
    *edge_known = false;
    if (!ui_te_pacing)
        return AUTHORED_PERIOD_Q8;
#ifdef HAVE_LCD_VSYNC
    uint32_t period;
    if (lcd_vsync_timing(&period, edge, edge_known))
        return period;
#else
    (void)edge;
#endif
    return NOMINAL_PERIOD_Q8;
}

static bool measured(void)
{
#ifdef HAVE_LCD_VSYNC
    uint32_t period, edge;
    bool known;
    return ui_te_pacing && lcd_vsync_timing(&period, &edge, &known);
#else
    return false;
#endif
}

uint32_t ui_frame_period_q8(void)
{
    uint32_t edge;
    bool known;
    return period_q8(&edge, &known);
}

uint32_t ui_frame_period(void)
{
    return (ui_frame_period_q8() + 128) >> 8;
}

static uint32_t phase_of(uint32_t clock, uint32_t edge, uint32_t period)
{
    int32_t since = clock - edge;
    uint64_t span = since < 0 ? -(int64_t)since : since;
    uint32_t phase = (span << 8) % period;
    if (since < 0 && phase)
        phase = period - phase;
    return phase >> 8;
}

uint32_t ui_clock_phase(uint32_t clock)
{
    uint32_t edge;
    bool known;
    uint32_t period = period_q8(&edge, &known);
    return known ? phase_of(clock, edge, period) : UINT32_MAX;
}

bool ui_present_refresh(uint32_t now, uint32_t *refresh)
{
    if (ui_hwscroll_latest(refresh))
        return true;
#ifdef HAVE_LCD_VSYNC
    uint32_t edge, phase, slot;
    bool known;
    uint32_t period = period_q8(&edge, &known);
    if (!known)
        return false;
    lcd_vsync_async_timing(NULL, &phase, &slot, NULL);
    if (lcd_te_pending())
    {
        *refresh = slot - phase_of(slot, edge, period) + ((period + 128) >> 8);
        return true;
    }
    if (phase == UINT32_MAX)
        return false;
    /* The transfer's phase counts from the rise before the one whose scan
     * it ran ahead of. */
    uint32_t whole = (period + 128) >> 8;
    bool ahead = phase >= whole;
    uint32_t started = ahead ? phase - whole : phase;
    uint32_t rise = now - phase_of(now, edge, period);
    if (now - rise < started)
        rise -= whole;
    *refresh = ahead ? rise : rise + whole;
    return true;
#else
    (void)now;
    (void)refresh;
    return false;
#endif
}

uint32_t ui_clock_start(void)
{
    uint32_t now = USEC_TIMER, edge;
    bool known;
    uint32_t period = period_q8(&edge, &known);
    return known ? now - phase_of(now, edge, period) : now;
}

static void begin(struct ui_pace *pace, uint32_t start, unsigned frame,
                  uint32_t period)
{
    /* Stay at half rate while the last run's transfers or work could not
     * fit one refresh; otherwise try full rate again. */
    uint32_t cost_limit = pace->async ? period * 15 / 16 : period * 3 / 4;
    bool half = pace->divisor == 2 &&
                ((!pace->async && pace->dma > period * 15 / 16) ||
                 pace->cost > cost_limit ||
                 pace->busy > period + period / 16);
    pace->divisor = ui_te_pacing && (half || pace->half_rate) ? 2 : 1;
    pace->start = start;
    pace->frame = frame;
    pace->late = pace->on_time = 0;
    pace->shown = pace->missed = 0;
    pace->dma = pace->busy = 0;
    pace->running = true;
}

#ifdef HAVE_LCD_VSYNC
/* Moves the wake so the next submit meets the driver's TE window just
 * after it arrives, instead of waiting there with the CPU. */
static void follow_window(struct ui_pace *pace, uint32_t wait, uint32_t period)
{
    int32_t limit = period / 8;
    int32_t error = (int32_t)wait - WINDOW_LEAD_US;
    if (error > (int32_t)period / 2)
        error -= period;
    int32_t step = MAX(-limit, MIN(limit, error / 2));
    int32_t offset = (int32_t)pace->offset + step;
    if (offset < 0)
        offset += period;
    if (offset > (int32_t)(period - WAKE_GUARD_US))
        offset = offset > (int32_t)(period - WAKE_GUARD_US / 2) ?
                 0 : (int32_t)(period - WAKE_GUARD_US);
    pace->offset = offset;
}
#endif

static void measure(struct ui_pace *pace, uint32_t cost, uint32_t period,
                    uint32_t busy)
{
    pace->cost = pace->shown ? (pace->cost * 3 + cost) / 4 : cost;
    uint32_t dma = 0;
#ifdef HAVE_LCD_VSYNC
    uint32_t wait;
    unsigned count;
    lcd_vsync_last_transfer(&wait, &dma, &count);
    if (count == pace->transfers || !ui_te_pacing)
        dma = 0;
    else if (!pace->async)
        follow_window(pace, wait, period);
    pace->transfers = count;
    /* The first reading may still be another screen's transfer. */
    if (pace->shown)
        pace->dma = MAX(pace->dma, dma);
    else
        dma = 0;
#endif
    pace->busy = busy;
    if (!ui_te_pacing)
        pace->offset = 0;
    else if (pace->half_rate)
        pace->late = pace->on_time = 0;
    else {
        bool late = cost > period * 15 / 16 ||
                    (!pace->async && dma > period * 15 / 16) ||
                    busy > period + period / 16;
        if (pace->divisor == 1 && late) {
            if (++pace->late >= LATE_LIMIT) {
                pace->divisor = 2;
                pace->on_time = 0;
            }
        } else if (pace->async && pace->recover_full_rate) {
            /* A wheel gesture can slow down without starting a new animation.
             * Require spare time for four draws before resuming every TE. */
            pace->late = 0;
            if (pace->divisor == 2 && !late && cost <= period * 7 / 8) {
                if (++pace->on_time >= RECOVERY_FRAMES) {
                    pace->divisor = 1;
                    pace->on_time = 0;
                }
            } else {
                pace->on_time = 0;
            }
        }
    }
    pace->shown++;
}

uint32_t ui_pace_next(struct ui_pace *pace, uint32_t start)
{
    uint32_t now = USEC_TIMER;
    uint32_t period = ui_frame_period();
    uint32_t busy = 0, next_slot = now;
    bool async = false;
#ifdef HAVE_LCD_VSYNC
    async = ui_te_pacing && lcd_vsync_async_timing(&busy, NULL, &next_slot, NULL);
#endif
    int32_t drift = start - pace->start;
    last_pace = pace;

    if (pace->async != async)
    {
        pace->async = async;
        pace->divisor = 1;
        pace->offset = pace->cost = 0;
        pace->running = false;
    }
    if (!pace->running || drift > (int32_t)period / 4 ||
        drift < -(int32_t)period / 4)
        begin(pace, start, ui_clock_frame(now - start), period);
    else if ((int32_t)(now - pace->due) < 0)
        return pace->due;
    else
        measure(pace, now - pace->due, period, busy);

    /* Render ahead of the next staging slot by the measured CPU cost. */
    uint32_t first = next_slot - MIN(pace->cost, period);
    if (!async || (int32_t)(first - now) <= 0)
        first = now;
    unsigned divisor = pace->divisor;
    int32_t since = first - pace->start - pace->offset;
    unsigned slot = since < 0 ? 0 : ui_clock_frame(since) + 1;
    unsigned next = pace->frame + divisor;
    if (slot > next)
        next += (slot - next + divisor - 1) / divisor * divisor;
    if (pace->shown)
        pace->missed += (next - pace->frame) / divisor - 1;
    pace->frame = next;
    pace->due = pace->start + ui_frame_clock(next) + pace->offset;
    return pace->due;
}

void ui_pace_get_status(struct ui_pace_status *status)
{
    memset(status, 0, sizeof(*status));
    status->period = ui_frame_period();
    status->enabled = ui_te_pacing;
    status->measured = measured();
    status->phased = ui_clock_phase(USEC_TIMER) != UINT32_MAX;
    if (!last_pace)
        return;
    status->offset = last_pace->offset;
    status->cost = last_pace->cost;
    status->dma = last_pace->dma;
    status->busy = last_pace->busy;
    status->async = last_pace->async;
    status->shown = last_pace->shown;
    status->missed = last_pace->missed;
    status->divisor = last_pace->divisor;
}

int ui_clock_wait_until(uint32_t deadline)
{
    int32_t left = deadline - (uint32_t)USEC_TIMER;
#ifdef PRECISE_WAKE
    if (left <= 0)
        return TIMEOUT_NOBLOCK;
    button_arm_frame_deadline(deadline);
    return 1 + (left - 1) / (CLOCK_HZ / HZ);
#else
    return left <= 0 ? TIMEOUT_NOBLOCK : 1 + (left - 1) / (CLOCK_HZ / HZ);
#endif
}

void ui_clock_sleep(int ticks)
{
#ifdef PRECISE_WAKE
    uint32_t deadline;
    if (button_take_frame_deadline(&deadline))
    {
        lcd_vsync_sleep_until(deadline);
        return;
    }
#endif
    if (ticks > 0)
        sleep(ticks);
    else
        yield();
}

void ui_clock_sleep_until(uint32_t deadline)
{
#ifdef SIMULATOR
    /* A simulator has no panel wake interrupt. Yield through the last
     * partial tick so presentation tests can measure individual refreshes. */
    int32_t left;
    while ((left = deadline - (uint32_t)USEC_TIMER) > 0)
    {
        long ticks = left / (CLOCK_HZ / HZ);
        if (ticks >= 2)
            sleep(ticks - 1);
        else
            yield();
    }
#else
    ui_clock_sleep(ui_clock_wait_until(deadline));
#endif
}

static struct ui_pace *site_pace(const void *site)
{
    for (unsigned i = 0; i < PACE_SITES; ++i)
    {
        if (sites[i].site == site)
            return &sites[i];
    }
    struct ui_pace *pace = &sites[next_site++ % PACE_SITES];
    memset(pace, 0, sizeof(*pace));
    pace->site = site;
    return pace;
}

static int clock_wait(const void *site, uint32_t elapsed, unsigned duration,
                      unsigned ticks_per_second, bool poll)
{
    if (ui_clock_frame(elapsed) >= duration)
        return 0;
    uint32_t start = (uint32_t)USEC_TIMER - elapsed;
    uint32_t deadline = ui_pace_next(site_pace(site), start);
    int32_t left = deadline - (uint32_t)USEC_TIMER;
#ifdef PRECISE_WAKE
    if (left <= 0 && poll)
        return TIMEOUT_NOBLOCK;
    /* A passed deadline makes the one-tick wait return at once. */
    button_arm_frame_deadline(deadline);
    if (left <= 0)
        return 1;
    return 1 + (uint64_t)(left - 1) * ticks_per_second / CLOCK_HZ;
#else
    int ticks = left <= 0 ? 0 : (uint64_t)left * ticks_per_second / CLOCK_HZ;
    return poll || ticks ? ticks : 1;
#endif
}

int ui_clock_poll_timeout(uint32_t elapsed, unsigned duration, unsigned ticks_per_second)
{
    return clock_wait(__builtin_return_address(0), elapsed, duration,
                      ticks_per_second, true);
}

int ui_clock_timeout(uint32_t elapsed, unsigned duration, unsigned ticks_per_second)
{
    return clock_wait(__builtin_return_address(0), elapsed, duration,
                      ticks_per_second, false);
}
