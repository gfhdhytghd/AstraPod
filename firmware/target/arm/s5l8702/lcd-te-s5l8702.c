/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "config.h"
#include "kernel.h"
#include "lcd.h"
#include "lcd-experiment-s5l8702.h"
#include "lcd-s5l8702.h"
#include "lcd-te-s5l8702.h"
#include "system.h"
#include "s5l87xx.h"
#include "gpio-s5l8702.h"
#include <stdio.h>
#include <string.h>

#define TE_PORT           6
#define TE_BIT            7
#define TE_MASK           (1u << TE_BIT)
#define TE_MIN_US         13000u
#define TE_MAX_US         17000u
#define TE_JITTER_US        700u
#define TE_PROBE_US       (3 * TE_MAX_US + 1000u)
#define POLL_US           3u
#define POLL_YIELD_US     250u
#define POLL_MAX_GAP_US   32u
#define PHASE_GUARD_US    150
#define START_BUDGET_US   120u
#define SAMPLE_MARGIN_US  220u
#define SEARCH_US         240u
#define MAX_SAMPLE_GAP_US 600u
#define TE_HIGH_MAX_US    1800u
#define TE_LOST_PERIODS   3u

/* Include bus and FIFO time around the measured 23.47 ms at 0x33. */
#define FULL_DMA_FAST_US  22000
#define FULL_DMA_SLOW_US  25500

enum { GPIO_PORTS = 16, STATUS = GPIO_PORTS, INTCON, IRQ_RAW, SOURCE_COUNT };

struct edge_stats {
    uint32_t last_rise;
    uint32_t rises, falls, periods;
    uint32_t min_period, max_period, period_sum;
    uint32_t te_min, te_max, te_periods;
};

struct dma_stats {
    uint32_t samples, min, max, last;
    unsigned int width, height;
};

struct te_pulse {
    uint32_t period, high;
    bool valid;
};

static struct edge_stats gpio_edges[GPIO_PORTS][8];
static struct edge_stats lcd_edges[SOURCE_COUNT - GPIO_PORTS][32];
static uint32_t pinmux[GPIO_PORTS];
static struct te_pulse pulse;
static struct dma_stats full_dma, band_dma, rect_dma;
struct te_edge {
    uint32_t lower, upper;
    bool valid;
};

enum sample_mode { SAMPLE_PRE_EDGE, SAMPLE_POST_EDGE, SAMPLE_SEARCH };

static struct te_edge edge, previous_edge;
static uint32_t last_rise, sample_at, sample_time, stable_since;
static long last_rise_tick;
static enum sample_mode sample_mode;
static bool te_registered, sample_valid, sample_high, loss_confirmed;
static bool observed_high;
static unsigned int model_guard = PHASE_GUARD_US;
static uint32_t observed_high_width, last_any_rise;
static bool want_sync = true;
static volatile bool pending;
static bool deferred;
static int first_phase, last_phase;
static uint32_t admitted, slot_at, occupancy, last_copy, running_until;
static unsigned int copy_owner;
static unsigned int queued, deferred_count, untimed, no_window, trail_slots;
static unsigned int loss_starts, gpio_rises, timer_rises, missed_starts;
static uint32_t max_wait, max_irq_late, max_irq_bound, max_edge_error;
static uint32_t active_wait, last_wait, last_dma, dma_total;
static uint32_t irq_edge_at, irq_deadline;
static bool irq_edge_valid;
static uint32_t last_phase_us = UINT32_MAX;
static unsigned int transfers;
extern int lcd_type;

static void schedule(void);

bool lcd_te_supported(void)
{
    return lcd_type == 1;
}

bool lcd_te_enabled(void)
{
    return lcd_te_supported() && want_sync;
}

void lcd_te_set_enabled(bool enable)
{
    if (!lcd_te_supported())
        return;

    int flags = disable_irq_save();
    want_sync = enable;
    if (pending && lcd_experiment_irq_mode() != LCD_TE_IRQ_OFF)
        schedule();
    restore_irq(flags);
}

