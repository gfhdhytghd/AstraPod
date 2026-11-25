/* Optional frame-time capture for native motion. GPL version 2+. */
#include "config.h"
#include "perf.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "animation.h"
#include "pace.h"
#include "core_alloc.h"
#include "file.h"
#include "lcd-vsync.h"
#include "logfile.h"
#include "rbpaths.h"
#include "string-extra.h"
#include "system.h"
#include "thread.h"

#define PERF_MARKER ROCKBOX_DIR "/ui-perf.enable"
#define PERF_LOG ROCKBOX_DIR "/ui-perf.txt"
#define PERF_CAPACITY 512u
#define FRAME_DECIMALS 10000u
/* An interval up to this fraction of a period past its slots is wake
 * jitter, not a miss. */
#define JITTER_DIVISOR 8u
#define FRAME_FORMAT "%u.%04u"
#define FRAME_ARG(n) (n) / FRAME_DECIMALS, (n) % FRAME_DECIMALS

/* Times are in USEC_TIMER units and frames are panel refreshes. A phase is
 * microseconds after the predicted TE rise, UINT32_MAX when unknown. */
struct frame_sample {
    uint32_t render, submit, submit_overhead, interval, skipped, phase;
    uint32_t dma_phase;
    uint8_t divisor;
    uint8_t parts;
    uint16_t art_rows, caption_rows;
};

struct frame_capture {
    char route[32];
    unsigned seen, render_misses, work_misses, interval_misses, half_frames;
    uint32_t worst_render, worst_work, worst_interval, skipped;
    uint32_t render_pending, last_submission, period_q8;
    int32_t slot_phase;
    uint8_t parts_pending;
    uint16_t art_pending, caption_pending;
    bool enabled, measured, async;
    struct frame_sample samples[PERF_CAPACITY];
    uint32_t scratch[PERF_CAPACITY];
};

static bool enabled;
static bool active_passive;
static int active_handle;
static unsigned serial, owner;

static uint32_t add_time(uint32_t a, uint32_t b)
{
    return a > UINT32_MAX - b ? UINT32_MAX : a + b;
}

static uint32_t frame_work(const struct frame_sample *sample)
{
    return add_time(sample->render,
                    add_time(sample->submit, sample->submit_overhead));
}

static uint32_t as_frames(uint32_t elapsed, uint32_t period_q8)
{
    return (((uint64_t)elapsed * FRAME_DECIMALS << 8) + period_q8 / 2) / period_q8;
}

/* The longest interval that still meets a slot at this divisor. */
static uint32_t interval_budget(uint32_t period, unsigned divisor)
{
    return divisor * period + period / JITTER_DIVISOR;
}

void ui_perf_init(void)
{
    enabled = file_exists(PERF_MARKER);
}

static unsigned begin_capture(const char *route, bool passive)
{
    if (!enabled)
        return 0;
    if (active_handle > 0)
    {
        if (!active_passive || passive || owner != thread_self())
            return 0;
        active_handle = core_free(active_handle);
    }
    int handle = core_alloc_free_space(sizeof(struct frame_capture));
    if (handle <= 0)
        return 0;

    struct frame_capture *capture = core_get_data_pinned(handle);
    memset(capture, 0, sizeof(*capture));
    strlcpy(capture->route, route, sizeof(capture->route));
    core_put_data_pinned(capture);
    owner = thread_self();
    if (++serial == 0)
        ++serial;
    active_passive = passive;
    active_handle = handle;
    return serial;
}

unsigned ui_perf_begin(const char *route) { return begin_capture(route, false); }
unsigned ui_perf_begin_passive(const char *route) { return begin_capture(route, true); }

void ui_perf_note_render(uint32_t elapsed_clock)
{
    if (active_handle <= 0 || owner != thread_self())
        return;
    struct frame_capture *capture = core_get_data_pinned(active_handle);
    capture->render_pending = add_time(capture->render_pending, elapsed_clock);
    core_put_data_pinned(capture);
}

void ui_perf_note_rects(unsigned parts, unsigned art_rows,
                        unsigned caption_rows)
{
    if (active_handle <= 0 || owner != thread_self())
        return;
    struct frame_capture *capture = core_get_data_pinned(active_handle);
    capture->parts_pending = parts;
    capture->art_pending = art_rows;
    capture->caption_pending = caption_rows;
    core_put_data_pinned(capture);
}

