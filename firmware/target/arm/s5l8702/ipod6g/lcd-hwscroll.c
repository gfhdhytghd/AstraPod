/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <errno.h>
#include <string.h>
#include "kernel.h"
#include "lcd.h"
#include "system.h"
#include "s5l87xx.h"
#include "lcd-experiment-s5l8702.h"
#include "lcd-hwscroll.h"
#include "lcd-hwscroll-plan.h"
#include "lcd-te-s5l8702.h"

enum { DCS_NORMAL = 0x13, DCS_SCROLL_AREA = 0x33, DCS_SCROLL_START = 0x37 };

#if LCD_WIDTH != HWSCROLL_COLUMNS || LCD_HEIGHT != HWSCROLL_ROWS
#error "lcd-hwscroll-plan.h assumes the 320x240 panel"
#endif

/* Microseconds. The commands go out inside the TE pulse, from OPEN_US
 * after the rise; the IRQ latency eats into the rest of the pulse. */
#define OPEN_US      300
#define GUARD_US     150
#define COMMAND_US    40
#define SETUP_US      50    /* addressing plus the FIFO drain before it */
#define PREPARE_US   100    /* from submission to the earliest start */
#define RETRY_US      50
#define SEND_US      100

enum batch_state { BATCH_IDLE, BATCH_WAITING, BATCH_SENDING };

static struct hwscroll_ring ring = { 0, LCD_HEIGHT, 0 };
static int count_top;
static volatile unsigned int writes;

static struct {
    struct hwscroll_ring ring;        /* once the frame is sent */
    struct hwscroll_segment segment[HWSCROLL_SEGMENTS];
    const uint16_t *stage;
    int count, next, close;
    uint32_t earliest, rise;
    bool redefine, presented;
    volatile enum batch_state state;
} batch;

int lcd_hwscroll_map(int y)
{
    return hwscroll_ring_map(&ring, y);
}

int lcd_hwscroll_run(int y, int rows)
{
    return hwscroll_ring_run(&ring, y, rows);
}

int lcd_hwscroll_offset(void)
{
    return ring.offset;
}

unsigned int lcd_hwscroll_writes(void)
{
    return writes;
}

void lcd_hwscroll_queued(int y, int height)
{
    if (y + height > count_top)
        writes++;
}

bool lcd_hwscroll_pending(void)
{
    return batch.state != BATCH_IDLE;
}

void lcd_hwscroll_cancel(void)
{
    batch.state = BATCH_IDLE;
}

bool lcd_hwscroll_presented(uint32_t *rise)
{
    int flags = disable_irq_save();
    bool presented = batch.presented;
    *rise = batch.rise;
    restore_irq(flags);
    return presented;
}

void lcd_hwscroll_forget(void)
{
    int flags = disable_irq_save();
    ring = (struct hwscroll_ring){ 0, LCD_HEIGHT, 0 };
    writes++;
    restore_irq(flags);
}

bool lcd_hwscroll_reset(void)
{
    if (!lcd_te_supported())
        return true;

    static const uint8_t area[] = {
        0, 0, LCD_HEIGHT >> 8, LCD_HEIGHT & 0xff, 0, 0,
    };
    static const uint8_t start[] = { 0, 0 };
    bool ok = lcd_hwscroll_send(DCS_SCROLL_AREA, area, sizeof(area)) &&
              lcd_hwscroll_send(DCS_SCROLL_START, start, sizeof(start)) &&
              lcd_hwscroll_send(DCS_NORMAL, NULL, 0);
    lcd_hwscroll_forget();
    return ok;
}

static void start_next(void)
{
    if (batch.next < batch.count)
    {
        const struct hwscroll_segment *s = &batch.segment[batch.next];
        if (!lcd_hwscroll_start_dma(batch.stage + s->pixel, s->x, s->panel_y,
                                    s->width, s->rows))
        {
            lcd_te_timer_arm(USEC_TIMER + RETRY_US);
            return;
        }
        batch.next++;
    }
    if (batch.next == batch.count)
        batch.state = BATCH_IDLE;
}

/* The ring moves with 0x37; from then on the frame must go out whatever
 * the timing, as the panel already shows it. */