bool lcd_te_async_timing(uint32_t *busy, uint32_t *phase,
                         uint32_t *next_slot, uint32_t *copy)
{
    int flags = disable_irq_save();
    bool active = lcd_te_async_enabled();
    if (busy)
        *busy = occupancy;
    if (phase)
        *phase = last_phase_us;
    if (next_slot)
        *next_slot = pending ? slot_at : USEC_TIMER;
    if (copy)
        *copy = is_thread_context() && copy_owner == thread_self() ?
                last_copy : UINT32_MAX;
    restore_irq(flags);
    return active;
}

bool lcd_te_timing(uint32_t *period, uint32_t *rise, long *rise_tick)
{
    int flags = disable_irq_save();
    bool valid = lcd_te_supported() && pulse.valid && edge.valid;
    if (valid)
    {
        *period = pulse.period;
        *rise = last_rise;
        *rise_tick = last_rise_tick;
    }
    restore_irq(flags);
    return valid;
}

void lcd_te_last_transfer(uint32_t *wait, uint32_t *dma, unsigned int *count)
{
    int flags = disable_irq_save();
    *wait = last_wait;
    *dma = last_dma;
    *count = transfers;
    restore_irq(flags);
}

unsigned int lcd_te_bus_timing(void)
{
    return LCD_PHTIME_CLASSIC_DEFAULT;
}

bool lcd_te_set_bus_timing(unsigned int timing)
{
    return timing == LCD_PHTIME_CLASSIC_DEFAULT;
}

static uint32_t read_source(unsigned int source)
{
    switch (source)
    {
        case STATUS: return LCD_STATUS;
        case INTCON: return LCD_INTCON;
        case IRQ_RAW: return VIC0RAWINTR & (1u << IRQ_LCD);
        default: return PDAT(source) & 0xff;
    }
}

static bool frame_period(uint32_t period)
{
    return period >= TE_MIN_US && period <= TE_MAX_US;
}

/* PCON6[31:28] is normally 3. PDAT6 still exposes the signal in
 * that mode. The EIC mux routing is unverified, so leave the pin alone. */
static bool te_high(void)
{
    return (PDAT(TE_PORT) & TE_MASK) != 0;
}

static struct te_pulse measure_pulse(struct te_edge *rise_out)
{
    struct te_pulse found = { 0 };
    uint32_t now = USEC_TIMER, until = now + TE_PROBE_US;
    uint32_t previous_time = now, next_yield = now + POLL_YIELD_US;
    uint32_t rise = 0, high = 0, period_sum = 0, high_sum = 0;
    bool level = te_high(), seen_rise = false;
    unsigned int periods = 0;

    while (TIME_BEFORE((now = USEC_TIMER), until))
    {
        bool next = te_high();
        if (next != level)
        {
            bool timely = now - previous_time <= POLL_MAX_GAP_US;
            if (!next && seen_rise)
                high = timely ? now - rise : 0;
            if (next)
            {
                uint32_t period = now - rise;
                if (timely && seen_rise && frame_period(period))
                {
                    if (high > MAX_SAMPLE_GAP_US && high <= TE_HIGH_MAX_US)
                    {
                        if (periods &&
                            (MAX(period, found.period) -
                             MIN(period, found.period) > TE_JITTER_US))
                        {
                            periods = 0;
                            period_sum = high_sum = 0;
                        }
                        period_sum += period;
                        high_sum += high;
                        if (++periods == 2)
                        {
                            *rise_out = (struct te_edge){
                                previous_time, now, true
                            };
                            return (struct te_pulse){
                                period_sum / periods, high_sum / periods, true
                            };
                        }
                    }
                    else
                        periods = 0;
                    found.period = period;
                    found.high = high;
                }
                else
                    periods = 0;
                if (!periods)
                    period_sum = high_sum = 0;
                rise = now;
                high = 0;
                seen_rise = timely;
            }
            level = next;
        }
        previous_time = now;
        if (!TIME_BEFORE(now, next_yield))
        {
            yield();
            next_yield = USEC_TIMER + POLL_YIELD_US;
        }
        udelay(POLL_US);
    }
    return found;
}

static void timer_stop(void)
{
    THCMD = 0;
    TSTAT = 0x07;
}

