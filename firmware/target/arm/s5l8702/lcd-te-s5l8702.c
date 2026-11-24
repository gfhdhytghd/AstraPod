/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "config.h"
#include "kernel.h"
#include "lcd.h"
#include "lcd-te-s5l8702.h"
#include "system.h"
#include "s5l87xx.h"
#include <stdio.h>
#include <string.h>

#define TE_MIN_US       14000u
#define TE_MAX_US       17000u
#define SYNC_WAIT_US    20000u
#define READ_WAIT_US      250u
#define SCAN_SAMPLES      768
#define ID_SAMPLES        256
#define MAX_SCANLINE     1023u
#define LCD_READ_READY    0x01u
#define LCD_BUS_IDLE      0x02u
#define LCD_WRITE_FULL    0x10u

enum { GPIO_PORTS = 16, STATUS = GPIO_PORTS, INTCON, IRQ_RAW, SOURCE_COUNT };
enum sync_method { SYNC_NONE, SYNC_EDGE, SYNC_SCANLINE };

struct edge_stats {
    uint32_t last_rise;
    uint32_t rises, falls, periods;
    uint32_t min_period, max_period, period_sum;
    uint32_t te_min, te_max, te_periods;
};

struct scan_stats {
    unsigned int reads, distinct, advances, retreats, frame_wraps;
    unsigned int min, max, invalid;
    uint32_t elapsed, wrap_min, wrap_max;
    unsigned char first[3];
};

struct id_stats {
    unsigned int reads, changes;
    unsigned char first[4];
};

static struct edge_stats gpio_edges[GPIO_PORTS][8];
static struct edge_stats lcd_edges[SOURCE_COUNT - GPIO_PORTS][32];
static uint32_t pinmux[GPIO_PORTS];
static enum sync_method method;
static unsigned int edge_source;
static uint32_t edge_mask;
static bool enabled;
extern int lcd_type;

bool lcd_te_supported(void)
{
    return lcd_type == 1;
}

bool lcd_te_enabled(void)
{
    return lcd_te_supported() && enabled;
}

