/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Predicts TE edges from the rises lcd-te-s5l8702.c observes, and sleeps a
 * thread to a microsecond deadline. */
#include "config.h"
#include "kernel.h"
#include "system.h"
#include "timer.h"
#include "lcd-te-s5l8702.h"
#include "lcd-vsync.h"

/* Beyond this the predicted phase may have drifted, and USEC_TIMER may have
 * wrapped. */
#define EDGE_MAX_AGE  (10 * HZ)
/* Spans shorter than this are dominated by the rise's polling jitter. */
#define REFINE_MIN    16u
#define REFINE_MAX    4096u

static uint32_t probed, period_q8, ref_rise, last_seen;
static long ref_tick;
static struct semaphore wake;
static bool wake_ready;

static void rebase(uint32_t rise, long tick)
{
    ref_rise = rise;
    ref_tick = tick;
}

/* Fits the period to the span between the reference rise and the latest,
 * a whole number of periods apart. */
static void refine(uint32_t period, uint32_t rise, long tick)
{
    if (period != probed)
    {
        probed = period;
        period_q8 = period << 8;
        last_seen = rise;
        rebase(rise, tick);
        return;
    }
    if (rise == last_seen)
        return;
    last_seen = rise;

    uint32_t span = rise - ref_rise;
    uint32_t estimate = (period_q8 + 128) >> 8;
    uint32_t n = (span + estimate / 2) / estimate;
    int64_t residual = ((int64_t)span << 8) - (int64_t)n * period_q8;
    if (tick - ref_tick > EDGE_MAX_AGE || !n || n > REFINE_MAX ||
        residual > (int64_t)period_q8 / 4 || residual < -(int64_t)period_q8 / 4)
    {
        rebase(rise, tick);
        return;
    }
    if (n >= REFINE_MIN)
        period_q8 = (period_q8 + (uint32_t)(((uint64_t)span << 8) / n) + 1) / 2;
}

bool lcd_vsync_timing(uint32_t *period, uint32_t *edge, bool *edge_known)
{
    uint32_t probe, rise;
    long tick;
    if (!lcd_te_timing(&probe, &rise, &tick))
        return false;
    /* The status bar's tick task reads the period too. */
    int irq = disable_irq_save();
    refine(probe, rise, tick);
    *period = period_q8;
    restore_irq(irq);
    *edge = rise;
    *edge_known = current_tick - tick <= EDGE_MAX_AGE;
    return true;
}

void lcd_vsync_last_transfer(uint32_t *wait, uint32_t *dma, unsigned int *count)
{
    lcd_te_last_transfer(wait, dma, count);
}

static void wake_fire(void)
{
    timer_stop();
    semaphore_release(&wake);
}

void lcd_vsync_sleep_until(uint32_t deadline)
{
    int32_t left = deadline - (uint32_t)USEC_TIMER;
    if (left <= 0)
        return;
    if (!wake_ready)
    {
        semaphore_init(&wake, 1, 0);
        wake_ready = true;
    }

    int ticks = 1 + (left - 1) / (1000000 / HZ);
    int irq = disable_irq_save();
    bool armed = timer_register(0, NULL, left * (TIMER_FREQ / 1000000),
                                wake_fire);
    restore_irq(irq);
    if (!armed)
    {
        sleep(ticks);
        return;
    }

    /* One tick of slack covers the fallback's tick rounding. */
    semaphore_wait(&wake, ticks + 1);
    irq = disable_irq_save();
    if (pfn_timer == wake_fire)
        timer_unregister();
    restore_irq(irq);
    semaphore_wait(&wake, TIMEOUT_NOBLOCK);
}
