/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2025-2026 David Cormier
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

/*
 * S5L8702 hardware H.264 player.
 *
 * This is deliberately a narrow production contract: non-fragmented MP4/M4V,
 * the measured iTunes 9.2.1 Constrained Baseline syntax (Level 1.3 or 3.0,
 * up to four advertised reference pictures and ordered slices),
 * <= 640x480, <= 30 fps, with optional AAC-LC audio.
 *
 * Copyright (C) 2025-2026 David Cormier
 */

#include "plugin.h"
#include "backlight.h"
#include "button.h"
#include "crc32.h"
#include "dir.h"
#include "file.h"
#include "font.h"
#include "kernel.h"
#include "lang.h"
#include "lcd.h"
#include "menu.h"
#include "misc.h"
#include "mp4_demux.h"
#include "pcmbuf.h"
#include "rbpaths.h"
#include "settings.h"
#include "sound.h"
#include "splash.h"
#include "system.h"
#include "video_audio.h"
#include "video_pcm.h"
#include "video_playback.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define VIDEO_MAX_WIDTH          640
#define VIDEO_MAX_HEIGHT         480
#define VIDEO_MAX_LEVEL          30
#define VIDEO_MAX_FPS            30
#define VIDEO_MAX_SAMPLES        400000u
#define VIDEO_MAX_AUDIO_SAMPLES  600000u
#define VIDEO_READ_BUFFER        (512u * 1024u)
#define VIDEO_OUTPUT_BUFFER      (LCD_WIDTH * LCD_HEIGHT * 3u / 2u)
#define VIDEO_PCM_BUFFER         (256u * 1024u)
#define VIDEO_QUEUE_DEPTH        3
#define VIDEO_PRESENT_LATE_MS   33
#define VIDEO_CATCH_UP_MS        160
#define VIDEO_CATCH_UP_LEAD_MS   240
#define VIDEO_CATCH_UP_RETRY_MS  50
#ifdef VIDEO_ROCK_WORKER
#ifdef DEFAULT_STACK_SIZE
#define VIDEO_WORKER_STACK      MAX(12u * 1024u, DEFAULT_STACK_SIZE)
#else
#define VIDEO_WORKER_STACK      (12u * 1024u)
#endif
#else
#define VIDEO_WORKER_STACK      0
#endif
#define VIDEO_OSD_SHOW_TICKS     (HZ * 4)
#define VIDEO_RESUME_MAGIC       0x48323634u
#define VIDEO_RESUME_FILE        PLUGIN_APPS_DATA_DIR \
                                 "/h264-resume-%08lx.dat"

enum video_input_action
{
    VIDEO_INPUT_NONE = 0,
    VIDEO_INPUT_EXIT,
    VIDEO_INPUT_USB,
    VIDEO_INPUT_MENU,
    VIDEO_INPUT_PREVIOUS,
    VIDEO_INPUT_NEXT,
    VIDEO_INPUT_SEEK,
    VIDEO_INPUT_SEEK_BACK,
    VIDEO_INPUT_SEEK_FORWARD,
};

enum video_overlay
{
    VIDEO_OVERLAY_HIDDEN,
    VIDEO_OVERLAY_PROGRESS,
    VIDEO_OVERLAY_SCRUB,
    VIDEO_OVERLAY_VOLUME,
};

struct video_osd
{
    const struct video_play_options *options;
    const char *title;
    volatile bool *decode_paused;
    volatile bool *decode_busy;
    enum video_overlay mode;
    long until;
    long seek_until;
    uint32_t seek_ms;
    bool pending_seek;
};

struct video_resume_record
{
    uint32_t magic;
    uint32_t path_crc;
    int32_t frame;
    int32_t total_frames;
};

struct video_pool
{
    uint8_t *cursor;
    uint8_t *end;
};

struct video_frame
{
    uint8_t *pixels;
    int width;
    int height;
};

enum video_stage
{
    STAGE_READ, STAGE_DECODE, STAGE_VPU_PREP, STAGE_VPU_P, STAGE_VPU_I,
    STAGE_SCALE, STAGE_SCALE_OSD, STAGE_LCD_PREP, STAGE_LCD_PREP_OSD,
    STAGE_LCD, STAGE_LCD_OSD, STAGE_PRESENT,
    STAGE_COUNT,
};

#define VIDEO_HIST_BINS 64

struct video_latency
{
    uint32_t sum, max;
    unsigned int count;
    uint16_t bins[VIDEO_HIST_BINS];
};

struct video_sync_stats
{
    struct video_latency latency[STAGE_COUNT];
    unsigned int decoded, shown, late_enqueue, late_present, skipped_to_idr;
    unsigned int timeouts, status_failures, queue_peak, queue_empty;
    uint32_t max_frame_age;
    long report_at;
    bool reported;
};

static struct video_sync_stats stats;

static bool video_enqueue_late(bool grace, uint32_t pts, uint32_t clock)
{
    return !grace && pts < clock;
}

static uint32_t video_time_us(void)
{
    return rb->hw_h264->time_us ? rb->hw_h264->time_us() :
           (uint32_t)*rb->current_tick * (1000000 / HZ);
}

static void video_measure(enum video_stage stage, uint32_t elapsed)
{
#ifdef VIDEO_SYNC_LOG
    struct video_latency *latency = &stats.latency[stage];
    /* 1 ms buckets. The overflow bucket reports the observed maximum. */
    unsigned int bin = MIN(elapsed / 1000, VIDEO_HIST_BINS - 1);
    latency->bins[bin]++;
    latency->count++;
    latency->sum += elapsed;
    latency->max = MAX(latency->max, elapsed);
#else
    (void)stage;
    (void)elapsed;
#endif
}

struct video_timing
{
    uint32_t run;
    uint32_t in_run;
    uint32_t ctts_run, ctts_in_run;
    uint64_t ticks;
};

static void *video_pool_take(struct video_pool *pool, size_t size,
                             size_t alignment)
{
    uintptr_t current;
    uintptr_t aligned;

    if (pool == NULL || alignment == 0)
        return NULL;
    current = (uintptr_t)pool->cursor;
    aligned = (current + alignment - 1) & ~(uintptr_t)(alignment - 1);
    if (aligned < current || aligned > (uintptr_t)pool->end ||
        size > (size_t)(pool->end - (uint8_t *)aligned))
        return NULL;
    pool->cursor = (uint8_t *)aligned + size;
    return (void *)aligned;
}

static bool video_tables_sane(const struct mp4v_demux_res *demux)
{
    uint64_t timed_samples = 0;
    uint32_t i;
    bool video_sane =
        demux->num_samples > 0 &&
        demux->num_samples <= VIDEO_MAX_SAMPLES &&
        demux->num_stco > 0 &&
        demux->num_stco <= demux->num_samples &&
        demux->num_stsc > 0 &&
        demux->num_stsc <= demux->num_stco &&
        demux->num_stts > 0 && demux->mdat_len > 0 &&
        demux->num_stss <= demux->num_samples &&
        demux->timescale > 0;
    bool audio_sane =
        (demux->audio_num_samples == 0 &&
         demux->audio_num_stco == 0 && demux->audio_num_stsc == 0) ||
        (demux->audio_num_samples > 0 &&
         demux->audio_num_samples <= VIDEO_MAX_AUDIO_SAMPLES &&
         demux->audio_num_stco > 0 &&
         demux->audio_num_stco <= demux->audio_num_samples &&
         demux->audio_num_stsc > 0 &&
         demux->audio_num_stsc <= demux->audio_num_stco);

    if (!video_sane || !audio_sane)
        return false;
    for (i = 0; i < demux->num_stts; i++)
    {
        if (demux->stts[i].sample_count == 0 ||
            demux->stts[i].sample_delta == 0 ||
            (uint64_t)demux->stts[i].sample_delta * VIDEO_MAX_FPS <
                demux->timescale)
            return false;
        timed_samples += demux->stts[i].sample_count;
        if (timed_samples > demux->num_samples)
            return false;
    }
    if (timed_samples != demux->num_samples)
        return false;

    /* The Baseline decoder emits its pictures in decode order. Check that
     * composition offsets preserve order at every CTTS run boundary. */
    uint32_t samples = 0, run = 0, stts_end = demux->stts[0].sample_count;
    int64_t previous = 0;
    for (i = 0; i < demux->num_ctts; i++)
    {
        const struct mp4v_ctts_entry *entry = &demux->ctts[i];
        if (!entry->sample_count ||
            entry->sample_count > demux->num_samples - samples)
            return false;
        while (samples > stts_end)
            stts_end += demux->stts[++run].sample_count;
        if (samples && entry->offset - previous <
            -(int64_t)demux->stts[run].sample_delta)
            return false;
        samples += entry->sample_count;
        previous = entry->offset;
    }
    return !demux->num_ctts || samples == demux->num_samples;
}