void ui_perf_note_submit(uint32_t elapsed_clock)
{
    if (active_handle <= 0 || owner != thread_self())
        return;
    uint32_t clock = USEC_TIMER;
    struct ui_pace_status pace;
    ui_pace_get_status(&pace);
    uint32_t period_q8 = ui_frame_period_q8();
    uint32_t period = (period_q8 + 128) >> 8;
    uint32_t dma_phase = UINT32_MAX, copy = UINT32_MAX;
    bool async = false;
#ifdef HAVE_LCD_VSYNC
    async = lcd_vsync_async_timing(NULL, &dma_phase, NULL, &copy);
#endif
    uint32_t submit = async && copy <= elapsed_clock ? copy : elapsed_clock;
    struct frame_capture *capture = core_get_data_pinned(active_handle);
    struct frame_sample sample = { .render = capture->render_pending,
                                  .submit = submit,
                                  .submit_overhead = elapsed_clock - submit,
                                  .phase = ui_clock_phase(clock - elapsed_clock),
                                  .dma_phase = dma_phase,
                                  .divisor = MAX(1, pace.divisor),
                                  .parts = capture->parts_pending,
                                  .art_rows = capture->art_pending,
                                  .caption_rows = capture->caption_pending };
    capture->render_pending = 0;
    capture->parts_pending = 0;
    capture->art_pending = capture->caption_pending = 0;
    capture->period_q8 = period_q8;
    capture->enabled = pace.enabled;
    capture->measured = pace.measured;
    capture->async = async;
    capture->half_frames += sample.divisor > 1;
    if (capture->seen)
    {
        sample.interval = clock - capture->last_submission;
        /* Slots are the divisor's presentation slots, in 1/256 microseconds.
         * An interval counts the nearest whole slots, so jitter either side
         * of a slot boundary is not a skip. */
        int64_t slot = (int64_t)sample.divisor * period_q8;
        int64_t position = ((int64_t)sample.interval << 8) + capture->slot_phase;
        unsigned slots = position < 0 ? 0 : (position + slot / 2) / slot;
        capture->slot_phase = position - slots * slot;
        sample.skipped = slots > 1 ? slots - 1 : 0;
        capture->skipped = add_time(capture->skipped, sample.skipped);
        capture->interval_misses +=
            sample.interval > interval_budget(period, sample.divisor);
        capture->worst_interval = MAX(sample.interval, capture->worst_interval);
    }
    capture->last_submission = clock;
    uint32_t work = frame_work(&sample);
    capture->worst_render = MAX(sample.render, capture->worst_render);
    capture->render_misses += sample.render > period;
    capture->worst_work = MAX(work, capture->worst_work);
    capture->work_misses += work > period;
    capture->samples[capture->seen % PERF_CAPACITY] = sample;
    ++capture->seen;
    core_put_data_pinned(capture);
}

static int clock_compare(const void *a, const void *b)
{
    uint32_t first = *(const uint32_t *)a;
    uint32_t second = *(const uint32_t *)b;
    return (first > second) - (first < second);
}

enum sample_value { WORK, INTERVAL, PHASE };

/* The percentile of a value over the retained samples, or UINT32_MAX when
 * none has it. */
static uint32_t percentile(struct frame_capture *capture, unsigned first,
                           enum sample_value value, unsigned percent)
{
    unsigned count = 0;
    for (unsigned i = first; i < capture->seen; ++i)
    {
        const struct frame_sample *sample = &capture->samples[i % PERF_CAPACITY];
        if (value == WORK)
            capture->scratch[count++] = frame_work(sample);
        else if (value == INTERVAL && i != 0)
            capture->scratch[count++] = sample->interval;
        else if (value == PHASE && sample->phase != UINT32_MAX)
            capture->scratch[count++] = sample->phase;
    }
    if (!count)
        return UINT32_MAX;
    qsort(capture->scratch, count, sizeof(*capture->scratch), clock_compare);
    return capture->scratch[(count * percent + 99) / 100 - 1];
}

static uint32_t p99(struct frame_capture *capture, unsigned first, bool interval)
{
    uint32_t value = percentile(capture, first, interval ? INTERVAL : WORK, 99);
    return value == UINT32_MAX ? 0 : value;
}

static int phase_arg(uint32_t phase)
{
    return phase == UINT32_MAX ? -1 : (int)phase;
}