static void timer_arm(uint32_t now, uint32_t deadline)
{
    timer_stop();
    THDATA0 = TIME_BEFORE(now, deadline) ? deadline - now : 1;
    THCMD = 3; /* clear and enable */
}

static void record_rise(struct te_edge next, uint32_t estimate)
{
    uint32_t period = estimate - last_any_rise;
    if (!pulse.valid && last_any_rise && frame_period(period) &&
        observed_high_width > MAX_SAMPLE_GAP_US &&
        observed_high_width <= TE_HIGH_MAX_US)
    {
        pulse = (struct te_pulse){period, observed_high_width, true};
        /* Bound one period error plus the two samples bounding TE high. */
        model_guard = 3 * MAX_SAMPLE_GAP_US;
    }
    last_any_rise = estimate;
    previous_edge = edge;
    if (current_tick - last_rise_tick > 2 + HZ / 20)
        previous_edge.valid = false;
    edge = next;
    last_rise = estimate;
    last_rise_tick = current_tick;
    sample_valid = loss_confirmed = false;
    observed_high = true;
    if (pulse.valid)
    {
        sample_mode = SAMPLE_PRE_EDGE;
        sample_at = estimate + pulse.period - SAMPLE_MARGIN_US;
    }
    else
    {
        sample_mode = SAMPLE_SEARCH;
        sample_at = next.upper + SEARCH_US;
    }
}

static int pending_y, pending_width, rows_left, rows_done;

static void start_complete(int rows, uint32_t started)
{
    running_until = started +
        (FULL_DMA_SLOW_US * rows * pending_width +
         LCD_WIDTH * LCD_HEIGHT - 1) / (LCD_WIDTH * LCD_HEIGHT);
    rows_done += rows;
    rows_left -= rows;
    pending = rows_left > 0;
}

static void te_irq_edge(enum lcd_te_irq_mode mode, uint32_t now)
{
    if (te_high() != (mode == LCD_TE_IRQ_RISE))
        return;

    irq_edge_at = now;
    irq_edge_valid = true;
    if (mode == LCD_TE_IRQ_RISE || pulse.valid)
    {
        last_rise = mode == LCD_TE_IRQ_RISE ? now : now - pulse.high;
        last_rise_tick = current_tick;
        edge.valid = true;
    }
    if (!pending || !want_sync)
        return;

    irq_deadline = now + TE_LOST_PERIODS * TE_MAX_US;
    uint32_t started;
    if (lcd_te_dma_busy() ||
        !lcd_te_start_dma(rows_done, rows_left, true,
                          now + START_BUDGET_US, &started))
    {
        lcd_experiment_irq_retry();
        schedule();
        return;
    }

    start_complete(rows_left, started);
    active_wait = started - admitted;
    max_wait = MAX(max_wait, active_wait);
    last_phase_us = started - last_rise;
    if (mode == LCD_TE_IRQ_FALL && !pulse.valid)
        last_phase_us = UINT32_MAX;
    lcd_experiment_irq_started(active_wait, started - now);
    timer_stop();
}

static void te_gpio_edge(struct eic_handler *handler)
{
    (void)handler;
    uint32_t now = USEC_TIMER;
    enum lcd_te_irq_mode mode = lcd_experiment_irq_mode();
    if (mode != LCD_TE_IRQ_OFF)
    {
        te_irq_edge(mode, now);
        return;
    }

    if (!te_high())
    {
        sample_valid = loss_confirmed = false;
        return;
    }
    if (edge.valid && now - edge.upper < TE_MIN_US / 2)
        return;

    uint32_t lower = now - TE_HIGH_MAX_US;
    if (sample_valid && !sample_high && now - sample_time < TE_HIGH_MAX_US)
        lower = sample_time;

    /* The GPIO controller latches an edge without capturing its time. */
    gpio_rises++;
    max_irq_bound = MAX(max_irq_bound, now - lower);
    uint32_t estimate = now;
    if (pulse.valid && edge.valid)
    {
        uint32_t predicted = last_rise + pulse.period;
        if (!TIME_BEFORE(predicted, lower) && !TIME_BEFORE(now, predicted))
        {
            estimate = predicted;
            max_irq_late = MAX(max_irq_late, now - predicted);
        }
    }
    record_rise((struct te_edge){lower, now, true}, estimate);
    schedule();
}