static int64_t video_pts_ticks(const struct mp4v_demux_res *demux,
                               const struct video_timing *timing)
{
    return (int64_t)timing->ticks + (timing->ctts_run < demux->num_ctts ?
           demux->ctts[timing->ctts_run].offset : 0);
}

static uint32_t video_pts_ms(const struct mp4v_demux_res *demux,
                             const struct video_timing *timing)
{
    int64_t ticks = video_pts_ticks(demux, timing);
    uint32_t leading = 0;
    if (demux->video_edit.present)
    {
        ticks -= demux->video_edit.media_start;
        leading = mp4v_ticks_to_ms(demux->video_edit.leading,
                                    demux->movie_timescale);
    }
    uint32_t ms = mp4v_ticks_to_ms(MAX(ticks, 0), demux->timescale);
    return leading + MIN(ms, UINT32_MAX - leading);
}

static void video_timing_advance(const struct mp4v_demux_res *demux,
                                 struct video_timing *timing)
{
    const struct mp4v_stts_entry *entry;

    if (timing->run >= demux->num_stts)
        return;
    entry = &demux->stts[timing->run];
    timing->ticks += entry->sample_delta;
    timing->in_run++;
    if (timing->ctts_run < demux->num_ctts &&
        ++timing->ctts_in_run == demux->ctts[timing->ctts_run].sample_count)
    {
        timing->ctts_run++;
        timing->ctts_in_run = 0;
    }
    if (timing->in_run >= entry->sample_count)
    {
        timing->run++;
        timing->in_run = 0;
    }
}

static uint32_t video_clock_ms(bool audio_master, long start_tick)
{
    return audio_master ? video_pcm_get_clock_ms() :
        (uint32_t)((uint64_t)(unsigned long)
                   (*rb->current_tick - start_tick) * 1000 / HZ);
}

#ifdef VIDEO_SYNC_LOG
static unsigned int video_p95(const struct video_latency *latency)
{
    unsigned int rank = (latency->count * 95 + 99) / 100;
    unsigned int count = 0;

    if (!rank)
        return 0;
    for (unsigned int i = 0; i < VIDEO_HIST_BINS - 1; i++)
    {
        count += latency->bins[i];
        if (count >= rank)
            return MIN(latency->max, (i + 1) * 1000 - 1);
    }
    return latency->max;
}

static void video_sync_log(uint32_t media, uint32_t audio, uint32_t shown,
                           unsigned int fill, bool force)
{
    long now = *rb->current_tick;
    if (!force && TIME_BEFORE(now, stats.report_at))
        return;

    /* Both video threads run on the foreground core. Copy the window
     * before file I/O can let the decode worker run. */
    static struct video_sync_stats window;
    window = stats;
    rb->memset(&stats, 0, sizeof(stats));
    stats.report_at = now + 10 * HZ;
    stats.reported = window.reported;
#ifdef VIDEO_ROCK_WORKER
    struct rock_h264_audio_progress pcm;
    bool have_pcm = rb->h264_audio && rb->h264_audio->progress &&
                    rb->h264_audio->progress(&pcm);
#endif
    int fd = rb->open(VIDEO_SYNC_LOG, O_WRONLY | O_CREAT |
                      (stats.reported ? O_APPEND : O_TRUNC), 0666);
    if (fd < 0)
        return;

    static const char *const names[] = {
        "read", "decode", "vpu_prep", "vpu_p", "vpu_i", "scale",
        "scale_osd", "lcd_prep", "lcd_prep_osd", "lcd", "lcd_osd",
        "present_error",
    };
    rb->fdprintf(fd, "media_ms=%u audio_ms=%u shown_pts_ms=%u "
                 "offset_ms=%d decoded=%u shown=%u dropped=%u "
                 "late_enqueue=%u late_present=%u skipped_to_idr=%u "
                 "vpu_timeouts=%u vpu_status_failures=%u "
                 "queue=%u/%u peak=%u empty_polls=%u max_frame_age_ms=%u\n",
                 (unsigned)media, (unsigned)audio, (unsigned)shown,
                 (int)(audio - shown), window.decoded, window.shown,
                 window.late_enqueue + window.late_present +
                 window.skipped_to_idr, window.late_enqueue,
                 window.late_present, window.skipped_to_idr,
                 window.timeouts, window.status_failures,
                 fill, VIDEO_QUEUE_DEPTH, window.queue_peak,
                 window.queue_empty, (unsigned)window.max_frame_age);
    for (unsigned int i = 0; i < STAGE_COUNT; i++)
    {
        const struct video_latency *stage = &window.latency[i];
        if (stage->count)
            rb->fdprintf(fd, "%s_us n=%u avg=%u p95=%u max=%u\n",
                         names[i], stage->count,
                         (unsigned)(stage->sum / stage->count),
                         video_p95(stage), (unsigned)stage->max);
    }
#ifdef VIDEO_ROCK_WORKER
    if (have_pcm)
        rb->fdprintf(fd, "pcm_base_ms=%u rate=%u mix_content_frames=%u "
                     "clock_content_frames=%u dma_total_frames=%u "
                     "underrun_frames=%u pcm_queued_frames=%u\n",
                     (unsigned)pcm.media_base_ms, (unsigned)pcm.rate,
                     (unsigned)pcm.delivered, (unsigned)pcm.completed,
                     (unsigned)pcm.dma_frames, (unsigned)pcm.underrun_frames,
                     (unsigned)pcm.queued);
#endif
    rb->close(fd);
    stats.reported = true;
}
#endif

static uint32_t video_duration_ms(const struct mp4v_demux_res *demux)
{
    uint64_t ticks = 0;
    uint32_t i;

    if (demux->video_edit.present)
    {
        if (demux->video_edit.duration > UINT64_MAX - demux->video_edit.leading)
            return 0;
        return mp4v_ticks_to_ms(demux->video_edit.leading +
                               demux->video_edit.duration,
                               demux->movie_timescale);
    }
    for (i = 0; i < demux->num_stts; i++)
        ticks += (uint64_t)demux->stts[i].sample_count *
                 demux->stts[i].sample_delta;
    int64_t end = ticks;
    if (demux->num_ctts)
        end += demux->ctts[demux->num_ctts - 1].offset;
    return mp4v_ticks_to_ms(MAX(end, 0), demux->timescale);
}

static void video_timing_for_sample(const struct mp4v_demux_res *demux,
                                    uint32_t sample,
                                    struct video_timing *timing)
{
    uint32_t remaining = sample;

    rb->memset(timing, 0, sizeof(*timing));
    while (timing->ctts_run < demux->num_ctts)
    {
        uint32_t count = demux->ctts[timing->ctts_run].sample_count;
        if (remaining < count)
        {
            timing->ctts_in_run = remaining;
            break;
        }
        remaining -= count;
        timing->ctts_run++;
    }
    remaining = sample;
    while (timing->run < demux->num_stts)
    {
        const struct mp4v_stts_entry *entry =
            &demux->stts[timing->run];

        if (remaining < entry->sample_count)
        {
            timing->in_run = remaining;
            timing->ticks += (uint64_t)remaining * entry->sample_delta;
            return;
        }
        timing->ticks += (uint64_t)entry->sample_count *
                         entry->sample_delta;
        remaining -= entry->sample_count;
        timing->run++;
    }
}

static uint32_t video_sample_for_ms(const struct mp4v_demux_res *demux,
                                    uint32_t target_ms,
                                    struct video_timing *timing)
{
    uint32_t lo = 0, hi = demux->num_samples;
    while (lo + 1 < hi)
    {
        uint32_t mid = lo + (hi - lo) / 2;
        video_timing_for_sample(demux, mid, timing);
        if (video_pts_ms(demux, timing) <= target_ms)
            lo = mid;
        else
            hi = mid;
    }
    video_timing_for_sample(demux, lo, timing);
    return lo;
}

static void video_resume_filename(const char *path, char *filename,
                                  size_t size, uint32_t *crc_out)
{
    uint32_t crc = rb->crc_32(path, rb->strlen(path), 0xffffffff);

    rb->snprintf(filename, size, VIDEO_RESUME_FILE, (unsigned long)crc);
    if (crc_out != NULL)
        *crc_out = crc;
}

static uint32_t video_resume_load(const char *path, uint32_t total_samples)
{
    struct video_resume_record record;
    char filename[MAX_PATH];
    uint32_t crc;
    int fd;

    video_resume_filename(path, filename, sizeof(filename), &crc);
    fd = rb->open(filename, O_RDONLY);
    if (fd < 0)
        return 0;
    if (rb->read(fd, &record, sizeof(record)) != sizeof(record))
    {
        rb->close(fd);
        return 0;
    }
    rb->close(fd);
    if (record.magic != VIDEO_RESUME_MAGIC || record.path_crc != crc ||
        record.total_frames != (int32_t)total_samples || record.frame <= 0 ||
        record.frame >= (int32_t)total_samples)
        return 0;
    return (uint32_t)record.frame;
}