static void send_frame(void)
{
    uint32_t until = USEC_TIMER + SEND_US;
    const struct hwscroll_ring *next = &batch.ring;
    int start = next->top + next->offset;
    uint8_t area[] = {
        next->top >> 8, next->top & 0xff,
        next->height >> 8, next->height & 0xff, 0, 0,
    };
    uint8_t line[] = { start >> 8, start & 0xff };
    /* Retry the first unfinished command before publishing the row map. */
    if (batch.redefine)
        batch.redefine = !lcd_hwscroll_send_until(DCS_SCROLL_AREA, area,
                                                   sizeof(area), until);
    if (batch.redefine ||
        !lcd_hwscroll_send_until(DCS_SCROLL_START, line, sizeof(line), until))
    {
        lcd_te_timer_arm(USEC_TIMER + RETRY_US);
        return;
    }

    ring = *next;
    batch.state = BATCH_SENDING;
    batch.next = 0;
    start_next();
}

void lcd_hwscroll_timer(uint32_t now)
{
    if (batch.state == BATCH_SENDING)
    {
        /* A start that failed; a running segment chains the next itself. */
        if (!lcd_te_dma_busy())
            start_next();
        return;
    }
    if (batch.state != BATCH_WAITING)
        return;

    uint32_t rise, period, high;
    if (!lcd_te_irq_timing(&rise, &period, &high))
    {
        /* Untracked, send at once: a mistimed frame tears for one
         * refresh, a held one would hold every LCD update. */
        batch.rise = now;
    }
    else
    {
        /* A slot that closed before the IRQ ran moves to the next. */
        uint32_t from = batch.earliest - OPEN_US;
        if (TIME_BEFORE(from, now - batch.close))
            from = now - batch.close;
        batch.rise = hwscroll_plan_rise(rise, period, from);
        if (TIME_BEFORE(now, batch.rise + OPEN_US))
        {
            lcd_te_timer_arm(batch.rise + OPEN_US);
            return;
        }
    }
    if (lcd_te_dma_busy())
        lcd_te_timer_arm(now + RETRY_US);
    else
        send_frame();
}

bool lcd_hwscroll_dma_done(void)
{
    if (batch.state != BATCH_SENDING)
        return false;
    start_next();
    return true;
}

static bool frame_valid(const struct lcd_hwscroll_frame *frame)
{
    int top = frame->top, height = LCD_HEIGHT - top;
    if (top < 0 || top >= LCD_HEIGHT || !frame->count ||
        frame->count > LCD_HWSCROLL_BANDS ||
        frame->shift <= -height || frame->shift >= height)
        return false;

    unsigned int pixels = 0;
    for (int i = 0; i < frame->count; i++)
    {
        const struct lcd_hwscroll_band *b = &frame->band[i];
        if (b->x < 0 || b->width <= 0 || b->width > LCD_WIDTH - b->x ||
            b->y < top || b->height <= 0 || b->height > LCD_HEIGHT - b->y)
            return false;
        pixels += b->width * b->height;
    }
    return pixels <= LCD_WIDTH * LCD_HEIGHT;
}

static void copy_segments(uint16_t *stage)
{
    for (int i = 0; i < batch.count; i++)
    {
        const struct hwscroll_segment *s = &batch.segment[i];
        uint16_t *out = stage + s->pixel;
        for (int y = s->y; y < s->y + s->rows; y++, out += s->width)
            memcpy(out, FBADDR(s->x, y), s->width * sizeof(*out));
    }
}

