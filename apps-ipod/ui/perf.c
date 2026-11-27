/* Optional frame-time capture for native motion. GPL version 2+. */
#include "config.h"
#include "perf.h"
#include "retained.h"

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
struct cover_work {
    unsigned target_changes;
    uint32_t cache_wait_us, caption_us;
    uint32_t stage_us[UI_COVER_STAGE_COUNT];
};

struct frame_sample {
    struct cover_work cover;
    uint32_t render, submit, submit_overhead, interval, skipped, phase;
    uint32_t dma_phase, caption_copy_us, caption_overhead_us;
    uint8_t divisor;
    uint8_t parts;
    /* TE rises since the last submission began, and refreshes since the
     * last submission's first refresh; UINT8_MAX when unknown. */
    uint8_t te_slots, present_slots;
    uint16_t art_rows, caption_rows;
    /* The panel's scroll offset after a hardware-scroll frame, -1 for a
     * frame sent whole, and the rows its bands carry. */
    int16_t hw_offset;
    uint16_t band_rows;
};

struct frame_capture {
    char route[32];
    unsigned seen, render_misses, work_misses, interval_misses, half_frames;
    uint32_t worst_render, worst_work, worst_interval, skipped;
    uint32_t render_pending, last_submission, period_q8;
    uint32_t last_start, last_phase, last_present;
    uint32_t ready_wait_us;
    bool first_run, readiness_measured;
    unsigned te_gaps, te_repeats, present_gaps, present_repeats;
    bool presented;
    int32_t slot_phase;
    struct cover_work cover_pending;
    uint8_t parts_pending;
    uint16_t art_pending, caption_pending;
    int16_t hw_offset_pending;
    uint16_t band_rows_pending;
    bool enabled, measured, async;
    struct frame_sample samples[PERF_CAPACITY];
    uint32_t scratch[PERF_CAPACITY];
};

static bool enabled;
static bool active_passive;
static int active_handle;
static unsigned serial, owner;
/* Store the route names, including routes assembled on a caller's stack. */
static char seen_routes[128][sizeof(((struct frame_capture *)0)->route)];
static unsigned route_count;

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

bool ui_perf_enabled(void)
{
    return enabled;
}

void ui_perf_wps_target(char route, const struct ui_rect *rect,
                        const char *reason, const char *bitmap,
                        uint32_t handoff_us, uint32_t playlist_us,
                        uint32_t wps_setup_us)
{
    if (!enabled)
        return;
    int fd = logfile_open(PERF_LOG);
    if (fd >= 0)
    {
        logfile_printf(fd, "cover-wps route=%c source=%s rect=%d,%d,%d,%d "
                       "reason=%s bitmap=%s handoff_us=%lu playlist_us=%lu wps_setup_us=%lu\n", route,
                       rect->width ? "layout" : "fallback", rect->x, rect->y,
                       rect->width, rect->height, reason, bitmap,
                       (unsigned long)handoff_us, (unsigned long)playlist_us,
                       (unsigned long)wps_setup_us);
        logfile_close(fd);
    }
}

void ui_perf_flow_path(const char *enter, const char *enter_why,
                       const char *leave, const char *leave_why)
{
    if (!enabled)
        return;
    int fd = logfile_open(PERF_LOG);
    if (fd >= 0)
    {
        logfile_printf(fd, "cover-flow-path enter=%s enter_why=%s "
                       "leave=%s leave_why=%s\n", enter, enter_why, leave,
                       leave_why);
        logfile_close(fd);
    }
}

void ui_perf_album_open(const char *path, bool remote, bool cached,
                        const char *art, bool scene, const char *result,
                        const char *reason, const char *back, unsigned frames,
                        bool traced)
{
    if (!enabled)
        return;
    int fd = logfile_open(PERF_LOG);
    if (fd >= 0)
    {
        logfile_printf(fd, "cover-album-open path=%s source=%s cache=%s "
                       "art=%s scene=%s result=%s reason=%s back=%s "
                       "frames=%u trace=%s\n",
                       path, remote ? "remote" : "local",
                       cached ? "hit" : "miss", art, scene ? "busy" : "idle",
                       result, reason, back, frames, traced ? "yes" : "no");
        logfile_close(fd);
    }
}

