/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "config.h"
#include "kernel.h"
#include "lcd.h"
#include "lcd-s5l8702.h"
#include "lcd-te-s5l8702.h"
#include "system.h"
#include "s5l87xx.h"
#include <stdio.h>
#include <string.h>

#define TE_PORT           6
#define TE_BIT            7
#define TE_MASK           (1u << TE_BIT)
#define TE_MIN_US         13000u
#define TE_MAX_US         17000u
#define TE_JITTER_US        700u
#define TE_PROBE_US       (3 * TE_MAX_US + 1000u)
#define SYNC_WAIT_US      20000u
#define POLL_US           3u
#define POLL_YIELD_US     250u
#define POLL_MAX_GAP_US   32u
#define PHASE_GUARD_US    50
/* Bit 1 is set when the FIFO accepts data; bit 4 is set while it drains,
 * the same pair lcd-s5l8702.c polls before a command. */
#define LCD_READY_BIT    (1u << 1)
#define LCD_BUSY_BIT     (1u << 4)
#define LCD_IDLE_WAIT_US  5000u

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
static unsigned int bus_timing = LCD_PHTIME_CLASSIC_DEFAULT;
static uint32_t last_rise;
static long last_rise_tick;
static bool have_rise;
static bool want_sync = true;
static unsigned int lead_slots, trail_slots, timed_out, no_window;
static uint32_t max_wait;
extern int lcd_type;

bool lcd_te_supported(void)
{
    return lcd_type == 1;
}

bool lcd_te_enabled(void)
{
    return lcd_te_supported() && want_sync && pulse.valid;
}

void lcd_te_set_enabled(bool enable)
{
    if (lcd_te_supported() && (!enable || pulse.valid))
        want_sync = enable;
}

unsigned int lcd_te_bus_timing(void)
{
    return bus_timing;
}