static void video_resume_save(const char *path, uint32_t sample,
                              uint32_t total_samples)
{
    struct video_resume_record record;
    char filename[MAX_PATH];
    uint32_t crc;
    int fd;

    video_resume_filename(path, filename, sizeof(filename), &crc);
    if (sample == 0 || sample >= total_samples)
    {
        rb->remove(filename);
        return;
    }
    rb->mkdir(PLUGIN_APPS_DATA_DIR);
    record.magic = VIDEO_RESUME_MAGIC;
    record.path_crc = crc;
    record.frame = (int32_t)sample;
    record.total_frames = (int32_t)total_samples;
    fd = rb->open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return;
    (void)rb->write(fd, &record, sizeof(record));
    rb->close(fd);
}

static void video_resume_clear(const char *path)
{
    char filename[MAX_PATH];

    video_resume_filename(path, filename, sizeof(filename), NULL);
    rb->remove(filename);
}

static void video_volume_change(int delta)
{
    int minimum = rb->sound_min(SOUND_VOLUME);
    int maximum = rb->sound_max(SOUND_VOLUME);
    int volume = rb->global_status->volume + delta;

    if (volume < minimum)
        volume = minimum;
    if (volume > maximum)
        volume = maximum;
    if (volume != rb->global_status->volume)
        rb->sound_set(SOUND_VOLUME, volume);
}

static void video_set_paused(bool pause, bool *paused, long *start_tick,
                             long *pause_started, bool have_audio,
                             bool *cpu_boosted, struct video_osd *osd)
{
    if (pause == *paused)
        return;
    if (pause)
    {
        *paused = true;
        if (osd->decode_paused)
            *osd->decode_paused = true;
        *pause_started = (*rb->current_tick);
        if (have_audio)
        {
            video_audio_pause();
            video_pcm_pause(true);
        }
        while (osd->decode_busy && *osd->decode_busy)
            rb->sleep(1);
        if (*cpu_boosted)
        {
            rb->cpu_boost(false);
            *cpu_boosted = false;
        }
    }
    else
    {
        if (!*cpu_boosted)
        {
            rb->cpu_boost(true);
            *cpu_boosted = true;
        }
        *paused = false;
        *start_tick += (*rb->current_tick) - *pause_started;
        if (have_audio)
        {
            video_audio_resume();
            video_pcm_pause(false);
        }
    }
}

static void video_osd_show(struct video_osd *osd, enum video_overlay mode)
{
    osd->mode = mode;
    osd->until = *rb->current_tick + VIDEO_OSD_SHOW_TICKS;
}

static void video_preview_seek(struct video_osd *osd, uint32_t position,
                               uint32_t duration, int step, long delay)
{
    int64_t target = (int64_t)(osd->pending_seek ? osd->seek_ms : position)
                     + step;
    osd->seek_ms = MAX(0, MIN(target, (int64_t)duration - 1));
    osd->pending_seek = true;
    osd->seek_until = *rb->current_tick + delay;
}

static bool video_chapter_seek(struct video_osd *osd, uint32_t position,
                               uint32_t duration, bool forward)
{
    const struct video_play_options *options = osd->options;
    for (size_t i = 0; i < options->chapter_count; i++)
    {
        uint32_t chapter = options->chapters[i];
        if (chapter >= duration)
            break;
        if (forward)
        {
            if (chapter <= position)
                continue;
        }
        else
        {
            if (chapter > position || position - chapter < 3000)
                break;
        }
        osd->seek_ms = chapter;
        if (forward)
            return true;
    }

    return !forward && osd->seek_ms > 0;
}

static enum video_input_action video_input(bool *paused, long *start_tick,
                                           long *pause_started,
                                           bool have_audio,
                                           bool *cpu_boosted,
                                           struct video_osd *osd,
                                           uint32_t position, uint32_t duration,
                                           bool *redraw)
{
    int action = rb->get_action(CONTEXT_WPS, TIMEOUT_NOBLOCK);
    bool stock = osd->options != NULL;

    *redraw = false;
    switch (action)
    {
    case ACTION_NONE:
        if (osd->pending_seek &&
            !TIME_BEFORE(*rb->current_tick, osd->seek_until))
            return VIDEO_INPUT_SEEK;
        if (osd->mode != VIDEO_OVERLAY_HIDDEN &&
            !TIME_BEFORE(*rb->current_tick, osd->until))
        {
            osd->mode = VIDEO_OVERLAY_HIDDEN;
            *redraw = true;
        }
        break;
    case ACTION_WPS_MENU:
        return VIDEO_INPUT_MENU;
    case ACTION_WPS_STOP:
        return VIDEO_INPUT_EXIT;
    case ACTION_WPS_BROWSE:
    {
        enum video_overlay mode = VIDEO_OVERLAY_PROGRESS;
        if (osd->mode == VIDEO_OVERLAY_PROGRESS || !stock)
            mode = VIDEO_OVERLAY_SCRUB;
        if (osd->mode == VIDEO_OVERLAY_SCRUB)
            mode = VIDEO_OVERLAY_HIDDEN;
        video_osd_show(osd, mode);
        *redraw = true;
        if (osd->pending_seek)
            return VIDEO_INPUT_SEEK;
        break;
    }
    case ACTION_WPS_VOLUP:
    case ACTION_WPS_VOLDOWN:
    {
        int direction = action == ACTION_WPS_VOLUP ? 1 : -1;
        *redraw = true;
        if (osd->mode == VIDEO_OVERLAY_SCRUB)
        {
            if (!stock)
                return direction > 0 ? VIDEO_INPUT_SEEK_FORWARD :
                                       VIDEO_INPUT_SEEK_BACK;
            int step = MAX(1000, duration / 160);
            video_preview_seek(osd, position, duration, direction * step,
                               HZ / 3);
            video_osd_show(osd, VIDEO_OVERLAY_SCRUB);
        }
        else
        {
            video_volume_change(direction);
            video_osd_show(osd, VIDEO_OVERLAY_VOLUME);
        }
        break;
    }
    case ACTION_WPS_SKIPPREV:
    case ACTION_WPS_SKIPNEXT:
    {
        bool forward = action == ACTION_WPS_SKIPNEXT;
        if (!stock)
            return forward ? VIDEO_INPUT_SEEK_FORWARD : VIDEO_INPUT_SEEK_BACK;
        osd->seek_ms = 0;
        if (!video_chapter_seek(osd, position, duration, forward))
        {
            if (forward)
                return VIDEO_INPUT_NEXT;
            if (position < 3000 && osd->options->has_previous)
                return VIDEO_INPUT_PREVIOUS;
        }
        video_osd_show(osd, VIDEO_OVERLAY_PROGRESS);
        return VIDEO_INPUT_SEEK;
    }
    case ACTION_WPS_SEEKBACK:
    case ACTION_WPS_SEEKFWD:
        if (!stock)
            return action == ACTION_WPS_SEEKBACK ? VIDEO_INPUT_SEEK_BACK :
                                                  VIDEO_INPUT_SEEK_FORWARD;
        video_preview_seek(osd, position, duration,
                           action == ACTION_WPS_SEEKBACK ? -3000 : 3000, HZ);
        video_osd_show(osd, VIDEO_OVERLAY_SCRUB);
        *redraw = true;
        break;
    case ACTION_WPS_STOPSEEK:
        if (osd->pending_seek)
        {
            video_osd_show(osd, VIDEO_OVERLAY_PROGRESS);
            return VIDEO_INPUT_SEEK;
        }
        break;
    case ACTION_WPS_PLAY:
        video_set_paused(!*paused, paused, start_tick, pause_started,
                         have_audio, cpu_boosted, osd);
        video_osd_show(osd, VIDEO_OVERLAY_PROGRESS);
        *redraw = true;
        break;
    default:
        if (rb->default_event_handler(action) == SYS_USB_CONNECTED)
            return VIDEO_INPUT_USB;
        break;
    }
    return VIDEO_INPUT_NONE;
}

enum video_menu_item
{
    VIDEO_MENU_RESUME = 0,
    VIDEO_MENU_QUIT,
};

static enum video_input_action video_show_menu(
    bool *paused, long *start_tick, long *pause_started, bool have_audio,
    bool *cpu_boosted, struct video_osd *osd, bool *redraw)
{
    bool resume_after = !*paused;
    int selection;

    MENUITEM_STRINGLIST(menu, "H.264 Player", NULL,
                        ID2P(LANG_RESUME_PLAYBACK),
                        ID2P(LANG_MENU_QUIT));

    video_set_paused(true, paused, start_tick, pause_started, have_audio,
                     cpu_boosted, osd);
    rb->button_clear_queue();
    selection = rb->do_menu(&menu, NULL, NULL, false);
    rb->button_clear_queue();
    if (selection == MENU_ATTACHED_USB)
        return VIDEO_INPUT_USB;
    if (selection == VIDEO_MENU_QUIT)
        return VIDEO_INPUT_EXIT;
    if (resume_after)
        video_set_paused(false, paused, start_tick, pause_started,
                         have_audio, cpu_boosted, osd);
    video_osd_show(osd, VIDEO_OVERLAY_PROGRESS);
    *redraw = true;
    return VIDEO_INPUT_NONE;
}