void ui_perf_flow_wps_path(const char *path, const char *centre,
                           const char *result, const char *reason,
                           uint32_t start_us, uint32_t prepare_us)
{
    if (!enabled)
        return;
    int fd = logfile_open(PERF_LOG);
    if (fd >= 0)
    {
        logfile_printf(fd, "cover-flow-wps-path path=%s centre=%s "
                       "result=%s reason=%s start_us=%lu prepare_us=%lu\n",
                       path, centre, result, reason,
                       (unsigned long)start_us, (unsigned long)prepare_us);
        logfile_close(fd);
    }
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
    capture->hw_offset_pending = -1;
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

void ui_perf_ready(unsigned trace, uint32_t requested_at)
{
    if (!trace || trace != serial || active_handle <= 0 || owner != thread_self())
        return;
    uint32_t wait = (uint32_t)USEC_TIMER - requested_at;
    struct frame_capture *capture = core_get_data_pinned(active_handle);
    unsigned i = 0;
    while (i < route_count && strcmp(seen_routes[i], capture->route))
        ++i;
    if (!capture->readiness_measured && i < ARRAYLEN(seen_routes))
    {
        capture->readiness_measured = true;
        capture->ready_wait_us = wait;
        capture->first_run = i == route_count;
        if (capture->first_run)
            strlcpy(seen_routes[route_count++], capture->route, sizeof(seen_routes[0]));
    }
    core_put_data_pinned(capture);
}

void ui_perf_note_cover_work(unsigned changes, uint32_t cache_wait_us,
                             uint32_t caption_us, const uint32_t stages[UI_COVER_STAGE_COUNT])
{
    if (active_handle <= 0 || owner != thread_self())
        return;
    struct frame_capture *capture = core_get_data_pinned(active_handle);
    struct cover_work *work = &capture->cover_pending;
    work->target_changes += changes;
    work->cache_wait_us = add_time(work->cache_wait_us, cache_wait_us);
    work->caption_us = add_time(work->caption_us, caption_us);
    for (unsigned i = 0; i < UI_COVER_STAGE_COUNT; i++)
        work->stage_us[i] = add_time(work->stage_us[i], stages[i]);
    core_put_data_pinned(capture);
}

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

void ui_perf_note_hwscroll(int offset, unsigned rows)
{
    if (active_handle <= 0 || owner != thread_self())
        return;
    struct frame_capture *capture = core_get_data_pinned(active_handle);
    capture->hw_offset_pending = offset;
    capture->band_rows_pending = rows;
    core_put_data_pinned(capture);
}

static void add_transfer(struct ui_perf_transfers *timing, uint32_t start,
                         uint32_t end, unsigned part)
{
    uint32_t elapsed = end - start;
    uint32_t copy = UINT32_MAX;
    timing->dma_phase = UINT32_MAX;
#ifdef HAVE_LCD_VSYNC
    timing->async = lcd_vsync_async_timing(NULL, &timing->dma_phase, NULL, &copy);
#endif
    /* A pending caption can be submitted before this frame's artwork. */
    if (!timing->sampled || (part & UI_PERF_ART))
        timing->primary_start = start;
    timing->sampled = true;
    timing->last = end;
    timing->calls_us = add_time(timing->calls_us, elapsed);
    if (!timing->async || copy > elapsed)
        copy = elapsed;
    timing->copy_us = add_time(timing->copy_us, copy);
    if (part == UI_PERF_CAPTION)
    {
        timing->caption_calls_us = add_time(timing->caption_calls_us, elapsed);
        timing->caption_copy_us = add_time(timing->caption_copy_us, copy);
    }
}

void ui_perf_add_transfer(struct ui_perf_transfers *timing, uint32_t start, unsigned part)
{
    add_transfer(timing, start, USEC_TIMER, part);
}

void ui_perf_note_submit(uint32_t elapsed_clock)
{
    if (active_handle <= 0 || owner != thread_self())
        return;
    struct ui_perf_transfers timing = {0};
    uint32_t end = USEC_TIMER;
    add_transfer(&timing, end - elapsed_clock, end, 0);
    ui_perf_note_transfers(&timing);
}

void ui_perf_note_transfers(const struct ui_perf_transfers *timing)
{
    if (!timing->sampled || active_handle <= 0 || owner != thread_self())
        return;
    uint32_t clock = timing->last;
    struct ui_pace_status pace;
    ui_pace_get_status(&pace);
    uint32_t period_q8 = ui_frame_period_q8();
    uint32_t period = (period_q8 + 128) >> 8;
    struct frame_capture *capture = core_get_data_pinned(active_handle);
    struct frame_sample sample = { .render = capture->render_pending,
                                  .submit = timing->copy_us,
                                  .submit_overhead = timing->calls_us - timing->copy_us,
                                  .phase = ui_clock_phase(timing->primary_start),
                                  .dma_phase = timing->dma_phase,
                                  .divisor = MAX(1, pace.divisor),
                                  .te_slots = UINT8_MAX,
                                  .present_slots = UINT8_MAX,
                                  .parts = capture->parts_pending,
                                  .art_rows = capture->art_pending,
                                  .caption_rows = capture->caption_pending,
                                  .hw_offset = capture->hw_offset_pending,
                                  .band_rows = capture->band_rows_pending };
    if (sample.parts == UI_PERF_CAPTION)
    {
        sample.caption_copy_us = sample.submit;
        sample.caption_overhead_us = sample.submit_overhead;
    }
    else
    {
        sample.caption_copy_us = timing->caption_copy_us;
        sample.caption_overhead_us = timing->caption_calls_us - timing->caption_copy_us;
    }
    if (sample.parts & UI_PERF_ART)
    {
        sample.cover = capture->cover_pending;
        capture->cover_pending = (struct cover_work){0};
    }
    capture->render_pending = 0;
    capture->parts_pending = 0;
    capture->art_pending = capture->caption_pending = 0;
    capture->hw_offset_pending = -1;
    capture->band_rows_pending = 0;
    capture->period_q8 = period_q8;
    capture->enabled = pace.enabled;
    capture->measured = pace.measured;
    capture->async = timing->async;
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
        if (sample.phase != UINT32_MAX && capture->last_phase != UINT32_MAX)
        {
            uint64_t since = capture->last_phase +
                             (uint64_t)(timing->primary_start - capture->last_start);
            sample.te_slots = MIN(UINT8_MAX - 1, (since << 8) / period_q8);
            capture->te_gaps += sample.te_slots > 1;
            capture->te_repeats += sample.te_slots == 0;
        }
    }
    uint32_t present;
    if (ui_present_refresh(USEC_TIMER, &present))
    {
        if (capture->presented)
        {
            int32_t since = present - capture->last_present;
            int64_t slots = since <= 0 ? 0 :
                (((int64_t)since << 8) + period_q8 / 2) / period_q8;
            sample.present_slots = MIN(UINT8_MAX - 1, slots);
            capture->present_gaps += sample.present_slots > 1;
            capture->present_repeats += sample.present_slots == 0;
        }
        capture->last_present = present;
        capture->presented = true;
    }
    capture->last_submission = clock;
    capture->last_start = timing->primary_start;
    capture->last_phase = sample.phase;
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
        "last_dma_start_phase_us,target_changes,cache_wait_us,caption_us,"
        "motion_us,project_us,columns_us,blit_us,clear_us,render_other_us,"
        "art_copy_us,art_submit_overhead_us,caption_copy_us,caption_submit_overhead_us,"
        "te_slots,present_slots,hw_offset,band_rows\n";
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
            ",%u,%u,%u,%u,%d,%s,%u,%u," FRAME_FORMAT ",%d,%u,%u,%u"
            ",%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%d,%d,%d,%u\n",
            i + 1, FRAME_ARG(render_f), FRAME_ARG(submit_f), FRAME_ARG(work_f),
            FRAME_ARG(interval_f), work > period, interval_miss,
            (unsigned)sample->skipped, sample->divisor,
            phase_arg(sample->phase), parts[sample->parts & 3],
            sample->art_rows, sample->caption_rows, FRAME_ARG(overhead_f),
            phase_arg(sample->dma_phase), sample->cover.target_changes,
            (unsigned)sample->cover.cache_wait_us, (unsigned)sample->cover.caption_us,
            (unsigned)sample->cover.stage_us[UI_COVER_MOTION],
            (unsigned)sample->cover.stage_us[UI_COVER_PROJECT],
            (unsigned)sample->cover.stage_us[UI_COVER_COLUMNS],
            (unsigned)sample->cover.stage_us[UI_COVER_BLIT],
            (unsigned)sample->cover.stage_us[UI_COVER_CLEAR],
            (unsigned)sample->cover.stage_us[UI_COVER_OTHER],
            sample->parts & UI_PERF_ART ? (unsigned)(sample->submit - sample->caption_copy_us) : 0,
            sample->parts & UI_PERF_ART ? (unsigned)(sample->submit_overhead - sample->caption_overhead_us) : 0,
            (unsigned)sample->caption_copy_us, (unsigned)sample->caption_overhead_us,
            sample->te_slots == UINT8_MAX ? -1 : (int)sample->te_slots,
            sample->present_slots == UINT8_MAX ? -1 :
                                                 (int)sample->present_slots,
            sample->hw_offset, sample->band_rows);
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
    size_t rows_size = 530 + (size_t)retained * 310;
    int rows_handle = retained ? core_alloc_free_space(rows_size) : 0;
    char *rows = NULL;
    size_t rows_used = 0;
    if (rows_handle > 0)
    {
        rows = core_get_data_pinned(rows_handle);
        rows_used = format_rows(capture, first, rows, rows_size);
    }
    char summary[704], readiness[64] = "";
    if (capture->readiness_measured)
        snprintf(readiness, sizeof(readiness), " ready_wait_us=%lu first_run=%u",
                 (unsigned long)capture->ready_wait_us, capture->first_run ? 1u : 0u);
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
        "phase_p50_us=%d phase_p99_us=%d te_gaps=%u te_repeats=%u "
        "present_gaps=%u present_repeats=%u%s\n",
        capture->route, capture->seen, retained, first, rows ? retained : 0,
        FRAME_ARG(work_p99), FRAME_ARG(interval_p99),
        FRAME_ARG(work_worst), FRAME_ARG(interval_worst),
        capture->work_misses, capture->interval_misses, (unsigned)capture->skipped,
        FRAME_ARG(render_worst), capture->render_misses,
        (unsigned)((period_q8 + 128) >> 8), te,
        capture->async ? "on" : "off", rate, capture->half_frames,
        phase_p50, phase_p99, capture->te_gaps, capture->te_repeats,
        capture->present_gaps, capture->present_repeats, readiness);
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
