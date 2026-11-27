/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "panel_perf.h"
#include "file.h"
#include "dir.h"
#include "font_pbf.h"
#include "kernel.h"
#include "logfile.h"
#include "rbpaths.h"
#include "system.h"
#include "system/bg_task.h"
#include "usb.h"
#include <string.h>

#define PERF_DIR ROCKBOX_DIR "/podbox"
#define PERF_EVENT 0x1184
#define PERF_SAMPLES 128

static struct panel_sample {
    uint32_t stamp, elapsed, value, detail;
    enum panel_perf_event event;
} samples[PERF_SAMPLES];

static struct mutex lock;
static unsigned tail, head;
static bool enabled, posted, art_active;
static unsigned frames, fallback_frames, black_frames;
static uint32_t drawn[128], last_frames;
static struct {uint32_t key, stamp;} issued[128];
static struct bg_task task;

static void flush_samples(const struct queue_event *event)
{
    (void)event;
    if (usb_host_wants_storage())
        return;
    mkdir(PERF_DIR);
    int fd = logfile_open(PERF_DIR "/perf.log");
    for (unsigned i = 0; i < PERF_SAMPLES; i++)
    {
        mutex_lock(&lock);
        if (tail == head)
        {
            posted = false;
            mutex_unlock(&lock);
            break;
        }
        struct panel_sample s = samples[tail++ % PERF_SAMPLES];
        mutex_unlock(&lock);
        if (fd >= 0)
        {
            static const char *const names[] = {
                "select", "settle", "ready", "open", "tagcache", "remote",
                "blur", "glyph", "draw", "disk", "ram", "cancel",
                "cover-begin", "cover-issue", "cover-done", "cover-cancel",
                "cover-hit", "cover-ram", "cover-draw", "cover-frames", "cover-fallback", "cover-end",
            };
            logfile_printf(fd, "panel %s %lu %lu %u %u\n", names[s.event],
                           (unsigned long)s.stamp, (unsigned long)s.elapsed,
                           s.value, s.detail);
        }
    }
    if (fd >= 0)
        logfile_close(fd);
    mutex_lock(&lock);
    posted = false;
    mutex_unlock(&lock);
}

static struct bg_task task = { .name = "panel perf", .handle_event = flush_samples };

void panel_perf_record(enum panel_perf_event event, uint32_t elapsed,
                       unsigned value, unsigned detail)
{
    if (!(event >= PP_ART_BEGIN ? art_active : enabled) || usb_host_wants_storage())
        return;
    struct panel_sample s = { USEC_TIMER, elapsed, value, detail, event };
    mutex_lock(&lock);
    unsigned slot = value % (sizeof(issued) / sizeof(issued[0]));
    if (event == PP_ART_ISSUE)
    {
        issued[slot].key = value;
        issued[slot].stamp = s.stamp;
    }
    if (event == PP_ART_DRAW && issued[slot].key == value && issued[slot].stamp)
        s.elapsed = s.stamp - issued[slot].stamp;
    if (head - tail == PERF_SAMPLES)
        ++tail;
    samples[head++ % PERF_SAMPLES] = s;
    bool post = !posted || !bg_task_queued(&task, PERF_EVENT);
    posted = true;
    mutex_unlock(&lock);
    if (post)
        bg_task_post(&task, PERF_EVENT);
}

static void art_flush_frames(void)
{
    if (!frames)
        return;
    panel_perf_record(PP_ART_FRAMES, USEC_TIMER - last_frames, frames, black_frames);
    panel_perf_record(PP_ART_FALLBACK, 0, fallback_frames, 0);
    frames = fallback_frames = black_frames = 0;
    last_frames = USEC_TIMER;
}

void panel_perf_art_session(bool active)
{
    if (active)
    {
        mutex_lock(&lock);
        memset(issued, 0, sizeof(issued));
        art_active = true;
        mutex_unlock(&lock);
        frames = fallback_frames = black_frames = 0;
        last_frames = USEC_TIMER;
        memset(drawn, 0, sizeof(drawn));
        panel_perf_record(PP_ART_BEGIN, 0, 0, 0);
    }
    else
    {
        art_flush_frames();
        panel_perf_record(PP_ART_END, 0, 0, 0);
        art_active = false;
    }
}

void panel_perf_art_drawn(unsigned version, unsigned tier)
{
    if (!art_active || !version || !tier)
        return;
    unsigned slot = (version >> 2) % (sizeof(drawn) / sizeof(drawn[0]));
    if (drawn[slot] == version)
        return;
    drawn[slot] = version;
    panel_perf_record(PP_ART_DRAW, 0, version >> 2, tier);
}

void panel_perf_art_frame(bool fallback, bool black)
{
    if (!art_active)
        return;
    frames++;
    fallback_frames += fallback;
    black_frames += black;
    if ((uint32_t)USEC_TIMER - last_frames >= 1000000u)
        art_flush_frames();
}

static void font_batch(unsigned height, uint32_t elapsed, unsigned count)
{
    panel_perf_record(PP_FONT, elapsed, height, count);
}

void panel_perf_init(void)
{
    mutex_init(&lock);
    enabled = file_exists(PERF_DIR "/perf.enable");
    if (enabled)
        font_pbf_set_batch_trace(font_batch);
}