static void video_scale_plane(uint8_t *destination, int destination_stride,
                              int destination_width, int destination_height,
                              const uint8_t *source, int source_stride,
                              int source_width, int source_height)
{
    uint32_t source_y = 0;
    uint32_t step_y;
    uint32_t step_x;
    int row;

    if (destination_width <= 0 || destination_height <= 0 ||
        source_width <= 0 || source_height <= 0)
        return;
    step_x = ((uint32_t)source_width << 16) / destination_width;
    step_y = ((uint32_t)source_height << 16) / destination_height;
    for (row = 0; row < destination_height; row++)
    {
        const uint8_t *source_row = source +
            (source_y >> 16) * source_stride;
        uint8_t *destination_row = destination +
            row * destination_stride;
        uint32_t source_x = 0;
        int column;

        for (column = 0; column < destination_width; column++)
        {
            destination_row[column] = source_row[source_x >> 16];
            source_x += step_x;
        }
        source_y += step_y;
    }
}

static uint8_t video_clamp_yuv(int value)
{
    if (value < 0)
        return 0;
    if (value > 255)
        return 255;
    return (uint8_t)value;
}

static void video_yuv_color(int red, int green, int blue,
                            uint8_t *y, uint8_t *u, uint8_t *v)
{
    *y = video_clamp_yuv(
        ((66 * red + 129 * green + 25 * blue + 128) >> 8) + 16);
    *u = video_clamp_yuv(
        ((-38 * red - 74 * green + 112 * blue + 128) >> 8) + 128);
    *v = video_clamp_yuv(
        ((112 * red - 94 * green - 18 * blue + 128) >> 8) + 128);
}

static void video_yuv_pixel(uint8_t * const *planes, int x, int y,
                            int red, int green, int blue)
{
    uint8_t py;
    uint8_t pu;
    uint8_t pv;

    if (x < 0 || x >= LCD_WIDTH || y < 0 || y >= LCD_HEIGHT)
        return;
    video_yuv_color(red, green, blue, &py, &pu, &pv);
    planes[0][y * LCD_WIDTH + x] = py;
    planes[1][(y / 2) * (LCD_WIDTH / 2) + x / 2] = pu;
    planes[2][(y / 2) * (LCD_WIDTH / 2) + x / 2] = pv;
}

static void video_yuv_rect(uint8_t * const *planes, int x, int y,
                           int width, int height,
                           int red, int green, int blue)
{
    int left = MAX(0, x), right = MIN(LCD_WIDTH, x + width);
    int top = MAX(0, y), bottom = MIN(LCD_HEIGHT, y + height);
    uint8_t py, pu, pv;

    if (left >= right || top >= bottom)
        return;
    video_yuv_color(red, green, blue, &py, &pu, &pv);
    for (int row = top; row < bottom; row++)
        rb->memset(planes[0] + row * LCD_WIDTH + left, py, right - left);
    for (int row = top / 2; row < (bottom + 1) / 2; row++)
    {
        int offset = row * (LCD_WIDTH / 2) + left / 2;
        int count = (right + 1) / 2 - left / 2;
        rb->memset(planes[1] + offset, pu, count);
        rb->memset(planes[2] + offset, pv, count);
    }
}

static int video_yuv_text_width(const char *text)
{
    struct font *font = rb->font_get(FONT_SYSFIXED);
    int width = 0;

    if (font == NULL)
        return 0;
    while (*text != '\0')
        width += rb->font_get_width(font, (unsigned char)*text++);
    return width;
}

static void video_yuv_text(uint8_t * const *planes, int x, int y, int limit,
                           const char *text,
                           int red, int green, int blue)
{
    struct font *font = rb->font_get(FONT_SYSFIXED);

    if (font == NULL || font->depth != 0)
        return;
    while (*text != '\0' && x < limit)
    {
        unsigned char ch = *text++;
        if (ch >= 128)
        {
            while (((unsigned char)*text & 0xc0) == 0x80)
                text++;
            ch = '?';
        }
        int glyph_width = rb->font_get_width(font, ch);
        if (x + glyph_width > limit)
            break;
        const unsigned char *bits = rb->font_get_bits(font, ch);
        int column;

        for (column = 0; column < glyph_width; column++)
        {
            const unsigned char *source = bits + column;
            int row;

            for (row = 0; row < (int)font->height; row++)
                if (source[(row >> 3) * glyph_width] &
                    (1u << (row & 7)))
                    video_yuv_pixel(planes, x + column, y + row,
                                    red, green, blue);
        }
        x += glyph_width;
    }
}

static void video_time_text(uint32_t milliseconds, char *text, size_t size)
{
    uint32_t seconds = milliseconds / 1000u;

    if (seconds >= 3600u)
        rb->snprintf(text, size, "%lu:%02lu:%02lu",
                 (unsigned long)(seconds / 3600u),
                 (unsigned long)((seconds / 60u) % 60u),
                 (unsigned long)(seconds % 60u));
    else
        rb->snprintf(text, size, "%lu:%02lu",
                 (unsigned long)(seconds / 60u),
                 (unsigned long)(seconds % 60u));
}

static void video_draw_status_icon(uint8_t * const *planes, int x, int y,
                                   bool paused)
{
    int row;

    if (paused)
    {
        video_yuv_rect(planes, x, y, 2, 9, 255, 255, 255);
        video_yuv_rect(planes, x + 5, y, 2, 9, 255, 255, 255);
        return;
    }
    for (row = 0; row < 9; row++)
    {
        int width = row <= 4 ? row + 1 : 9 - row;

        video_yuv_rect(planes, x, y + row, width, 1, 255, 255, 255);
    }
}

/* Darken an even-aligned region without flattening the picture behind it. */
static void video_dim_panel(uint8_t * const *planes, int x, int y,
                            int width, int height)
{
    for (int plane = 0; plane < 3; plane++)
    {
        int shift = plane != 0;
        int stride = LCD_WIDTH >> shift;
        int black = plane ? 128 : 16;
        uint8_t *row = planes[plane] + (y >> shift) * stride + (x >> shift);
        for (int line = 0; line < (height >> shift); line++, row += stride)
            for (int col = 0; col < (width >> shift); col++)
                row[col] = (row[col] + 3 * black) / 4;
    }
}

static void video_draw_themed_controls(uint8_t * const *planes, bool paused,
                                       uint32_t position, uint32_t duration,
                                       const struct video_osd *osd)
{
    const int left = 16, right = LCD_WIDTH - left;
    const int panel = LCD_HEIGHT - 44;
    const int rail = panel + 12, width = right - left;
    if (osd->mode == VIDEO_OVERLAY_HIDDEN)
    {
        if (paused)
        {
            video_dim_panel(planes, LCD_WIDTH - 36, 8, 28, 22);
            video_draw_status_icon(planes, LCD_WIDTH - 26, 15, true);
        }
        return;
    }

    unsigned accent = osd->options->accent;
    int red = ((accent >> 11) & 31) * 255 / 31;
    int green = ((accent >> 5) & 63) * 255 / 63;
    int blue = (accent & 31) * 255 / 31;
    int fill;
    char left_text[20], right_text[20];

    video_dim_panel(planes, 8, panel, LCD_WIDTH - 16, 36);
    if (osd->mode == VIDEO_OVERLAY_VOLUME)
    {
        int min = rb->sound_min(SOUND_VOLUME);
        int max = osd->options->max_volume;
        int volume = MAX(min, MIN(max, rb->global_status->volume));
        int percent = max > min ? (volume - min) * 100 / (max - min) : 0;
        fill = percent * width / 100;
        rb->snprintf(left_text, sizeof(left_text), "Volume");
        rb->snprintf(right_text, sizeof(right_text), "%d%%", percent);
    }
    else
    {
        video_dim_panel(planes, 8, 8, LCD_WIDTH - 16, 22);
        if (paused)
            video_draw_status_icon(planes, left, 15, true);
        int title_right = right;
        int battery = osd->options->battery_level ?
                      osd->options->battery_level() : -1;
        if (battery >= 0)
        {
            title_right -= 24;
            video_yuv_rect(planes, right - 18, 16, 16, 7, 122, 129, 136);
            video_yuv_rect(planes, right - 2, 18, 2, 3, 122, 129, 136);
            video_yuv_rect(planes, right - 18, 16, MIN(battery, 100) * 16 / 100,
                           7, 255, 255, 255);
        }
        video_yuv_text(planes, paused ? left + 16 : left, 15, title_right,
                       osd->title, 255, 255, 255);
        position = MIN(osd->pending_seek ? osd->seek_ms : position, duration);
        fill = (uint64_t)position * width / duration;
        video_time_text(position, left_text, sizeof(left_text));
        right_text[0] = '-';
        video_time_text(duration - position, right_text + 1,
                        sizeof(right_text) - 1);
    }
    video_yuv_rect(planes, left, rail, width, 4, 122, 129, 136);
    video_yuv_rect(planes, left, rail, fill, 4, red, green, blue);
    if (osd->mode == VIDEO_OVERLAY_SCRUB)
        video_yuv_rect(planes, left + fill - 2, rail - 4, 4, 12, 255, 255, 255);
    video_yuv_text(planes, left, panel + 23, right, left_text, 255, 255, 255);
    video_yuv_text(planes, right - video_yuv_text_width(right_text), panel + 23,
                   right, right_text, 255, 255, 255);
}