bool lcd_hwscroll_present(const struct lcd_hwscroll_frame *frame)
{
    uint32_t rise, period, high;
    if (!lcd_te_supported() || !frame_valid(frame) ||
        !lcd_te_irq_timing(&rise, &period, &high))
        return false;

    uint16_t *stage = lcd_hwscroll_stage();
    if (!stage)
        return false;

    /* The previous frame has started; the ring no longer moves. */
    int height = LCD_HEIGHT - frame->top;
    bool redefine = ring.top != frame->top;
    struct hwscroll_ring next = {
        frame->top, height,
        ((ring.offset + frame->shift) % height + height) % height,
    };
    struct hwscroll_timing timing = {
        .open = OPEN_US, .guard = GUARD_US, .command = COMMAND_US,
        .setup = SETUP_US, .full = FULL_DMA_SLOW_US,
    };
    int open, close, count = -1;
    bool ok = (!redefine || !ring.offset) &&
              lcd_te_irq_timing(&rise, &period, &high);
    if (ok)
    {
        timing.period = period;
        timing.high = high;
        count = hwscroll_plan_segments(&next, frame->band, frame->count,
                                       batch.segment);
        ok = count >= 0 && hwscroll_plan_window(&timing, frame->top,
                                                batch.segment, count,
                                                &open, &close);
    }
    if (ok)
    {
        const struct hwscroll_segment *last = &batch.segment[count - 1];
        batch.count = count;
        copy_segments(stage);
        commit_dcache_range(stage, (last->pixel + last->width * last->rows) *
                                   sizeof(*stage));

        int flags = disable_irq_save();
        batch.ring = next;
        batch.stage = stage;
        batch.close = close;
        batch.redefine = redefine;
        batch.earliest = USEC_TIMER + PREPARE_US;
        batch.rise = hwscroll_plan_rise(rise, period,
                                        batch.earliest - OPEN_US);
        batch.presented = true;
        batch.state = BATCH_WAITING;
        count_top = frame->top;
        writes++;
        lcd_te_timer_arm(batch.earliest);
        restore_irq(flags);
    }
    lcd_hwscroll_staged(frame->top, height, ok);
    return ok;
}

bool lcd_hwscroll_valid(const struct lcd_hwscroll_request *request)
{
    if (!request->lines)
        return !request->top && !request->height && !request->start;

    unsigned int end = request->top + request->height;
    return (request->lines == LCD_HEIGHT || request->lines == LCD_WIDTH) &&
           request->height && end <= request->lines &&
           request->start >= request->top && request->start < end;
}

static unsigned int fixed_top, fixed_end;

static void draw_grid(uint16_t *pixels, int height, unsigned int frame)
{
    (void)frame;
    for (int y = 0; y < height; y++)
    {
        bool fixed = (unsigned int)y < fixed_top || (unsigned int)y >= fixed_end;
        for (int x = 0; x < LCD_WIDTH; x++)
        {
            /* The white lines and color steps identify both axes; the
             * rows meant to stay fixed are solid red. */
            *pixels++ = fixed ? LCD_RGBPACK(220, 0, 0) :
                x % 40 < 2 || y % 20 < 2 ? LCD_WHITE :
                LCD_RGBPACK(32 + (x / 40) * 28, 24 + (y / 20) * 19, 72);
        }
    }
}

static bool set_scroll(const struct lcd_hwscroll_request *request)
{
    unsigned int bottom = request->lines - request->top - request->height;
    uint8_t area[] = {
        request->top >> 8, request->top & 0xff,
        request->height >> 8, request->height & 0xff,
        bottom >> 8, bottom & 0xff,
    };
    uint8_t start[] = { request->start >> 8, request->start & 0xff };
    return lcd_hwscroll_send(DCS_SCROLL_AREA, area, sizeof(area)) &&
           lcd_hwscroll_send(DCS_SCROLL_START, start, sizeof(start));
}

int lcd_hwscroll_test(const struct lcd_hwscroll_request *request)
{
    if (!lcd_hwscroll_valid(request))
        return EINVAL;
    if (!lcd_te_supported())
        return ENOTSUP;
    if (!lcd_te_acquire_bus())
        return EBUSY;

    bool ok = lcd_hwscroll_reset();
    if (ok && request->lines)
    {
        fixed_top = request->top;
        fixed_end = request->top + request->height;
        uint32_t dma = lcd_experiment_dma_total();
        lcd_experiment_present(0, LCD_HEIGHT, draw_grid, 0);
        ok = lcd_experiment_dma_total() != dma;
        if (ok)
        {
            sleep(2 * HZ);
            /* Send only commands between the baseline and shifted image. */
            struct lcd_hwscroll_request step = *request;
            ok = set_scroll(&step);
            /* Step the start line every tick for eight seconds. */
            for (int i = 1; ok && i < 8 * HZ; i++)
            {
                sleep(1);
                step.start = request->top +
                    (request->start - request->top + i) % request->height;
                uint8_t start[] = { step.start >> 8, step.start & 0xff };
                ok = lcd_hwscroll_send(DCS_SCROLL_START, start, sizeof(start));
            }
        }
        if (!lcd_hwscroll_reset())
            ok = false;
    }
    lcd_te_release_bus();
    lcd_update();
    lcd_wait_idle();
    return ok ? 0 : EIO;
}
