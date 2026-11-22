/* Foreground AAC decoder lease. GNU GPL version 2 or later. */
#include <string.h>
#include "system.h"
#include "audio.h"
#include "audio/codec_thread.h"
#include "codecs.h"
#include "kernel.h"
#include "pcm_mixer.h"
#include "pcm_sampr.h"
#include "plugins/rock_h264_audio.h"
#include "usbstack/usb_audio.h"

extern struct codec_api ci;

#define VIDEO_CHANNEL PCM_MIXER_CHAN_ROCK_STREAM

static struct mutex lease;
static bool initialized;
static unsigned int owner;
static uintptr_t scratch_start;
static size_t scratch_size;
static unsigned int codec_id;
static void (*producer)(void);
static volatile bool producing;

/* The mixer only reads firmware memory. All queue indexes and clock fields
 * are shared under the PCM lock. */
static _Alignas(4) int16_t output[MIX_FRAME_SAMPLES * 2];
static int16_t *queue;
static unsigned int capacity, front, used;
static uint32_t elapsed_frames, base_ms, rate, saved_rate;

static void next_pcm(const void **start, size_t *size)
{
    /* The mixer asks for a new buffer when the output route changes. */
    if (pcm_current_sink() != PCM_SINK_BUILTIN ||
        mixer_get_frequency() != rate)
    {
        used = 0;
        *start = NULL;
        *size = 0;
        return;
    }

    unsigned int frames = MIN(used, (unsigned int)MIX_FRAME_SAMPLES);
    unsigned int first = MIN(frames, capacity - front);
    memcpy(output, queue + 2 * front, first * 4);
    memcpy(output + 2 * first, queue, (frames - first) * 4);
    memset(output + 2 * frames, 0, (MIX_FRAME_SAMPLES - frames) * 4);
    front = (front + frames) % capacity;
    used -= frames;
    elapsed_frames += frames;
    *start = output;
    *size = sizeof(output);
}

static const struct mixer_play_cbs callbacks = { .get_more = next_pcm };

static void play(void)
{
    mixer_channel_set_amplitude(VIDEO_CHANNEL, MIX_AMP_UNITY);
    mixer_channel_play_data(VIDEO_CHANNEL, &callbacks, NULL, 0);
}

static bool on_foreground(void)
{
    return owner && owner == thread_self();
}

static bool valid_rate(uint32_t value)
{
    for (size_t i = 0; i < HW_NUM_FREQ; ++i)
        if (value == hw_freq_sampr[i])
            return true;
    return false;
}

static bool video_open(void *buffer, size_t bytes, uint32_t frequency)
{
    if (!initialized)
        return false;
    mutex_lock(&lease);
    uintptr_t address = (uintptr_t)buffer;
    bool ok = on_foreground() && !queue && buffer && !(address % 4) &&
              bytes >= sizeof(output) && bytes <= scratch_size &&
              address >= scratch_start &&
              address - scratch_start <= scratch_size - bytes &&
              valid_rate(frequency) && !audio_status() &&
              !usb_audio_get_active() &&
              pcm_current_sink() == PCM_SINK_BUILTIN &&
              usb_audio_reserve_recording();
    if (ok)
    {
        /* Close native playback's cached codec before borrowing the slot. */
        codec_unload();
        saved_rate = mixer_get_frequency();
        mixer_set_frequency(frequency);
        pcm_play_lock();
        queue = buffer;
        capacity = bytes / 4;
        front = used = elapsed_frames = base_ms = 0;
        rate = frequency;
        play();
        pcm_play_unlock();
    }
    mutex_unlock(&lease);
    return ok;
}

static void decode_on_codec_thread(void)
{
    if (!producing)
        return;
#ifdef HAVE_PRIORITY_SCHEDULING
    int priority = thread_set_priority(thread_self(), PRIORITY_PLAYBACK - 4);
#endif
    producer();
#ifdef HAVE_PRIORITY_SCHEDULING
    thread_set_priority(thread_self(), priority);
#endif
}

static bool video_start(void (*decode)(void))
{
    if (!initialized)
        return false;
    mutex_lock(&lease);
    bool ok = on_foreground() && queue && !producer && decode;
    if (ok)
    {
        producer = decode;
        producing = true;
        codec_thread_do_callback(decode_on_codec_thread, &codec_id);
    }
    mutex_unlock(&lease);
    return ok;
}

static void drain(void)
{
    if (!producer)
        return;
    producing = false;
    /* The codec queues this acknowledgement after the producer unwinds. */
    codec_thread_do_callback(NULL, NULL);
    producer = NULL;
    codec_id = 0;
}

static bool video_running_locked(void)
{
    return producing && thread_self() == codec_id &&
           mixer_channel_status(VIDEO_CHANNEL) != CHANNEL_STOPPED;
}