static struct eic_handler te_gpio = {
    .gpio_n = TE_PORT * 8 + TE_BIT,
    .type = EIC_INTTYPE_EDGE,
    .level = EIC_INTLEVEL_HIGH,
    .isr = te_gpio_edge,
};

static void register_te(void)
{
    /* PDAT exposes TE in its LCD mux; keep the board's pin setup. */
    THCON = (1 << 12) | (4 << 8) | (1 << 6);
    THPRE = 11; /* 12 MHz ECLK / (11 + 1) */
    timer_stop();
    lcd_experiment_irq_bind(&te_gpio);
    te_registered = true;
}

bool lcd_te_set_irq_mode(enum lcd_te_irq_mode mode)
{
    if (mode > LCD_TE_IRQ_FALL || !lcd_te_acquire_bus())
        return false;

    int flags = disable_irq_save();
    if (te_registered)
        eint_unregister(&te_gpio);
    lcd_experiment_irq_select(mode);
    edge.valid = previous_edge.valid = irq_edge_valid = false;
    sample_valid = loss_confirmed = observed_high = false;
    last_any_rise = observed_high_width = 0;
    last_phase_us = UINT32_MAX;
    register_te();
    restore_irq(flags);
    lcd_te_release_bus();
    return true;
}

void lcd_te_probe(void)
{
    if (!lcd_te_supported() || !is_thread_context() || !irq_enabled())
        return;

    struct te_edge rise = {0};
    struct te_pulse measured = measure_pulse(&rise);
    int flags = disable_irq_save();
    edge.valid = previous_edge.valid = false;
    sample_valid = loss_confirmed = observed_high = false;
    observed_high_width = last_any_rise = 0;
    sample_mode = SAMPLE_SEARCH;
    sample_at = USEC_TIMER;
    if (measured.valid)
    {
        pulse = measured;
        model_guard = PHASE_GUARD_US;
        record_rise(rise, rise.upper);
    }
    if (!te_registered)
        register_te();
    restore_irq(flags);
}

void lcd_te_stop(void)
{
    int flags = disable_irq_save();
    if (te_registered)
    {
        timer_stop();
        eint_unregister(&te_gpio);
        te_registered = false;
    }
    edge.valid = previous_edge.valid = irq_edge_valid = false;
    restore_irq(flags);
}

static void sample_pin(uint32_t now)
{
    bool high = te_high();
    bool close_sample = sample_valid &&
                        now - sample_time <= MAX_SAMPLE_GAP_US;
    bool same_level = close_sample && high == sample_high;

    if (high)
    {
        if (close_sample && !sample_high &&
            (!edge.valid || now - edge.upper >= TE_MIN_US / 2))
        {
            uint32_t estimate = sample_time + (now - sample_time) / 2;
            max_edge_error = MAX(max_edge_error, now - sample_time);
            timer_rises++;
            record_rise((struct te_edge){sample_time, now, true}, estimate);
        }
        else if (sample_mode != SAMPLE_PRE_EDGE || !pulse.valid ||
                 !edge.valid || !TIME_BEFORE(now, sample_at))
        {
            sample_mode = SAMPLE_SEARCH;
            sample_at = now + SEARCH_US;
        }
        observed_high = true;
    }
    else
    {
        if (observed_high && edge.valid && now - edge.lower <= TE_HIGH_MAX_US)
            observed_high_width = now - edge.lower;
        observed_high = false;
        if (sample_mode == SAMPLE_PRE_EDGE)
        {
            sample_mode = SAMPLE_POST_EDGE;
            sample_at = now + 2 * SAMPLE_MARGIN_US;
        }
        else
        {
            sample_mode = SAMPLE_SEARCH;
            sample_at = now + SEARCH_US;
        }
    }
    if (!same_level)
        stable_since = now;
    uint32_t period = pulse.valid ? pulse.period : TE_MAX_US;
    loss_confirmed = now - stable_since >= TE_LOST_PERIODS * period;
    sample_valid = true;
    sample_high = high;
    sample_time = now;
}