static size_t format_rows(const struct frame_capture *capture, unsigned first,
                          char *output, size_t size)
{
    static const char header[] =
        "frame,render_f,submit_f,work_f,interval_f,work_miss,interval_miss,skipped_slots,"
        "divisor,phase_us,parts,art_rows,caption_rows,submit_overhead_f,"
        "last_dma_start_phase_us\n";
    static const char *const parts[] = { "-", "a", "c", "ac" };
    uint32_t period_q8 = capture->period_q8;
    uint32_t period = (period_q8 + 128) >> 8;
    size_t used = strlcpy(output, header, size);
    for (unsigned i = first; i < capture->seen; ++i)
    {
        const struct frame_sample *sample = &capture->samples[i % PERF_CAPACITY];
        uint32_t work = frame_work(sample);
        unsigned work_f = as_frames(work, period_q8);
        unsigned render_f = as_frames(sample->render, period_q8);
        unsigned submit_f = as_frames(sample->submit, period_q8);
        unsigned overhead_f = as_frames(sample->submit_overhead, period_q8);
        unsigned interval_f = as_frames(sample->interval, period_q8);
        bool interval_miss =
            sample->interval > interval_budget(period, sample->divisor);
        int n = snprintf(output + used, size - used,
            "%u," FRAME_FORMAT "," FRAME_FORMAT "," FRAME_FORMAT "," FRAME_FORMAT
            ",%u,%u,%u,%u,%d,%s,%u,%u," FRAME_FORMAT ",%d\n",
            i + 1, FRAME_ARG(render_f), FRAME_ARG(submit_f), FRAME_ARG(work_f),
            FRAME_ARG(interval_f), work > period, interval_miss,
            (unsigned)sample->skipped, sample->divisor,
            phase_arg(sample->phase), parts[sample->parts & 3],
            sample->art_rows, sample->caption_rows, FRAME_ARG(overhead_f),
            phase_arg(sample->dma_phase));
        if (n < 0 || (size_t)n >= size - used)
            break;
        used += n;
    }
    return used;
}

void ui_perf_end(unsigned token)
{
    if (!token || active_handle <= 0 || token != serial || owner != thread_self())
        return;
    int handle = active_handle;
    active_handle = 0;
    struct frame_capture *capture = core_get_data_pinned(handle);
    unsigned retained = MIN(capture->seen, PERF_CAPACITY);
    unsigned first = capture->seen - retained;
    uint32_t period_q8 = capture->period_q8 ? capture->period_q8 :
                                              ui_frame_period_q8();
    unsigned work_p99 = as_frames(p99(capture, first, false), period_q8);
    unsigned interval_p99 = as_frames(p99(capture, first, true), period_q8);
    unsigned work_worst = as_frames(capture->worst_work, period_q8);
    unsigned interval_worst = as_frames(capture->worst_interval, period_q8);
    int phase_p50 = phase_arg(percentile(capture, first, PHASE, 50));
    int phase_p99 = phase_arg(percentile(capture, first, PHASE, 99));
    size_t rows_size = 200 + (size_t)retained * 144;
    int rows_handle = retained ? core_alloc_free_space(rows_size) : 0;
    char *rows = NULL;
    size_t rows_used = 0;
    if (rows_handle > 0)
    {
        rows = core_get_data_pinned(rows_handle);
        rows_used = format_rows(capture, first, rows, rows_size);
    }
    char summary[512];
    unsigned render_worst = as_frames(capture->worst_render, period_q8);
    const char *te = !capture->enabled ? "off" :
                     capture->measured ? "measured" : "nominal";
    const char *rate = !capture->half_frames ? "full" :
                       capture->half_frames == capture->seen ? "half" : "mixed";
    int n = snprintf(summary, sizeof(summary),
        "route=%s seen=%u retained=%u dropped=%u rows=%u "
        "work_p99_f=" FRAME_FORMAT " interval_p99_f=" FRAME_FORMAT " "
        "work_worst_f=" FRAME_FORMAT " interval_worst_f=" FRAME_FORMAT " "
        "work_misses=%u interval_misses=%u skipped_slots=%u "
        "render_worst_f=" FRAME_FORMAT " render_misses=%u "
        "period_us=%u te=%s async=%s rate=%s half_frames=%u "
        "phase_p50_us=%d phase_p99_us=%d\n",
        capture->route, capture->seen, retained, first, rows ? retained : 0,
        FRAME_ARG(work_p99), FRAME_ARG(interval_p99),
        FRAME_ARG(work_worst), FRAME_ARG(interval_worst),
        capture->work_misses, capture->interval_misses, (unsigned)capture->skipped,
        FRAME_ARG(render_worst), capture->render_misses,
        (unsigned)((period_q8 + 128) >> 8), te,
        capture->async ? "on" : "off", rate, capture->half_frames,
        phase_p50, phase_p99);
    core_put_data_pinned(capture);
    core_free(handle);
    if (n > 0 && (size_t)n < sizeof(summary))
    {
        int fd = logfile_open(PERF_LOG);
        if (fd >= 0)
        {
            logfile_write(fd, summary, n);
            if (rows)
                logfile_write(fd, rows, rows_used);
            logfile_close(fd);
        }
    }
    if (rows)
    {
        core_put_data_pinned(rows);
        core_free(rows_handle);
    }
}