bool lcd_te_set_bus_timing(unsigned int timing)
{
    if (timing == bus_timing)
        return true;
    if (!lcd_te_acquire_bus())
        return false;

    /* DMA completion can precede the last bytes leaving the LCD FIFO. */
    uint32_t until = USEC_TIMER + LCD_IDLE_WAIT_US;
    while ((LCD_STATUS & (LCD_READY_BIT | LCD_BUSY_BIT)) != LCD_READY_BIT)
    {
        if (!TIME_BEFORE(USEC_TIMER, until))
        {
            lcd_te_release_bus();
            return false;
        }
        yield();
    }

    /* S5L8700X spec 26-10 to 26-12: [7:4] PH1_TIME and [3:0]
     * PH2_TIME set the two strobe pulse widths to (value + 1) clock
     * units. The phase polarity and clock unit on S5L8702 are unverified. */
    LCD_PHTIME = timing;
    bus_timing = timing;
    full_dma = band_dma = rect_dma = (struct dma_stats){0};
    lead_slots = trail_slots = timed_out = no_window = max_wait = 0;
    lcd_te_release_bus();
    return true;
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

void lcd_te_probe(void)
{
    if (!lcd_te_supported() || !is_thread_context() || !irq_enabled())
        return;

    struct te_pulse found = { 0 };
    have_rise = false;
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
                    if (high && high < period / 2)
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
                            pulse = (struct te_pulse){
                                period_sum / periods, high_sum / periods, true
                            };
                            last_rise = now;
                            last_rise_tick = current_tick;
                            have_rise = true;
                            return;
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
    pulse = found;
}

void lcd_te_dma_done(unsigned int width, unsigned int height, uint32_t usec)
{
    struct dma_stats *stats = width != LCD_WIDTH || height < LCD_HEIGHT / 4 ?
                              &rect_dma :
                              height == LCD_HEIGHT ? &full_dma : &band_dma;
    stats->min = stats->samples ? MIN(stats->min, usec) : usec;
    stats->max = MAX(stats->max, usec);
    stats->last = usec;
    stats->width = width;
    stats->height = height;
    stats->samples++;
}

/* DCS TE mode 0: high denotes vertical blanking. Assume the panel
 * scans logical rows top-to-bottom, like the DMA write. Bound the
 * first and last rows, using the measured full-frame rate per pixel. */
static bool phase_window(int y, int width, int height, int *first, int *last)
{
    int period = pulse.period;
    int active = period - pulse.high;
    int scan_top = pulse.high + active * y / LCD_HEIGHT;
    int scan_bottom = pulse.high + active * (y + height - 1) / LCD_HEIGHT;
    int row_time = (active + LCD_HEIGHT - 1) / LCD_HEIGHT;
    int pixels = width * height;
    int screen = LCD_WIDTH * LCD_HEIGHT;

    /* Avoid scaling a stalled transfer or extrapolating across two scans. */
    if (!full_dma.samples || full_dma.max >= 2 * pulse.period)
        return false;

    /* Both measured durations are below two refresh periods. */
    int fast = full_dma.min * pixels / screen;
    int slow = (full_dma.max * pixels + screen - 1) / screen;
    int slow_row = (slow + height - 1) / height;
    int lead_end = MIN(scan_top - slow_row, scan_bottom - slow);

    if (slow < period && lead_end >= PHASE_GUARD_US)
    {
        *first = 0;
        *last = lead_end - PHASE_GUARD_US;
        return true;
    }

    /* Trail this scan but finish each row ahead of the following scan. */
    *first = MAX(scan_top, scan_bottom - fast * (height - 1) / height)
             + row_time + PHASE_GUARD_US;
    *last = MIN(period, period + lead_end) - PHASE_GUARD_US;
    return *first <= *last;
}

void lcd_te_wait(int y, int width, int height)
{
    if (!lcd_te_enabled() || !is_thread_context() || !irq_enabled())
        return;

    int first, last;
    if (!phase_window(y, width, height, &first, &last))
    {
        no_window++;
        return;
    }

    uint32_t start = USEC_TIMER, now = start, until = start + SYNC_WAIT_US;
    uint32_t previous_time = now, next_yield = now + POLL_YIELD_US;
    bool level = te_high();

    /* The tick check also expires the cached edge across a USEC_TIMER wrap. */
    if ((unsigned long)(current_tick - last_rise_tick) > 1 + HZ / 50)
        have_rise = false;

    while (TIME_BEFORE((now = USEC_TIMER), until))
    {
        bool next = te_high();
        if (!level && next && now - previous_time <= POLL_MAX_GAP_US)
        {
            last_rise = now;
            last_rise_tick = current_tick;
            have_rise = true;
        }
        uint32_t age = now - last_rise;
        if (have_rise && age >= (uint32_t)first && age <= (uint32_t)last)
        {
            if (first)
                trail_slots++;
            else
                lead_slots++;
            max_wait = MAX(max_wait, now - start);
            return;
        }
        level = next;
        previous_time = now;
        if (!TIME_BEFORE(now, next_yield))
        {
            yield();
            next_yield = USEC_TIMER + POLL_YIELD_US;
        }
        udelay(POLL_US);
    }
    have_rise = false;
    max_wait = MAX(max_wait, now - start);
    timed_out++;
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

bool lcd_te_run_diagnostic(int (*report)(const char *fmt, ...))
{
    if (!lcd_te_acquire_bus())
    {
        report("LCD unavailable (type, power or DMA busy)");
        return false;
    }

    /* Sampling reports a fresh probe without changing the active sync. */
    struct te_pulse saved_pulse = pulse;
    uint32_t saved_rise = last_rise;
    long saved_rise_tick = last_rise_tick;
    bool saved_have_rise = have_rise;
    bool sync_on = lcd_te_enabled();
    lcd_te_probe();
    struct te_pulse measured = pulse;
    pulse = saved_pulse;
    last_rise = saved_rise;
    last_rise_tick = saved_rise_tick;
    have_rise = saved_have_rise;

    uint32_t elapsed, max_gap = 0;
    unsigned int samples = sample_edges(&elapsed, &max_gap);
    struct dma_stats full = full_dma, band = band_dma, rect = rect_dma;
    unsigned int lead = lead_slots, trail = trail_slots;
    unsigned int misses = timed_out, bypasses = no_window;
    uint32_t worst_wait = max_wait;
    unsigned int timing = LCD_PHTIME;
    unsigned int status = LCD_STATUS;
    lcd_te_release_bus();

    report("TE candidates=%u | sync=%s", measured.valid,
           sync_on ? "on" : "off");
    report("LCD type 1 | panel readback unavailable");
    report("LCD_PHTIME=0x%02x | DMA samples since timing change", timing);
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
    report("TE lead=%u trail=%u timeout=%u no-window=%u",
           lead, trail, misses, bypasses);
    report("TE max wait=%u us", worst_wait);
    report_dma(report, "full", &full);
    report_dma(report, "band (320px wide, 60-239 rows)", &band);
    report_dma(report, "other rect", &rect);
    report("Signals: %u passes in %lu us (max gap %lu us)", samples,
           (unsigned long)elapsed, (unsigned long)max_gap);
    report_edges(report, measured.valid);
    return true;
}