void lcd_te_dma_done(unsigned int width, unsigned int height, uint32_t usec)
{
    struct dma_stats *stats = width != LCD_WIDTH || height < LCD_HEIGHT / 4 ?
                              &rect_dma :
                              height == LCD_HEIGHT ? &full_dma : &band_dma;
    stats->min = stats->samples ? MIN(stats->min, usec) : usec;
    stats->max = MAX(stats->max, usec);
    stats->last = usec;
    last_wait = active_wait;
    last_dma = usec;
    dma_total += usec;
    transfers++;
    stats->width = width;
    stats->height = height;
    stats->samples++;
    if (pending)
        schedule();
}

uint32_t lcd_experiment_dma_total(void)
{
    return dma_total;
}

/* Trail each row of this scan and finish it before the next. Both
 * endpoint inequalities also hold for the intervening rows. */
static void phase_window(int y, int width, int height)
{
    int period = pulse.period;
    int active = period - pulse.high;
    int scan_top = pulse.high + active * y / LCD_HEIGHT;
    int scan_bottom = pulse.high + active * (y + height - 1) / LCD_HEIGHT;
    int row_time = (active + LCD_HEIGHT - 1) / LCD_HEIGHT;
    int pixels = width * height;
    int screen = LCD_WIDTH * LCD_HEIGHT;
    int fast = FULL_DMA_FAST_US * pixels / screen;
    int slow = (FULL_DMA_SLOW_US * pixels + screen - 1) / screen;
    int slow_row = (slow + height - 1) / height;

    first_phase = MAX(scan_top, scan_bottom - fast * (height - 1) / height)
                  + row_time + model_guard;
    last_phase = period + MIN(scan_top - slow_row, scan_bottom - slow)
                 - model_guard;
}

static bool start_window(struct te_edge bounds, uint32_t now,
                         uint32_t *open, uint32_t *close)
{
    if (!pulse.valid || !bounds.valid)
        return false;

    *open = bounds.upper + first_phase;
    *close = bounds.lower + last_phase;
    return now - bounds.lower < 2 * pulse.period &&
           TIME_BEFORE(*open + START_BUDGET_US, *close) &&
           TIME_BEFORE(now + START_BUDGET_US, *close);
}

static void mark_deferred(void)
{
    if (!deferred)
    {
        deferred = true;
        deferred_count++;
    }
}

static bool next_window(uint32_t now, uint32_t *open, uint32_t *close,
                        uint32_t *origin, int *rows)
{
    if (!pulse.valid)
        return false;

    *rows = rows_left;
    phase_window(pending_y + rows_done, pending_width, *rows);
    while (*rows > 1 && last_phase - first_phase <=
                        (int)(START_BUDGET_US + MAX_SAMPLE_GAP_US))
    {
        *rows /= 2;
        phase_window(pending_y + rows_done, pending_width, *rows);
    }
    if (start_window(previous_edge, now, open, close))
        *origin = previous_edge.upper;
    else if (start_window(edge, now, open, close))
        *origin = edge.upper;
    else
    {
        mark_deferred();
        return false;
    }
    return true;
}

/* Used only for the UI wake estimate. A DMA completion never starts
 * a queued frame in edge mode. */
static void schedule_irq(uint32_t now)
{
    uint32_t period = pulse.valid ? pulse.period : TE_MAX_US;
    uint32_t ready = now;
    if (lcd_te_dma_busy() && TIME_BEFORE(now, running_until))
        ready = running_until;

    slot_at = ready;
    if (irq_edge_valid && ready - irq_edge_at < TE_LOST_PERIODS * TE_MAX_US)
    {
        uint32_t elapsed = ready - irq_edge_at;
        slot_at = irq_edge_at + (elapsed / period + 1) * period;
    }
    /* Timer H is a watchdog here. Keep the UI slot estimate independent. */
    timer_arm(now, TIME_BEFORE(now, irq_deadline) ?
                  irq_deadline : now + SEARCH_US);
}