static void video_draw_overlay(uint8_t * const *planes, bool paused,
                               uint32_t position_ms, uint32_t duration_ms,
                               const struct video_osd *osd)
{
    if (osd->options && osd->options->themed_controls)
    {
        video_draw_themed_controls(planes, paused, position_ms, duration_ms, osd);
        return;
    }
    if (osd->mode == VIDEO_OVERLAY_HIDDEN)
        return;

    const int left = 12;
    const int right = LCD_WIDTH - left;
    const int bar_width = LCD_WIDTH - 2 * left;
    const int bottom = LCD_HEIGHT - 34;
    const int accent_r = 111, accent_g = 195, accent_b = 178;
    int fill;
    char elapsed[20], remaining[20];

    if (osd->pending_seek)
        position_ms = osd->seek_ms;
    position_ms = MIN(position_ms, duration_ms);

    video_yuv_rect(planes, 0, 0, LCD_WIDTH, 20, 18, 24, 28);
    video_draw_status_icon(planes, left, 5, paused);
    video_yuv_text(planes, left + 15, 6, right - 28, osd->title, 241, 245, 245);
    if (osd->options && osd->options->battery_level)
    {
        int percent = osd->options->battery_level();
        if (percent >= 0)
        {
            video_yuv_rect(planes, right - 19, 5, 18, 10, 182, 194, 193);
            video_yuv_rect(planes, right - 1, 8, 2, 4, 182, 194, 193);
            video_yuv_rect(planes, right - 18, 6, 16, 8, 18, 24, 28);
            video_yuv_rect(planes, right - 18, 6, MIN(percent, 100) * 16 / 100,
                           8, accent_r, accent_g, accent_b);
        }
    }

    video_yuv_rect(planes, 0, bottom, LCD_WIDTH, 34, 18, 24, 28);
    video_yuv_rect(planes, left, bottom + 21, bar_width, 4, 66, 79, 84);
    if (osd->mode == VIDEO_OVERLAY_VOLUME)
    {
        int min = rb->sound_min(SOUND_VOLUME);
        int max = osd->options ? osd->options->max_volume : 0;
        int volume = MAX(min, MIN(max, rb->global_status->volume));
        fill = max > min ? (volume - min) * bar_width / (max - min) : 0;
        video_yuv_text(planes, left, bottom + 7, right, "Volume",
                       241, 245, 245);
    }
    else
    {
        fill = (uint64_t)position_ms * bar_width / duration_ms;
        video_time_text(position_ms, elapsed, sizeof(elapsed));
        remaining[0] = '-';
        video_time_text(duration_ms - position_ms, remaining + 1,
                        sizeof(remaining) - 1);
        video_yuv_text(planes, left, bottom + 7, right, elapsed, 241, 245, 245);
        video_yuv_text(planes, right - video_yuv_text_width(remaining),
                       bottom + 7, right, remaining, 241, 245, 245);
    }
    video_yuv_rect(planes, left, bottom + 21, fill, 4,
                   accent_r, accent_g, accent_b);
    if (osd->mode == VIDEO_OVERLAY_SCRUB)
        video_yuv_rect(planes, left + fill - 2, bottom + 18, 5, 10,
                       241, 245, 245);
}

static void video_draw_frame(const uint8_t *y, const uint8_t *cb,
                             const uint8_t *cr, int width, int height,
                             int stride, uint8_t *scaled, bool paused,
                             uint32_t position_ms, uint32_t duration_ms,
                             const struct video_osd *osd)
{
    unsigned char *planes[3];
    int draw_width;
    int draw_height;
    int x;
    int y_pos;

    if (y == NULL || cb == NULL || cr == NULL || scaled == NULL ||
        width <= 0 || height <= 0 || stride < width)
        return;

    draw_width = LCD_WIDTH;
    draw_height = (int)((int64_t)height * LCD_WIDTH / width);
    if (draw_height > LCD_HEIGHT)
    {
        draw_height = LCD_HEIGHT;
        draw_width = (int)((int64_t)width * LCD_HEIGHT / height);
    }
    draw_width &= ~1;
    draw_height &= ~1;
    x = ((LCD_WIDTH - draw_width) / 2) & ~1;
    y_pos = ((LCD_HEIGHT - draw_height) / 2) & ~1;

    uint32_t started = video_time_us();
    planes[0] = scaled;
    planes[1] = scaled + LCD_WIDTH * LCD_HEIGHT;
    planes[2] = planes[1] + LCD_WIDTH * LCD_HEIGHT / 4;
    rb->memset(planes[0], 16, LCD_WIDTH * LCD_HEIGHT);
    rb->memset(planes[1], 128, LCD_WIDTH * LCD_HEIGHT / 4);
    rb->memset(planes[2], 128, LCD_WIDTH * LCD_HEIGHT / 4);
    video_scale_plane(planes[0] + y_pos * LCD_WIDTH + x, LCD_WIDTH,
                      draw_width, draw_height, y, stride, width, height);
    video_scale_plane(planes[1] + (y_pos / 2) * (LCD_WIDTH / 2) + x / 2,
                      LCD_WIDTH / 2, draw_width / 2, draw_height / 2,
                      cb, stride / 2, width / 2, height / 2);
    video_scale_plane(planes[2] + (y_pos / 2) * (LCD_WIDTH / 2) + x / 2,
                      LCD_WIDTH / 2, draw_width / 2, draw_height / 2,
                      cr, stride / 2, width / 2, height / 2);
    video_draw_overlay(planes, paused, position_ms, duration_ms,
                       osd);
    bool overlay = osd->mode != VIDEO_OVERLAY_HIDDEN || paused;
    uint32_t submitted = video_time_us();
    video_measure(overlay ? STAGE_SCALE_OSD : STAGE_SCALE, submitted - started);
    if (rb->hw_h264->present)
    {
        struct h264_lcd_timing timing = {0};
        rb->hw_h264->present(planes, &timing);
        if (timing.submitted)
        {
            video_measure(overlay ? STAGE_LCD_PREP_OSD : STAGE_LCD_PREP,
                          timing.submit_us - submitted);
            video_measure(overlay ? STAGE_LCD_OSD : STAGE_LCD,
                          timing.complete_us - timing.submit_us);
        }
    }
    else
        rb->lcd_blit_yuv(planes, 0, 0, LCD_WIDTH, 0, 0, LCD_WIDTH, LCD_HEIGHT);
}

static void video_redraw_frame(const struct video_frame *frame,
                               uint8_t *scaled, bool paused,
                               uint32_t position_ms, uint32_t duration_ms,
                               const struct video_osd *osd)
{
    size_t y_size = (size_t)frame->width * frame->height;
    video_draw_frame(frame->pixels, frame->pixels + y_size,
                     frame->pixels + y_size + y_size / 4,
                     frame->width, frame->height, frame->width,
                     scaled, paused, position_ms, duration_ms, osd);
}

/* A decoded picture can be waiting for its PTS or can be dropped. Keep the
 * last presented picture separate so a paused redraw never shows either. */
static bool video_cache_frame(struct vpu_h264 *decoder,
                              struct video_frame *frame, size_t capacity)
{
    const uint8_t *planes[3] = {0};
    int width = 0, height = 0, stride = 0;

    rb->hw_h264->get_frame(decoder, &planes[0], &planes[1], &planes[2],
                          &width, &height, &stride);
    if (width < 2 || height < 2 || width > VIDEO_MAX_WIDTH ||
        height > VIDEO_MAX_HEIGHT || (width | height | stride) % 2 != 0 ||
        stride < width || (size_t)width * height * 3 / 2 > capacity ||
        !planes[0] || !planes[1] || !planes[2])
        return false;

    frame->width = width;
    frame->height = height;
    uint8_t *dst = frame->pixels;
    for (unsigned i = 0; i < 3; ++i)
    {
        int shift = i != 0;
        for (int row = 0; row < (height >> shift); ++row)
        {
            memcpy(dst, planes[i] + row * (stride >> shift),
                   width >> shift);
            dst += width >> shift;
        }
    }
    return true;
}

static struct mp4v_demux_res demux;

struct video_picture
{
    struct video_frame frame;
    uint32_t pts_ms, sample;
};

