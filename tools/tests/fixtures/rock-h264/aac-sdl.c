/* Exercise staged AAC on the SDL codec thread and mixer. GPL v2 or later. */
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include "config.h"
/* Import the native lease without adding its target-only SDK slots. */
#define HAVE_HW_H264
#include "plugins/rock_h264_audio.h"
#undef HAVE_HW_H264
#include <plugin.h>
#include "audio/codec_thread.h"
#include "codecs.h"
#include "plugins/rock_files.h"
#include "pcm-internal.h"
#include "pcm_mixer.h"
#include "mp4_demux.h"
#include "video_audio.h"

static int reserved;
static atomic_int capture_next, captured_peak, frames, crossings;
static int previous;

bool usb_audio_reserve_recording(void)
{
    assert(!reserved);
    reserved = 1;
    return true;
}

void usb_audio_release_recording(void)
{
    assert(reserved);
    reserved = 0;
}

bool usb_audio_get_active(void)
{
    return false;
}

static const struct plugin_api api = {
    .h264_audio = &rock_h264_audio_api,
    .memset = memset, .memcpy = memcpy,
    .open = rock_open, .close = rock_close, .read = rock_read,
    .lseek = rock_lseek, .ffilesize = rock_ffilesize,
    .sleep = sleep, .yield = yield,
};
const struct plugin_api *rb = &api;

static bool capture_on_stop;
void __real_mixer_channel_stop(enum pcm_mixer_channel channel);
void __wrap_mixer_channel_stop(enum pcm_mixer_channel channel)
{
    if (capture_on_stop && channel == PCM_MIXER_CHAN_ROCK_STREAM)
    {
        capture_on_stop = false;
        capture_next = 1;
    }
    __real_mixer_channel_stop(channel);
}

bool __real_pcm_play_dma_complete_callback(enum pcm_dma_status status,
                                           const void **addr, size_t *size);
bool __wrap_pcm_play_dma_complete_callback(enum pcm_dma_status status,
                                           const void **addr, size_t *size)
{
    if (!__real_pcm_play_dma_complete_callback(status, addr, size))
        return false;
    const int16_t *pcm = *addr;
    int peak = 0;
    int count = 0;
    for (size_t i = 0; i < *size / 2; i += 2)
    {
        int sample = pcm[i];
        if (sample < 0 && previous >= 0)
            ++count;
        previous = sample;
        if (abs(sample) > peak)
            peak = abs(sample);
    }
    if (atomic_exchange(&capture_next, 0))
        captured_peak = peak;
    if (peak > 64)
    {
        crossings += count;
        frames += *size / 4;
    }
    return true;
}

static void wait_frames(int minimum)
{
    for (int i = 0; i < 5 * HZ && frames < minimum; ++i)
    {
        if (video_audio_failed() || !video_audio_is_active())
            break;
        sleep(1);
    }
    fprintf(stderr, "frames=%d crossings=%d clock=%u active=%d failed=%d\n",
            frames, crossings, rb->h264_audio->clock_ms(),
            video_audio_is_active(), video_audio_failed());
    assert(frames >= minimum && !video_audio_failed());
}

static _Alignas(32) unsigned char workspace[2 * CODEC_SIZE];
static _Alignas(4) int16_t pcm_queue[4096 * 2];
static long handover_stack[DEFAULT_STACK_SIZE / sizeof(long)];

void __wrap_root_menu(void)
{
    const char *path = "/probe.mp4";
    static struct mp4v_demux_res demux;
    int fd = rock_open(path, O_RDONLY);
    if (fd < 0)
    {
        fputs("MP4 open failed\n", stderr);
        exit(1);
    }
    int result = mp4v_demux_read(fd, &demux);
    rock_close(fd);
    if (result < 0)
    {
        fputs("MP4 demux failed\n", stderr);
        exit(1);
    }
    fprintf(stderr, "demux rate=%u samples=%u audio chunks=%u trim=%u\n",
            demux.audio_sample_rate, demux.audio_num_samples,
            demux.audio_num_stco, demux.audio_lead_trim);

    unsigned saved_rate = mixer_get_frequency();
    assert(codec_load(-1, AFMT_MP4_AAC));
    rock_h264_audio_begin(pcm_queue, sizeof(pcm_queue));
    assert(video_audio_init(path, &demux, workspace, sizeof(workspace),
                            pcm_queue, sizeof(pcm_queue)) == 0);
    assert(codec_loaded() == AFMT_UNKNOWN);
    video_audio_play();
    wait_frames(44100 / 2);
    assert(crossings * 44100 / frames > 410 &&
           crossings * 44100 / frames < 470);

    /* Capture the next PCM buffer that the SDL sink receives after seek. */
    captured_peak = -1;
    capture_on_stop = true;
    video_audio_seek(3000);
    fprintf(stderr, "seek clock=%u pending=%u\n",
            rb->h264_audio->clock_ms(), rb->h264_audio->buffered());
    sleep(HZ / 4);
    fprintf(stderr, "first post-seek buffer peak=%d\n", captured_peak);
    int seek_peak = captured_peak;

    pcm_play_lock();
    frames = crossings = 0;
    pcm_play_unlock();
    wait_frames(44100 / 2);
    assert(crossings * 44100 / frames > 825 &&
           crossings * 44100 / frames < 930);

    video_audio_pause();
    unsigned clock = rb->h264_audio->clock_ms();
    sleep(HZ / 5);
    assert(rb->h264_audio->clock_ms() == clock && video_audio_is_active());
    video_audio_resume();
    int before_resume = frames;
    wait_frames(before_resume + 44100 / 4);
    captured_peak = -1;
    capture_on_stop = true;
    unsigned int handover = create_thread(
        rock_h264_audio_revoke, handover_stack, sizeof(handover_stack), 0,
        "AAC handover" IF_PRIO(, PRIORITY_USER_INTERFACE)
        IF_COP(, CPU));
    assert(handover);
    thread_wait(handover);
    assert(!video_audio_is_active() && !reserved);
    sleep(HZ / 4);
    fprintf(stderr, "first post-revoke buffer peak=%d\n", captured_peak);
    assert(captured_peak == 0);
    assert(seek_peak == 0);
    assert(mixer_get_frequency() == saved_rate);
    video_audio_stop();
    assert(codec_load(-1, AFMT_MP4_AAC));
    codec_unload();

    rock_h264_audio_begin(pcm_queue, sizeof(pcm_queue));
    assert(video_audio_init(path, &demux, workspace, sizeof(workspace),
                            pcm_queue, sizeof(pcm_queue)) == 0);
    video_audio_play();
    wait_frames(frames + 44100 / 4);
    mixer_set_frequency(48000);
    sleep(HZ / 4);
    fprintf(stderr, "after rate change: active=%d buffered=%u\n",
            video_audio_is_active(), rb->h264_audio->buffered());
    assert(!video_audio_is_active() && rb->h264_audio->buffered() == 0);
    video_audio_stop();
    assert(!reserved && mixer_get_frequency() == 48000);
    fputs("PASS: staged AAC decode, seek, pause, revoke, codec reuse"
          " and rate change\n", stderr);
    exit(0);
}