static void schedule(void)
{
    if (!pending)
    {
        if (te_registered)
            timer_stop();
        return;
    }

    uint32_t now = USEC_TIMER;
    if (want_sync && lcd_experiment_irq_mode() != LCD_TE_IRQ_OFF)
    {
        schedule_irq(now);
        return;
    }

    uint32_t next = sample_at;
    uint32_t open, close, origin;
    int rows;
    bool busy = lcd_te_dma_busy();
    if (!want_sync || loss_confirmed)
        next = busy ? now + SEARCH_US : now + 1;
    else if (busy)
        mark_deferred();
    else if (next_window(now, &open, &close, &origin, &rows))
    {
        if (TIME_BEFORE(open, next))
            next = open;
        slot_at = TIME_BEFORE(now, open) ? open : now;
        timer_arm(now, next);
        return;
    }

    slot_at = TIME_BEFORE(now, next) ? next : now;
    if (busy && TIME_BEFORE(slot_at, running_until))
        slot_at = running_until;
    timer_arm(now, next);
}

void INT_TIMERH(void)
{
    timer_stop();
    uint32_t now = USEC_TIMER;
    if (!pending)
        return;

    bool edge_mode = want_sync &&
                     lcd_experiment_irq_mode() != LCD_TE_IRQ_OFF;
    if (edge_mode)
    {
        if (TIME_BEFORE(now, irq_deadline))
        {
            schedule_irq(now);
            return;
        }
    }
    else if (want_sync && !TIME_BEFORE(now, sample_at))
        sample_pin(now);

    if (!lcd_te_dma_busy())
    {
        uint32_t open, close, origin, started;
        int rows = rows_left;
        bool timed = want_sync && !loss_confirmed && !edge_mode;
        bool start = !timed;
        if (timed && next_window(now, &open, &close, &origin, &rows))
            start = !TIME_BEFORE(now, open);

        if (start && lcd_te_start_dma(rows_done, rows, timed,
                                      timed ? close : 0, &started))
        {
            start_complete(rows, started);
            if (timed)
            {
                trail_slots++;
                last_phase_us = started - origin;
            }
            else
            {
                lcd_te_untimed_start();
                if (want_sync)
                    loss_starts++;
                if (edge_mode)
                    lcd_experiment_irq_fallback();
            }
            active_wait = started - admitted;
            if (rows_done == rows)
                max_wait = MAX(max_wait, active_wait);
            if (pending)
                schedule();
            return;
        }
        if (start)
        {
            if (timed)
                missed_starts++;
            mark_deferred();
            /* Retry after a late command/descriptor setup or FIFO drain. */
            timer_arm(USEC_TIMER, USEC_TIMER + START_BUDGET_US);
            return;
        }
    }
    schedule();
}

void lcd_te_submit(int y, int width, int height, uint32_t copy)
{
    uint32_t now = USEC_TIMER;
    if (now - sample_time > MAX_SAMPLE_GAP_US)
        sample_valid = loss_confirmed = false;
    if (current_tick - last_rise_tick > 2 + HZ / 20)
        edge.valid = previous_edge.valid = false;
    if (!edge.valid || !TIME_BEFORE(now, sample_at))
    {
        sample_mode = SAMPLE_SEARCH;
        sample_at = now;
    }
    pending_y = y;
    pending_width = width;
    rows_done = 0;
    rows_left = height;
    pending = true;
    deferred = false;
    admitted = now;
    irq_deadline = now + TE_LOST_PERIODS * TE_MAX_US;
    last_copy = copy;
    copy_owner = thread_self();
    occupancy = (FULL_DMA_SLOW_US * width * height +
                 LCD_WIDTH * LCD_HEIGHT - 1) / (LCD_WIDTH * LCD_HEIGHT);
    if (want_sync && pulse.valid)
        occupancy = MAX(occupancy, pulse.period);
    if (pulse.valid)
    {
        phase_window(y, width, height);
        if (first_phase + (int)START_BUDGET_US >= last_phase)
            no_window++;
    }
    queued++;
    schedule();
}

bool lcd_te_pending(void)
{
    return pending;
}

void lcd_te_cancel(void)
{
    pending = false;
    schedule();
}

void lcd_te_untimed_start(void)
{
    if (!pending)
        active_wait = 0;
    untimed++;
    last_phase_us = UINT32_MAX;
}