struct video_pipeline
{
    struct video_picture queue[VIDEO_QUEUE_DEPTH];
    volatile unsigned int head, tail;
    volatile bool stop, paused, preview, finished, ready, priming, busy;
    volatile bool audio_master;
    volatile long start_tick;
    bool first;
    int fd, error;
    bool read_failed;
    uint32_t offset, size;
    struct vpu_h264 *decoder;
    struct video_timing timing;
    uint32_t sample, show_sample, duration_ms, next_catch_up_ms;
    size_t frame_bytes;
    struct mp4v_read_window read;
};

static unsigned int video_queue_fill(const struct video_pipeline *pipe)
{
    return pipe->tail - pipe->head;
}

/* A sync-table entry must contain an IDR before playback can skip to it. */
static bool video_sample_is_idr(const uint8_t *data, size_t size)
{
    unsigned int prefix = demux.nalu_len_size;
    while (size > prefix)
    {
        uint32_t length = 0;
        for (unsigned int i = 0; i < prefix; i++)
            length = (length << 8) | *data++;
        size -= prefix;
        if (!length || length > size)
            return false;
        unsigned int type = *data & 31;
        if (type == 5)
            return true;
        if (type == 1)
            return false;
        data += length;
        size -= length;
    }
    return false;
}

static uint32_t video_catch_up(struct video_pipeline *pipe, uint32_t pts)
{
    if (pipe->priming || pipe->paused || !pipe->audio_master ||
        !demux.num_stss || video_queue_fill(pipe))
        return pipe->sample;
    uint32_t clock = video_clock_ms(true, pipe->start_tick);
    if (clock < pipe->next_catch_up_ms || clock <= pts ||
        clock - pts < VIDEO_CATCH_UP_MS)
        return pipe->sample;

    pipe->next_catch_up_ms = clock + VIDEO_CATCH_UP_RETRY_MS;
    struct video_timing timing;
    uint32_t target = video_sample_for_ms(&demux,
        clock + VIDEO_CATCH_UP_LEAD_MS, &timing);
    uint32_t sync;
    if (mp4v_find_keyframe(pipe->fd, &demux, target, &sync) &&
        sync > pipe->sample)
        return sync;
    return pipe->sample;
}

/* Decode in file order. The queue owns copies of visible planes, so
 * dropping or presenting a queued picture cannot evict a reference. */
static void video_decode_step(struct video_pipeline *pipe)
{
    if (pipe->finished || (pipe->paused && !pipe->preview) ||
        video_queue_fill(pipe) == VIDEO_QUEUE_DEPTH)
        return;

    uint32_t sample = pipe->sample;
    if (sample == demux.num_samples)
    {
        pipe->finished = true;
        return;
    }

    uint32_t pts_ms = video_pts_ms(&demux, &pipe->timing);
    if (pts_ms >= pipe->duration_ms)
    {
        pipe->finished = true;
        return;
    }
    uint32_t start = video_time_us();
    uint32_t offset = 0, size = 0;
    const uint8_t *compressed;
    uint32_t candidate = video_catch_up(pipe, pts_ms);
    int result = -1;
    if (candidate > sample &&
        mp4v_read_sample(pipe->fd, &demux, candidate, &pipe->read,
                         &compressed, &offset, &size) == 0 &&
        video_sample_is_idr(compressed, size))
    {
        stats.skipped_to_idr += candidate - sample;
        pipe->sample = sample = candidate;
        video_timing_for_sample(&demux, sample, &pipe->timing);
        pts_ms = video_pts_ms(&demux, &pipe->timing);
        result = 0;
    }
    if (result < 0)
        result = mp4v_read_sample(pipe->fd, &demux, sample, &pipe->read,
                                  &compressed, &offset, &size);
    int64_t pts_ticks = video_pts_ticks(&demux, &pipe->timing);
    bool visible = pts_ms < pipe->duration_ms &&
                   pts_ticks + demux.stts[pipe->timing.run].sample_delta >
                   demux.video_edit.media_start;
    video_measure(STAGE_READ, video_time_us() - start);
    if (result < 0)
    {
        pipe->read_failed = true;
        goto failed;
    }
    if (pipe->stop || (pipe->paused && !pipe->preview))
        return;

    pipe->busy = true;
    start = video_time_us();
    result = rb->hw_h264->decode_sample(pipe->decoder, compressed,
                                        size, demux.nalu_len_size);
    pipe->busy = false;
    video_measure(STAGE_DECODE, video_time_us() - start);
    if (rb->hw_h264->decode_timing)
    {
        const struct h264_decode_timing *timing =
            rb->hw_h264->decode_timing(pipe->decoder);
        if (timing && (timing->submit_us || timing->complete_us))
        {
            video_measure(STAGE_VPU_PREP, timing->submit_us - start);
            if (result != H264_DECODE_TIMEOUT)
                video_measure(timing->picture_type == 2 ? STAGE_VPU_I :
                              STAGE_VPU_P,
                              timing->complete_us - timing->submit_us);
        }
    }
    if (result < 0)
    {
        stats.timeouts += result == H264_DECODE_TIMEOUT;
        stats.status_failures += result == H264_DECODE_STATUS_ERROR;
        goto failed;
    }
    stats.decoded += result > 0;
    if (result > 0 && visible && sample >= pipe->show_sample)
    {
        struct video_picture *pic = &pipe->queue[pipe->tail % VIDEO_QUEUE_DEPTH];
        bool grace = pipe->first || pipe->paused || pipe->priming;
        uint32_t clock = video_clock_ms(pipe->audio_master, pipe->start_tick);
        if (video_enqueue_late(grace, pts_ms, clock))
            stats.late_enqueue++;
        else if (video_cache_frame(pipe->decoder, &pic->frame, pipe->frame_bytes))
        {
            clock = video_clock_ms(pipe->audio_master, pipe->start_tick);
            if (video_enqueue_late(grace, pts_ms, clock))
                stats.late_enqueue++;
            else
            {
                pic->pts_ms = pts_ms;
                pic->sample = sample;
                pipe->first = false;
                pipe->preview = false;
                unsigned int fill = video_queue_fill(pipe) + 1;
                stats.queue_peak = MAX(stats.queue_peak, fill);
                pipe->tail++;
            }
        }
    }
    video_timing_advance(&demux, &pipe->timing);
    pipe->sample++;
    return;

failed:
    pipe->offset = offset;
    pipe->size = size;
    pipe->error = result;
    pipe->finished = true;
}

#ifdef VIDEO_ROCK_WORKER
static void video_decode_worker(void *arg)
{
    struct video_pipeline *pipe = arg;
    while (!pipe->stop && !rb->rock_worker_stopping() && !pipe->finished)
    {
        if (pipe->ready && (!pipe->paused || pipe->preview) &&
            video_queue_fill(pipe) < VIDEO_QUEUE_DEPTH)
        {
            video_decode_step(pipe);
            rb->yield();
        }
        else
            rb->sleep(1);
    }
}

static bool video_worker_start(struct video_pipeline *pipe,
                               void *stack, unsigned int *worker)
{
    if (!rb->rock_worker_start)
        return true; /* The host fixture drives the same state machine. */
    pipe->ready = false;
    *worker = rb->rock_worker_start(video_decode_worker, pipe,
                                    stack, VIDEO_WORKER_STACK);
    if (!*worker || !rb->hw_h264->set_decode_worker(pipe->decoder, *worker))
        return false;
    pipe->ready = true;
    return true;
}

static void video_worker_stop(struct video_pipeline *pipe, unsigned int *worker)
{
    pipe->stop = true;
    if (*worker)
    {
        rb->rock_worker_join(*worker);
        rb->hw_h264->set_decode_worker(pipe->decoder, 0);
        *worker = 0;
    }
}
#endif

static size_t video_frame_bytes(const struct mp4v_demux_res *movie)
{
    return (size_t)((movie->width + 15) & ~15) *
           ((movie->height + 15) & ~15) * 3 / 2;
}

static bool video_format_supported(const struct mp4v_demux_res *movie)
{
    return movie->format == MAKEFOURCC('a', 'v', 'c', '1') &&
           movie->avc_profile == 66 && movie->avc_level <= VIDEO_MAX_LEVEL &&
           movie->width > 0 && movie->height > 0 &&
           movie->width <= VIDEO_MAX_WIDTH &&
           movie->height <= VIDEO_MAX_HEIGHT &&
           movie->nalu_len_size >= 1 && movie->nalu_len_size <= 4;
}