void lcd_te_set_enabled(bool enable)
{
    enabled = enable && lcd_te_supported() && method != SYNC_NONE;
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

static bool wait_status(uint32_t mask, uint32_t expected, uint32_t until)
{
    while (TIME_BEFORE(USEC_TIMER, until))
    {
        if ((LCD_STATUS & mask) == expected)
            return true;
    }
    return false;
}

/* Parallel DCS read: a dummy cycle precedes the response bytes. */
static bool read_dcs(unsigned int cmd, unsigned char *data, unsigned int count,
                     uint32_t until)
{
    if (!wait_status(LCD_WRITE_FULL, 0, until))
        return false;
    LCD_WCMD = cmd;
    udelay(4);
    while (count--)
    {
        if (!wait_status(LCD_BUS_IDLE, LCD_BUS_IDLE, until))
            return false;
        LCD_RDATA = 0;
        if (!wait_status(LCD_READ_READY, LCD_READ_READY, until))
            return false;
        *data++ = (LCD_DBUFF >> 1) & 0xff;
    }
    return true;
}

static unsigned int scanline(const unsigned char *data)
{
    return (data[1] << 8) | data[2];
}

void lcd_te_wait(int y)
{
    if (!lcd_te_enabled() || !is_thread_context() || !irq_enabled())
        return;

    uint32_t until = USEC_TIMER + SYNC_WAIT_US;
    if (method == SYNC_EDGE)
    {
        /* An already asserted pulse belongs to the current scan. */
        bool seen_low = false;
        while (TIME_BEFORE(USEC_TIMER, until))
        {
            if (!(read_source(edge_source) & edge_mask))
                seen_low = true;
            else if (seen_low)
                return;
        }
        return;
    }

    unsigned int previous = MAX_SCANLINE + 1;
    unsigned int top = y;
    while (TIME_BEFORE(USEC_TIMER, until))
    {
        unsigned char data[3];
        uint32_t read_until = USEC_TIMER + READ_WAIT_US;
        if (TIME_BEFORE(until, read_until))
            read_until = until;
        if (!read_dcs(0x45, data, sizeof(data), read_until))
            return;
        unsigned int line = scanline(data);
        if (line > MAX_SCANLINE)
            return;
        if (line > top && line <= top + 16 &&
            (previous <= top || (line < previous && previous <= MAX_SCANLINE &&
                                 previous >= LCD_HEIGHT - 16 && line <= 16)))
            return;
        previous = line;
        udelay(25);
    }
}

static void sample_scan(struct scan_stats *result)
{
    uint32_t seen[(MAX_SCANLINE + 1) / 32] = { 0 };
    uint32_t start = USEC_TIMER;
    uint32_t last_wrap = start;
    unsigned int previous = MAX_SCANLINE + 1;
    result->min = MAX_SCANLINE;
    result->wrap_min = UINT32_MAX;

    for (unsigned int i = 0; i < SCAN_SAMPLES; i++)
    {
        unsigned char data[3];
        if (!read_dcs(0x45, data, sizeof(data), USEC_TIMER + READ_WAIT_US))
            break;
        if (!result->reads++)
            memcpy(result->first, data, sizeof(data));

        unsigned int line = scanline(data);
        if (line <= MAX_SCANLINE)
        {
            uint32_t bit = 1u << (line % 32);
            if (!(seen[line / 32] & bit))
            {
                seen[line / 32] |= bit;
                result->distinct++;
            }
            result->min = MIN(result->min, line);
            result->max = MAX(result->max, line);
            if (previous <= MAX_SCANLINE && line != previous)
            {
                if (line > previous && line - previous <= 32)
                    result->advances++;
                else if (line <= 16 && previous >= LCD_HEIGHT - 16)
                {
                    uint32_t now = USEC_TIMER;
                    uint32_t period = now - last_wrap;
                    if (result->retreats && frame_period(period))
                    {
                        result->frame_wraps++;
                        result->wrap_min = MIN(result->wrap_min, period);
                        result->wrap_max = MAX(result->wrap_max, period);
                    }
                    last_wrap = now;
                    result->retreats++;
                }
                else
                    result->invalid++;
            }
        }
        else
            result->invalid++;
        previous = line;
        udelay(80);
        if (i % 32 == 31)
            yield();
    }
    result->elapsed = USEC_TIMER - start;
}

static void sample_ids(struct id_stats *ids)
{
    static const unsigned char commands[] = { 0x04, 0xda, 0xdb, 0xdc };
    bool timed_out[ARRAYLEN(commands)] = { false };
    for (unsigned int i = 0; i < ID_SAMPLES; i++)
    {
        for (unsigned int j = 0; j < ARRAYLEN(commands); j++)
        {
            if (timed_out[j])
                continue;
            unsigned char data[4] = { 0 };
            if (!read_dcs(commands[j], data, j == 0 ? 4 : 2,
                          USEC_TIMER + READ_WAIT_US))
            {
                timed_out[j] = true;
                continue;
            }
            if (!ids[j].reads++)
                memcpy(ids[j].first, data, sizeof(data));
            else if (memcmp(ids[j].first + 1, data + 1, sizeof(data) - 1))
                ids[j].changes++;
        }
        if (i % 32 == 31)
            yield();
    }
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

static bool is_candidate(const struct edge_stats *stats, int mux)
{
    return mux <= 0 && stats->te_periods >= 12 &&
        stats->te_periods * 2 >= stats->periods &&
        stats->min_period >= TE_MIN_US && stats->te_max - stats->te_min <= 700;
}

static unsigned int scan_edges(int (*report)(const char *fmt, ...))
{
    unsigned int candidates = 0;
    for (unsigned int source = 0; source < SOURCE_COUNT; source++)
    {
        for (unsigned int bit = 0; bit < source_bits(source); bit++)
        {
            const struct edge_stats *stats = &source_edges(source)[bit];
            if (!stats->rises && !stats->falls)
                continue;
            int mux = source < GPIO_PORTS ?
                (int)((pinmux[source] >> (bit * 4)) & 0xf) : -1;
            bool candidate = is_candidate(stats, mux);
            if (!report)
            {
                if (candidate)
                {
                    candidates++;
                    edge_source = source;
                    edge_mask = 1u << bit;
                }
                continue;
            }
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
    return candidates;
}

bool lcd_te_run_diagnostic(int (*report)(const char *fmt, ...))
{
    uint32_t command_mode;
    if (!lcd_te_acquire_bus(&command_mode))
    {
        report("LCD unavailable (type, power or DMA busy)");
        return false;
    }

    bool was_enabled = enabled;
    enabled = false;
    unsigned int raw_start = read_source(IRQ_RAW) != 0;
    uint32_t elapsed, max_gap = 0;
    unsigned int samples = sample_edges(&elapsed, &max_gap);
    unsigned int raw_end = read_source(IRQ_RAW) != 0;

    struct scan_stats scan = { 0 };
    struct id_stats ids[4] = { { 0 } };
    uint32_t saved_mode = LCD_CON;
    bool command_ready = wait_status(LCD_BUS_IDLE, LCD_BUS_IDLE,
                                     USEC_TIMER + READ_WAIT_US);
    bool restored_idle = command_ready;
    if (command_ready)
    {
        udelay(1);
        LCD_CON = command_mode;
        sample_scan(&scan);
        sample_ids(ids);
        restored_idle = wait_status(LCD_BUS_IDLE, LCD_BUS_IDLE,
                                    USEC_TIMER + READ_WAIT_US);
        if (LCD_STATUS & LCD_READ_READY)
            (void)LCD_DBUFF;
        udelay(1);
        LCD_CON = saved_mode;
    }
    lcd_te_release_bus();

    bool scan_ok = restored_idle && scan.reads == SCAN_SAMPLES &&
        scan.distinct >= 64 && scan.min <= 16 && scan.max >= LCD_HEIGHT - 1 &&
        scan.frame_wraps >= 2 && scan.invalid <= scan.advances / 10;

    unsigned int candidates = scan_edges(NULL);
    method = restored_idle && candidates == 1 ? SYNC_EDGE :
        scan_ok ? SYNC_SCANLINE : SYNC_NONE;
    const char *prototype = method == SYNC_EDGE ? "edge" :
        method == SYNC_SCANLINE ? "scanline" : "unavailable";
    report("TE candidates=%u | prototype=%s", candidates,
           prototype);
    report("LCD type 1 | scanline %s", scan_ok ? "usable" : "unqualified");
    report("0x45 reads=%u/%u distinct=%u",
           scan.reads, SCAN_SAMPLES, scan.distinct);
    if (scan.distinct)
        report("0x45 min=%u max=%u", scan.min, scan.max);
    if (scan.reads)
        report("0x45 first (dummy/MSB/LSB)=%02x %02x %02x",
               scan.first[0], scan.first[1], scan.first[2]);
    report("0x45 advance=%u wrap=%u other=%u", scan.advances, scan.retreats,
           scan.invalid);
    if (scan.frame_wraps)
        report("0x45 frame periods=%u min=%lu max=%lu us", scan.frame_wraps,
               (unsigned long)scan.wrap_min, (unsigned long)scan.wrap_max);
    report("0x45 elapsed=%lu us", (unsigned long)scan.elapsed);
    report("Command ready=%u idle before restore=%u",
           command_ready, restored_idle);
    report("0x04 reads=%u/%u changed=%u",
           ids[0].reads, ID_SAMPLES, ids[0].changes);
    if (ids[0].reads)
        report("0x04 dummy/ID=%02x %02x %02x %02x", ids[0].first[0],
               ids[0].first[1], ids[0].first[2], ids[0].first[3]);
    for (unsigned int i = 1; i < ARRAYLEN(ids); i++)
    {
        report("0x%02x reads=%u/%u changed=%u", 0xd9 + i,
               ids[i].reads, ID_SAMPLES, ids[i].changes);
        if (ids[i].reads)
            report("  dummy/ID=%02x %02x", ids[i].first[0], ids[i].first[1]);
    }
    report("Signals: %u passes in %lu us (max gap %lu us)", samples,
           (unsigned long)elapsed, (unsigned long)max_gap);
    report("IRQ_LCD raw start=%u end=%u; IRQ mask untouched",
           raw_start, raw_end);
    scan_edges(report);
    enabled = was_enabled && method != SYNC_NONE;
    return true;
}