static struct edge_stats *source_edges(unsigned int source)
{
    return source < GPIO_PORTS ?
        gpio_edges[source] : lcd_edges[source - GPIO_PORTS];
}

static unsigned int source_bits(unsigned int source)
{
    return source < GPIO_PORTS ? 8 : 32;
}

static void count_edges(unsigned int source, uint32_t changed, uint32_t value,
                        uint32_t now)
{
    while (changed)
    {
        unsigned int bit = __builtin_ctz(changed);
        uint32_t mask = 1u << bit;
        changed &= ~mask;
        struct edge_stats *stats = &source_edges(source)[bit];
        if (!(value & mask))
        {
            stats->falls++;
            continue;
        }
        if (stats->rises++)
        {
            uint32_t period = now - stats->last_rise;
            stats->periods++;
            stats->period_sum += period;
            stats->min_period = MIN(stats->min_period, period);
            stats->max_period = MAX(stats->max_period, period);
            if (frame_period(period))
            {
                stats->te_periods++;
                stats->te_min = MIN(stats->te_min, period);
                stats->te_max = MAX(stats->te_max, period);
            }
        }
        stats->last_rise = now;
    }
}

static unsigned int sample_edges(uint32_t *elapsed, uint32_t *max_gap)
{
    uint32_t previous[SOURCE_COUNT];
    memset(gpio_edges, 0, sizeof(gpio_edges));
    memset(lcd_edges, 0, sizeof(lcd_edges));
    for (unsigned int source = 0; source < SOURCE_COUNT; source++)
    {
        previous[source] = read_source(source);
        struct edge_stats *stats = source_edges(source);
        for (unsigned int bit = 0; bit < source_bits(source); bit++)
            stats[bit].min_period = stats[bit].te_min = UINT32_MAX;
    }
    for (unsigned int port = 0; port < GPIO_PORTS; port++)
        pinmux[port] = PCON(port);

    uint32_t start = USEC_TIMER, now = start, previous_time = start;
    uint32_t next_yield = start + 4000;
    unsigned int samples = 0;
    while (now - start < 1000000u)
    {
        for (unsigned int source = 0; source < SOURCE_COUNT; source++)
        {
            uint32_t value = read_source(source);
            uint32_t changed = value ^ previous[source];
            if (changed)
                count_edges(source, changed, value, USEC_TIMER);
            previous[source] = value;
        }
        ++samples;
        if (!TIME_BEFORE(now, next_yield))
        {
            yield();
            next_yield = USEC_TIMER + 4000;
        }
        now = USEC_TIMER;
        *max_gap = MAX(*max_gap, now - previous_time);
        previous_time = now;
    }
    *elapsed = now - start;
    return samples;
}

static void report_edges(int (*report)(const char *fmt, ...), bool qualified)
{
    for (unsigned int source = 0; source < SOURCE_COUNT; source++)
    {
        for (unsigned int bit = 0; bit < source_bits(source); bit++)
        {
            const struct edge_stats *stats = &source_edges(source)[bit];
            if (!stats->rises && !stats->falls)
                continue;
            int mux = source < GPIO_PORTS ?
                (int)((pinmux[source] >> (bit * 4)) & 0xf) : -1;
            bool candidate = source == TE_PORT && bit == TE_BIT && qualified;
            char gpio_name[12];
            const char *name = source == STATUS ? "LCD_STATUS" :
                source == INTCON ? "LCD_INTCON" : "VIC0RAWINTR";
            if (source < GPIO_PORTS)
            {
                snprintf(gpio_name, sizeof(gpio_name), "PDAT%u", source);
                name = gpio_name;
            }
            report("%s[%u]%s rise=%lu fall=%lu mux=%d", name, bit,
                   candidate ? " TE candidate" : "",
                   (unsigned long)stats->rises,
                   (unsigned long)stats->falls, mux);
            if (stats->periods)
                report("  us min=%lu avg=%lu max=%lu in-band=%lu/%lu",
                       (unsigned long)stats->min_period,
                       (unsigned long)(stats->period_sum / stats->periods),
                       (unsigned long)stats->max_period,
                       (unsigned long)stats->te_periods,
                       (unsigned long)stats->periods);
            if (stats->te_periods && stats->te_periods != stats->periods)
                report("  in-band min=%lu max=%lu us",
                       (unsigned long)stats->te_min,
                       (unsigned long)stats->te_max);
        }
    }
}