static bool video_running(void)
{
    pcm_play_lock();
    bool running = video_running_locked();
    pcm_play_unlock();
    return running;
}

static int video_decode_aac(struct codec_api *api)
{
    if (!video_running() || !api)
        return CODEC_ERROR;
    const struct codec_api source = *api;
    *api = ci;
    api->dsp = NULL;
    api->id3 = source.id3;
    api->filesize = source.filesize;
    api->curpos = 0;
    api->audio_hid = -1;
    api->codec_get_buffer = source.codec_get_buffer;
    api->pcmbuf_insert = source.pcmbuf_insert;
    api->set_elapsed = source.set_elapsed;
    api->read_filebuf = source.read_filebuf;
    api->request_buffer = source.request_buffer;
    api->advance_buffer = source.advance_buffer;
    api->seek_buffer = source.seek_buffer;
    api->seek_complete = source.seek_complete;
    api->set_offset = source.set_offset;
    api->configure = source.configure;
    api->get_command = source.get_command;
    api->loop_track = source.loop_track;
    api->strip_filesize = source.strip_filesize;
    int status = codec_load_file("aac", api);
    if (status >= 0)
        status = codec_run_proc();
    /* An entry-point failure can leave the codec loaded. */
    codec_close();
    *api = source;
    return status;
}

static size_t video_write(const int16_t *data, size_t frames)
{
    if (!data)
        return 0;
    pcm_play_lock();
    if (!video_running_locked())
    {
        pcm_play_unlock();
        return 0;
    }
    unsigned int tail = (front + used) % capacity;
    frames = MIN(frames, capacity - used);
    unsigned int first = MIN(frames, capacity - tail);
    memcpy(queue + tail * 2, data, first * 4);
    memcpy(queue, data + first * 2, (frames - first) * 4);
    used += frames;
    pcm_play_unlock();
    return frames;
}

static uint32_t video_buffered(void)
{
    pcm_play_lock();
    uint32_t frames = used;
    pcm_play_unlock();
    return frames;
}

static uint32_t video_clock(void)
{
    pcm_play_lock();
    uint32_t ms = base_ms;
    if (rate)
        ms += (uint64_t)elapsed_frames * 1000 / rate;
    pcm_play_unlock();
    return ms;
}

static bool video_pause(bool paused)
{
    mutex_lock(&lease);
    bool ok = on_foreground() && queue;
    if (ok)
        mixer_channel_play_pause(VIDEO_CHANNEL, !paused);
    mutex_unlock(&lease);
    return ok;
}

static bool video_seek(uint32_t ms)
{
    mutex_lock(&lease);
    bool ok = on_foreground() && queue;
    if (ok)
    {
        drain();
        pcm_play_lock();
        mixer_channel_stop(VIDEO_CHANNEL);
        mixer_discard_plugin_prefetch();
        front = used = elapsed_frames = 0;
        base_ms = ms;
        play();
        pcm_play_unlock();
    }
    mutex_unlock(&lease);
    return ok;
}

static void close_locked(bool handover)
{
    if (!queue)
        return;
    drain();
    pcm_play_lock();
    mixer_channel_stop(VIDEO_CHANNEL);
    /* A normal close can play the final mixed frame. Handover drops it. */
    if (handover)
        mixer_discard_plugin_prefetch();
    queue = NULL;
    uint32_t frequency = rate;
    base_ms += (uint64_t)elapsed_frames * 1000 / frequency;
    front = used = elapsed_frames = rate = 0;
    pcm_play_unlock();
    if (pcm_current_sink() == PCM_SINK_BUILTIN &&
        mixer_get_frequency() == frequency)
        mixer_set_frequency(saved_rate);
    usb_audio_release_recording();
}

static void video_close(void)
{
    mutex_lock(&lease);
    if (on_foreground())
        close_locked(false);
    mutex_unlock(&lease);
}

void rock_h264_audio_begin(void *scratch, size_t size)
{
    if (!initialized)
    {
        mutex_init(&lease);
        initialized = true;
    }
    mutex_lock(&lease);
    owner = thread_self();
    scratch_start = (uintptr_t)scratch;
    scratch_size = size;
    mutex_unlock(&lease);
}

void rock_h264_audio_revoke(void)
{
    if (!initialized)
        return;
    mutex_lock(&lease);
    close_locked(true);
    owner = 0;
    mutex_unlock(&lease);
}

const struct rock_h264_audio_api rock_h264_audio_api = {
    .open = video_open,
    .start = video_start,
    .running = video_running,
    .decode_aac = video_decode_aac,
    .write = video_write,
    .buffered = video_buffered,
    .clock_ms = video_clock,
    .pause = video_pause,
    .seek = video_seek,
    .close = video_close,
};