size_t video_h264_scratch_size(const char *path)
{
    int fd = rb->open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    int result = mp4v_demux_read(fd, &demux);
    rb->close(fd);
    if (result < 0 || !video_format_supported(&demux) ||
        !video_tables_sane(&demux))
        return 0;

    size_t decoder = rb->hw_h264->buf_size((demux.width + 15) & ~15,
                                          (demux.height + 15) & ~15);
    if (!decoder)
        return 0;
    /* One redraw surface and one surface per queued picture. */
    size_t bytes = 4095 + decoder + VIDEO_READ_BUFFER + VIDEO_OUTPUT_BUFFER +
                   (VIDEO_QUEUE_DEPTH + 1) * video_frame_bytes(&demux) +
                   VIDEO_WORKER_STACK + 5 * 31;
    if (demux.audio_format == MAKEFOURCC('m', 'p', '4', 'a'))
        bytes += video_audio_workspace_size(&demux) + VIDEO_PCM_BUFFER +
                 2 * (CACHEALIGN_SIZE - 1);
    return (bytes + 7) & ~(size_t)7;
}

int video_h264_play_named(const char *filepath, void *buffer,
                          size_t buffer_size,
                          const struct video_play_options *options)
{
    struct video_pool pool;
    struct video_timing timing;
    struct video_frame frame = {0};
    struct video_pipeline pipe = {0};
    unsigned int worker = 0;
    struct vpu_h264 *decoder = NULL;
    uint8_t *decoder_buffer;
    uint8_t *read_buffer;
    uint8_t *scale_buffer;
    void *audio_codec_workspace;
    void *pcm_buffer;
    size_t decoder_size;
    size_t audio_codec_workspace_size;
    int video_fd = -1;
    uint32_t duration_ms;
    uint32_t displayed_ms = 0;
    uint32_t sample = 0;
    uint32_t last_sample = 0, show_sample;
    uint32_t play_ms;
    char title[42];
    struct video_osd osd = {.options = options, .title = title};
    long start_tick = 0;
    long pause_started = 0;
    bool paused = false;
    bool cpu_boosted = false;
    bool have_audio = false;
    bool have_frame = false;
    void *worker_stack;
    bool preview_frame = false;
    bool audio_master = false;

    enum video_input_action action = VIDEO_INPUT_NONE;
    int result = -1;

    rb->memset(&stats, 0, sizeof(stats));
    if (filepath == NULL || filepath[0] == '\0' || buffer == NULL ||
        buffer_size == 0)
        return -1;

    const char *leaf = filepath;
    for (const char *p = filepath; *p; ++p)
        if (*p == '/')
            leaf = p + 1;
    if (options && options->title)
        rb->snprintf(title, sizeof(title), "%s", options->title);
    else
    {
        const char *extension = NULL, *end = leaf;
        while (*end)
        {
            if (*end == '.')
                extension = end;
            end++;
        }
        rb->snprintf(title, sizeof(title), "%.*s",
                     (int)((extension ? extension : end) - leaf), leaf);
    }
    if (!options)
        rb->splash(0, "Loading video...");
    rb->cpu_boost(true);
    cpu_boosted = true;
    video_fd = rb->open(filepath, O_RDONLY);
    if (video_fd < 0 || mp4v_demux_read(video_fd, &demux) < 0)
    {
        rb->splash(HZ * 2, "Invalid MP4/M4V file");
        goto cleanup;
    }
    if (!video_format_supported(&demux))
    {
        rb->splash(HZ * 3, "Need H.264 Baseline <= L3.0\nMax 640x480");
        goto cleanup;
    }
    if (!video_tables_sane(&demux))
    {
        rb->splash(HZ * 3, "Movie tables exceed safe limit");
        goto cleanup;
    }

    pool.cursor = buffer;
    pool.end = (uint8_t *)buffer + buffer_size;
    decoder_size = rb->hw_h264->buf_size(
        (demux.width + 15) & ~15, (demux.height + 15) & ~15);
    decoder_buffer = video_pool_take(&pool, decoder_size, 4096);
    read_buffer = video_pool_take(&pool, VIDEO_READ_BUFFER, 32);
    scale_buffer = video_pool_take(&pool, VIDEO_OUTPUT_BUFFER, 32);
    pipe.frame_bytes = video_frame_bytes(&demux);
    frame.pixels = video_pool_take(&pool,
        (VIDEO_QUEUE_DEPTH + 1) * pipe.frame_bytes, 32);
    worker_stack = video_pool_take(&pool, VIDEO_WORKER_STACK, 32);
    audio_codec_workspace_size = demux.audio_num_samples > 0 ?
        video_audio_workspace_size(&demux) : 0;
    audio_codec_workspace = audio_codec_workspace_size > 0 ?
        video_pool_take(&pool, audio_codec_workspace_size,
                        CACHEALIGN_SIZE) : NULL;
    pcm_buffer = audio_codec_workspace_size > 0 ?
        video_pool_take(&pool, VIDEO_PCM_BUFFER, CACHEALIGN_SIZE) : NULL;
    if (decoder_buffer == NULL || read_buffer == NULL ||
        scale_buffer == NULL || frame.pixels == NULL || !worker_stack ||
        decoder_size == 0 ||
        (audio_codec_workspace_size > 0 &&
         (audio_codec_workspace == NULL || pcm_buffer == NULL)))
    {
        rb->splash(HZ * 2, "Not enough video memory");
        goto cleanup;
    }
    for (unsigned int i = 0; i < VIDEO_QUEUE_DEPTH; i++)
        pipe.queue[i].frame.pixels = frame.pixels + (i + 1) * pipe.frame_bytes;
    if (!options)
        rb->splash(0, "Loading movie index...");
    if (!mp4v_validate_index(video_fd, &demux))
    {
        rb->splash(HZ * 2, "MP4 table parse failed");
        goto cleanup;
    }
    duration_ms = video_duration_ms(&demux);
    if (!duration_ms || duration_ms == UINT32_MAX)
    {
        rb->splash(HZ * 2, "Unsupported movie duration");
        goto cleanup;
    }
    show_sample = video_resume_load(filepath, demux.num_samples);
    if (show_sample && options && options->resume_menu)
    {
        int choice = options->resume_menu();
        if (choice == 1)
            show_sample = 0;
        else if (choice != 0)
        {
            result = choice == MENU_ATTACHED_USB ? VIDEO_USB : VIDEO_EXIT;
            goto cleanup;
        }
    }
    last_sample = show_sample;
    video_timing_for_sample(&demux, show_sample, &timing);
    play_ms = show_sample ? video_pts_ms(&demux, &timing) : 0;
    displayed_ms = play_ms;
    sample = show_sample;
    if (!mp4v_find_keyframe(video_fd, &demux, sample, &sample))
        goto cleanup;
    video_timing_for_sample(&demux, sample, &timing);

    /* Preparation, decode, and LCD presentation need the 108 MHz bus clock.
     * Keep one boost reference from table parsing through active playback. */
    if (!options)
        rb->splash(0, "Starting video...");
    decoder = rb->hw_h264->open(
        decoder_buffer, decoder_size,
        (demux.width + 15) & ~15, (demux.height + 15) & ~15);
    if (decoder == NULL ||
        rb->hw_h264->configure(decoder, demux.codecdata,
                           demux.codecdata_len) < 0)
    {
        rb->splash(HZ * 2, "H.264 decoder init failed");
        goto cleanup;
    }

    rb->pcmbuf_fade(false, true);
    rb->sound_set(SOUND_VOLUME, rb->global_status->volume);
    start_tick = (*rb->current_tick) -
        (long)((uint64_t)play_ms * HZ / 1000u);
    if (!options)
        video_osd_show(&osd, VIDEO_OVERLAY_PROGRESS);
    if (demux.audio_format == MAKEFOURCC('m', 'p', '4', 'a') &&
        demux.audio_codecdata_len > 0 &&
        video_audio_init(filepath, &demux, audio_codec_workspace,
                         audio_codec_workspace_size, pcm_buffer,
                         VIDEO_PCM_BUFFER) == 0)
    {
        int wait = 0;
        uint32_t initial_ms = play_ms;

        have_audio = true;
        audio_master = true;
        video_audio_play();
        if (initial_ms > 0)
            video_audio_seek(initial_ms);
        while (!video_audio_ready() && wait < HZ && action == VIDEO_INPUT_NONE)
        {
            bool redraw_requested;

            action = video_input(&paused, &start_tick,
                                 &pause_started, true, &cpu_boosted,
                                 &osd, displayed_ms, duration_ms,
                                 &redraw_requested);
            if (action == VIDEO_INPUT_MENU)
                action = video_show_menu(
                    &paused, &start_tick, &pause_started, true,
                    &cpu_boosted, &osd, &redraw_requested);
            rb->sleep(1);
            wait++;
        }
        if (video_audio_failed())
        {
            video_audio_stop();
            have_audio = false;
            audio_master = false;
        }
    }

    rb->lcd_set_background(LCD_BLACK);
    rb->lcd_clear_display();
    rb->lcd_update();
    rb->backlight_on();
    rb->backlight_set_timeout(0);
    stats.report_at = *rb->current_tick + 10 * HZ;
start_pipeline:
    osd.decode_paused = &pipe.paused;
    osd.decode_busy = &pipe.busy;
    pipe.decoder = decoder;
    pipe.fd = video_fd;
    pipe.timing = timing;
    pipe.sample = sample;
    pipe.show_sample = show_sample;
    pipe.read = (struct mp4v_read_window){read_buffer, VIDEO_READ_BUFFER, 0, 0};
    pipe.duration_ms = duration_ms;
    pipe.head = pipe.tail = 0;
    pipe.stop = pipe.finished = pipe.read_failed = false;
    pipe.error = 0;
    pipe.next_catch_up_ms = 0;
    pipe.first = true;
    pipe.paused = paused;
    pipe.preview = preview_frame;
    pipe.priming = true;
    pipe.audio_master = audio_master;
    pipe.start_tick = start_tick;
#ifdef VIDEO_ROCK_WORKER
    if (!video_worker_start(&pipe, worker_stack, &worker))
    {
        rb->splash(HZ * 2, "Video worker failed");
        goto cleanup;
    }
#endif
    while (action == VIDEO_INPUT_NONE)
    {
        bool redraw_requested;
        action = video_input(&paused, &start_tick, &pause_started,
                             have_audio, &cpu_boosted,
                             &osd, displayed_ms, duration_ms,
                             &redraw_requested);
        if (action == VIDEO_INPUT_MENU)
            action = video_show_menu(
                &paused, &start_tick, &pause_started, have_audio,
                &cpu_boosted, &osd, &redraw_requested);
        if (redraw_requested && have_frame)
            video_redraw_frame(&frame, scale_buffer, paused,
                               displayed_ms, duration_ms, &osd);
        if (action == VIDEO_INPUT_SEEK ||
            action == VIDEO_INPUT_SEEK_BACK ||
            action == VIDEO_INPUT_SEEK_FORWARD)
        {
            if (have_audio)
                video_audio_output_pause(true);
#ifdef VIDEO_ROCK_WORKER
            video_worker_stop(&pipe, &worker);
#endif
            int64_t target = osd.seek_ms;
            if (action != VIDEO_INPUT_SEEK)
                target = (int64_t)displayed_ms +
                    (action == VIDEO_INPUT_SEEK_BACK ? -10000 : 10000);
            target = MAX(0, MIN(target, (int64_t)duration_ms - 1));
            osd.pending_seek = false;
            show_sample = video_sample_for_ms(&demux, (uint32_t)target,
                                              &timing);
            play_ms = video_pts_ms(&demux, &timing);
            if (!mp4v_find_keyframe(video_fd, &demux, show_sample, &sample))
                goto cleanup;
            if (!options)
            {
                show_sample = sample;
                video_timing_for_sample(&demux, sample, &timing);
                play_ms = video_pts_ms(&demux, &timing);
                video_osd_show(&osd, VIDEO_OVERLAY_PROGRESS);
            }
            video_timing_for_sample(&demux, sample, &timing);
            if (!cpu_boosted)
            {
                rb->cpu_boost(true);
                cpu_boosted = true;
            }
            rb->hw_h264->close(decoder);
            decoder = rb->hw_h264->open(
                decoder_buffer, decoder_size,
                (demux.width + 15) & ~15,
                (demux.height + 15) & ~15);
            if (decoder == NULL ||
                rb->hw_h264->configure(decoder, demux.codecdata,
                                   demux.codecdata_len) < 0)
            {
                rb->splash(HZ * 2, "H.264 seek failed");
                goto cleanup;
            }
            if (have_audio)
                video_audio_seek(play_ms);
            start_tick = *rb->current_tick -
                (long)((uint64_t)play_ms * HZ / 1000u);
            pause_started = *rb->current_tick;
            displayed_ms = play_ms;
            preview_frame = paused;
            have_frame = false;
            action = VIDEO_INPUT_NONE;
            goto start_pipeline;
        }
        if (action != VIDEO_INPUT_NONE)
            break;

        unsigned int fill = video_queue_fill(&pipe);
        if (pipe.priming && !paused &&
            (fill == VIDEO_QUEUE_DEPTH || pipe.finished) &&
            (!have_audio || video_audio_ready()))
        {
            start_tick = *rb->current_tick -
                (long)((uint64_t)play_ms * HZ / 1000u);
            if (have_audio)
                video_audio_output_pause(false);
            pipe.start_tick = start_tick;
            pipe.priming = false;
        }
        uint32_t clock_ms = video_clock_ms(audio_master, start_tick);
        if (audio_master && !pipe.priming &&
            (video_audio_failed() ||
             (!video_audio_is_active() && video_pcm_empty())))
        {
            video_audio_stop();
            have_audio = false;
            audio_master = false;
            start_tick = *rb->current_tick -
                (long)((uint64_t)clock_ms * HZ / 1000u);
        }
        pipe.start_tick = start_tick;
        pipe.audio_master = audio_master;
        pipe.paused = paused;

        if (pipe.error)
        {
            rb->splash(HZ * 2, pipe.read_failed ? "Video sample read failed" :
                                                   "VPU sample decode failed");
            goto cleanup;
        }

        fill = video_queue_fill(&pipe);
        if (have_frame && !paused && clock_ms >= displayed_ms)
            stats.max_frame_age = MAX(stats.max_frame_age, clock_ms - displayed_ms);
#ifdef VIDEO_SYNC_LOG
        if (!paused)
            video_sync_log(clock_ms, clock_ms, displayed_ms, fill, false);
#endif
        if (fill && ((!paused && !pipe.priming) || preview_frame))
        {
            struct video_picture *pic = &pipe.queue[pipe.head % VIDEO_QUEUE_DEPTH];
            if (preview_frame || clock_ms >= pic->pts_ms)
            {
                if (!preview_frame && clock_ms - pic->pts_ms > VIDEO_PRESENT_LATE_MS)
                    stats.late_present++;
                else
                {
                    video_redraw_frame(&pic->frame, scale_buffer, paused,
                                       pic->pts_ms, duration_ms, &osd);
                    if (!preview_frame)
                    {
                        uint32_t complete = video_clock_ms(audio_master, start_tick);
                        video_measure(STAGE_PRESENT,
                                      (complete - pic->pts_ms) * 1000);
                    }
                    displayed_ms = pic->pts_ms;
                    last_sample = pic->sample;
                    struct video_frame old_frame = frame;
                    frame = pic->frame;
                    pic->frame = old_frame;
                    have_frame = true;
                    stats.shown++;
                    if (preview_frame)
                    {
                        preview_frame = false;
                        pause_started = *rb->current_tick;
                        start_tick = pause_started -
                            (long)((uint64_t)displayed_ms * HZ / 1000u);
                        if (paused)
                        {
                            rb->cpu_boost(false);
                            cpu_boosted = false;
                        }
                    }
                }
                pipe.head++;
                continue;
            }
        }
        if (!fill && pipe.finished && !paused && !pipe.priming &&
            clock_ms >= duration_ms)
            break;
        if (!fill && !paused)
            stats.queue_empty++;
        if (!worker && (!paused || pipe.preview) &&
            !pipe.finished && fill < VIDEO_QUEUE_DEPTH)
            video_decode_step(&pipe);
        else
            rb->sleep(1);
    }
    switch (action)
    {
    case VIDEO_INPUT_USB:
        result = VIDEO_USB;
        break;
    case VIDEO_INPUT_EXIT:
        result = VIDEO_EXIT;
        break;
    case VIDEO_INPUT_PREVIOUS:
        result = VIDEO_PREVIOUS;
        break;
    case VIDEO_INPUT_NEXT:
        result = VIDEO_NEXT;
        break;
    default:
        result = VIDEO_FINISHED;
        break;
    }
    if (result == VIDEO_FINISHED && have_frame)
        video_resume_clear(filepath);
    else if (result == VIDEO_EXIT || result == VIDEO_NEXT ||
             result == VIDEO_PREVIOUS)
        video_resume_save(filepath, last_sample, demux.num_samples);

cleanup:
#ifdef VIDEO_ROCK_WORKER
    video_worker_stop(&pipe, &worker);
#endif
#ifdef VIDEO_SYNC_LOG
    if (pipe.error || stats.shown)
        video_sync_log(displayed_ms, video_clock_ms(audio_master, start_tick),
                       displayed_ms, video_queue_fill(&pipe), true);
#endif
    if (have_audio)
        video_audio_stop();
    if (video_fd >= 0)
        rb->close(video_fd);
    if (decoder != NULL)
        rb->hw_h264->close(decoder);
    rb->pcmbuf_fade(false, false);
    rb->backlight_set_timeout(rb->global_settings->backlight_timeout);
    if (!options)
    {
        rb->lcd_set_foreground(LCD_BLACK);
        rb->lcd_clear_display();
        rb->lcd_update();
    }
    if (cpu_boosted)
        rb->cpu_boost(false);
    return result;
}


int video_h264_play(const char *filepath, void *buffer, size_t buffer_size)
{
    return video_h264_play_named(filepath, buffer, buffer_size, NULL);
}