static void report_dma(int (*report)(const char *fmt, ...), const char *name,
                       const struct dma_stats *stats)
{
    report("DMA %s samples=%u last=%ux%u %u us", name, stats->samples,
           stats->width, stats->height, stats->last);
    if (stats->samples)
        report("  min=%u max=%u us", stats->min, stats->max);
}

bool lcd_te_run_diagnostic(int (*report)(const char *fmt, ...), bool survey)
{
    if (!lcd_te_acquire_bus())
    {
        report("LCD unavailable (type, power or DMA busy)");
        return false;
    }

    /* Measure a fresh pulse without changing the active scheduler. */
    struct te_edge measured_rise;
    struct te_pulse measured = measure_pulse(&measured_rise);
    bool sync_on = lcd_te_enabled();
    bool async_on = lcd_te_async_enabled();

    uint32_t elapsed = 0, max_gap = 0;
    unsigned int samples = 0;
    if (survey)
        samples = sample_edges(&elapsed, &max_gap);
    else
        pinmux[TE_PORT] = PCON(TE_PORT);
    struct dma_stats full = full_dma, band = band_dma, rect = rect_dma;
    int flags = disable_irq_save();
    unsigned int trail = trail_slots, misses = missed_starts;
    unsigned int no_slot = no_window, accepted = queued, held = deferred_count;
    unsigned int bypasses = untimed, lost = loss_starts;
    unsigned int gpio = gpio_rises, sampled = timer_rises;
    uint32_t irq_late = max_irq_late, irq_bound = max_irq_bound;
    uint32_t sample_bound = max_edge_error, worst_wait = max_wait;
    restore_irq(flags);
    unsigned int timing = LCD_PHTIME;
    unsigned int status = LCD_STATUS;
    lcd_te_release_bus();

    report("TE candidates=%u | sync=%s | async=%s", measured.valid,
           sync_on ? "on" : "off", async_on ? "on" : "off");
    report("LCD type 1 | panel readback unavailable");
    report("LCD_PHTIME=0x%02x | DMA samples since boot", timing);
    report("LCD_STATUS after DMA=0x%08x", status);
    report("TE pin %s | PDAT6[7] mux=%u",
           measured.valid ? "selected" : "unqualified", pinmux[TE_PORT] >> 28);
    if (measured.period)
        report("TE period=%u us high=%u us low=%u us",
               measured.period, measured.high,
               measured.high ? measured.period - measured.high : 0);
    else
        report("TE period/pulse unavailable");
    if (measured.valid)
    {
        unsigned int millihz = (1000000000u + measured.period / 2) /
                               measured.period;
        report("TE refresh=%u.%03u Hz | 60 Hz period=16667 us",
               millihz / 1000, millihz % 1000);
    }
    report("TE phase assumes active-high blanking, top-to-bottom scan");
    report("TE trail=%u start-retry=%u no-window=%u", trail, misses, no_slot);
    report("LCD queued=%u coalesced=0 deferred=%u untimed=%u te-lost=%u",
           accepted, held, bypasses, lost);
    report("LCD max queue wait=%u us", worst_wait);
    report("TE GPIO rises=%u timer rises=%u", gpio, sampled);
    report("TE IRQ latency estimate max=%u us bound=%u us",
           irq_late, irq_bound);
    report("TE timer edge uncertainty max=%u us", sample_bound);
    report_dma(report, "full", &full);
    report_dma(report, "band (320px wide, 60-239 rows)", &band);
    report_dma(report, "other rect", &rect);
    lcd_experiment_report(report);
    if (!survey)
    {
        report("Signals: survey skipped");
        return true;
    }
    report("Signals: %u passes in %lu us (max gap %lu us)", samples,
           (unsigned long)elapsed, (unsigned long)max_gap);
    report_edges(report, measured.valid);
    return true;
}
