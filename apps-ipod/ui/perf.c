/* Optional frame-time capture for native motion. GPL version 2+. */
#include "config.h"
#include "perf.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "animation.h"
#include "core_alloc.h"
#include "file.h"
#include "logfile.h"
#include "rbpaths.h"
#include "string-extra.h"
#include "system.h"
#include "thread.h"

#define PERF_MARKER ROCKBOX_DIR "/ui-perf.enable"
#define PERF_LOG ROCKBOX_DIR "/ui-perf.txt"
#define PERF_CAPACITY 512u
#define TIMER_PER_SECOND 1000000u
#define FRAME_BUDGET ((TIMER_PER_SECOND + UI_ANIM_FPS - 1) / UI_ANIM_FPS)
#define FRAME_DECIMALS 10000u
#define FRAME_FORMAT "%u.%04u"
#define FRAME_ARG(n) (n) / FRAME_DECIMALS, (n) % FRAME_DECIMALS

struct frame_sample {
    uint32_t render, submit, interval, skipped;
};

struct frame_capture {
    char route[32];
    unsigned seen, render_misses, work_misses, interval_misses;
    uint32_t worst_render, worst_work, worst_interval, skipped;
    uint32_t render_pending, last_submission, slot_phase;
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

static uint32_t as_frames(uint32_t elapsed)
{
    return ((uint64_t)elapsed * UI_ANIM_FPS * FRAME_DECIMALS + TIMER_PER_SECOND / 2)
           / TIMER_PER_SECOND;
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

void ui_perf_note_submit(uint32_t elapsed_clock)
{
    if (active_handle <= 0 || owner != thread_self())
        return;
    uint32_t clock = USEC_TIMER;
    struct frame_capture *capture = core_get_data_pinned(active_handle);
    struct frame_sample sample = { .render = capture->render_pending,
                                  .submit = elapsed_clock };
    capture->render_pending = 0;
    if (capture->seen)
    {
        sample.interval = clock - capture->last_submission;
        uint64_t position = (uint64_t)sample.interval * UI_ANIM_FPS + capture->slot_phase;
        unsigned slots = position / TIMER_PER_SECOND;
        capture->slot_phase = position % TIMER_PER_SECOND;
        sample.skipped = slots > 1 ? slots - 1 : 0;
        capture->skipped = add_time(capture->skipped, sample.skipped);
        capture->interval_misses += sample.interval > FRAME_BUDGET;
        capture->worst_interval = MAX(sample.interval, capture->worst_interval);
    }
    capture->last_submission = clock;
    uint32_t work = add_time(sample.render, sample.submit);
    capture->worst_render = MAX(sample.render, capture->worst_render);
    capture->render_misses += sample.render > FRAME_BUDGET;
    capture->worst_work = MAX(work, capture->worst_work);
    capture->work_misses += work > FRAME_BUDGET;
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

static uint32_t p99(struct frame_capture *capture, unsigned first, bool interval)
{
    unsigned count = 0;
    for (unsigned i = first; i < capture->seen; ++i)
    {
        const struct frame_sample *sample = &capture->samples[i % PERF_CAPACITY];
        if (!interval || i != 0)
            capture->scratch[count++] = interval ? sample->interval :
                                                add_time(sample->render, sample->submit);
    }
    if (!count)
        return 0;
    qsort(capture->scratch, count, sizeof(*capture->scratch), clock_compare);
    return capture->scratch[(count * 99 + 99) / 100 - 1];
}

static size_t format_rows(const struct frame_capture *capture, unsigned first,
                          char *output, size_t size)
{
    static const char header[] =
        "frame,render_f,submit_f,work_f,interval_f,work_miss,interval_miss,skipped_slots\n";
    size_t used = strlcpy(output, header, size);
    for (unsigned i = first; i < capture->seen; ++i)
    {
        const struct frame_sample *sample = &capture->samples[i % PERF_CAPACITY];
        uint32_t work = add_time(sample->render, sample->submit);
        unsigned work_f = as_frames(work);
        unsigned render_f = as_frames(sample->render);
        unsigned submit_f = as_frames(sample->submit);
        unsigned interval_f = as_frames(sample->interval);
        int n = snprintf(output + used, size - used,
            "%u," FRAME_FORMAT "," FRAME_FORMAT "," FRAME_FORMAT "," FRAME_FORMAT ",%u,%u,%u\n",
            i + 1, FRAME_ARG(render_f), FRAME_ARG(submit_f), FRAME_ARG(work_f),
            FRAME_ARG(interval_f), work > FRAME_BUDGET, sample->interval > FRAME_BUDGET,
            (unsigned)sample->skipped);
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
    unsigned work_p99 = as_frames(p99(capture, first, false));
    unsigned interval_p99 = as_frames(p99(capture, first, true));
    unsigned work_worst = as_frames(capture->worst_work);
    unsigned interval_worst = as_frames(capture->worst_interval);
    size_t rows_size = 128 + (size_t)retained * 88;
    int rows_handle = retained ? core_alloc_free_space(rows_size) : 0;
    char *rows = NULL;
    size_t rows_used = 0;
    if (rows_handle > 0)
    {
        rows = core_get_data_pinned(rows_handle);
        rows_used = format_rows(capture, first, rows, rows_size);
    }
    char summary[384];
    unsigned render_worst = as_frames(capture->worst_render);
    int n = snprintf(summary, sizeof(summary),
        "route=%s seen=%u retained=%u dropped=%u rows=%u "
        "work_p99_f=" FRAME_FORMAT " interval_p99_f=" FRAME_FORMAT " "
        "work_worst_f=" FRAME_FORMAT " interval_worst_f=" FRAME_FORMAT " "
        "work_misses=%u interval_misses=%u skipped_slots=%u "
        "render_worst_f=" FRAME_FORMAT " render_misses=%u\n",
        capture->route, capture->seen, retained, first, rows ? retained : 0,
        FRAME_ARG(work_p99), FRAME_ARG(interval_p99),
        FRAME_ARG(work_worst), FRAME_ARG(interval_worst),
        capture->work_misses, capture->interval_misses, (unsigned)capture->skipped,
        FRAME_ARG(render_worst), capture->render_misses);
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
