#!/usr/bin/env python3
"""Probe the firmware's AAC/PCM lease with codec-thread and mixer doubles."""
from pathlib import Path
import re
import subprocess
import sys

root = Path(__file__).resolve().parents[2]
out = Path(sys.argv[1]).resolve()
out.mkdir(parents=True, exist_ok=True)

prefix = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#define HAVE_HW_H264
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MIX_FRAME_SAMPLES 4
#define MIX_AMP_UNITY 65536
#define PCM_MIXER_CHAN_ROCK_STREAM 2
#define HW_NUM_FREQ 2
#define PCM_SINK_BUILTIN 1
#define CODEC_ERROR -1
static const unsigned long hw_freq_sampr[HW_NUM_FREQ] = {44100, 48000};
struct codec_api {
    int data, audio_hid;
    size_t filesize, curpos;
    void *dsp, *id3;
    void (*codec_get_buffer)(void), (*pcmbuf_insert)(void), (*set_elapsed)(void);
    void (*read_filebuf)(void), (*request_buffer)(void), (*advance_buffer)(void);
    void (*seek_buffer)(void), (*seek_complete)(void), (*set_offset)(void);
    void (*configure)(void), (*get_command)(void), (*loop_track)(void);
    void (*strip_filesize)(void);
};
struct codec_api ci;
struct mutex { bool held; };
struct mixer_play_cbs { void (*get_more)(const void **, size_t *); };
static unsigned id = 1, mixer_rate = 48000, audio_state, unloads, barriers;
static bool codec_loaded, reserved, usb_active, paused, pcm_locked;
static const struct mixer_play_cbs *mixer;
static void (*callback)(void);
static void mutex_init(struct mutex *m) { m->held = false; }
static void mutex_lock(struct mutex *m) { assert(!m->held); m->held = true; }
static void mutex_unlock(struct mutex *m) { assert(m->held); m->held = false; }
static unsigned thread_self(void) { return id; }
static unsigned audio_status(void) { return audio_state; }
static bool usb_audio_get_active(void) { return usb_active; }
static int pcm_current_sink(void) { return PCM_SINK_BUILTIN; }
static bool usb_audio_reserve_recording(void)
{
    if (reserved)
        return false;
    reserved = true;
    return true;
}
static void usb_audio_release_recording(void)
{
    assert(reserved && !mixer && !codec_loaded);
    reserved = false;
}
static void codec_unload(void) { assert(reserved); ++unloads; }
static unsigned mixer_get_frequency(void) { return mixer_rate; }
static void mixer_set_frequency(unsigned rate)
{
    assert(reserved && !mixer);
    mixer_rate = rate;
}
static void pcm_play_lock(void) { assert(!pcm_locked); pcm_locked = true; }
static void pcm_play_unlock(void) { assert(pcm_locked); pcm_locked = false; }
static void mixer_channel_stop(int channel)
{
    assert(channel == PCM_MIXER_CHAN_ROCK_STREAM && pcm_locked);
    mixer = NULL;
    paused = false;
}
static void mixer_channel_play_data(int channel, const struct mixer_play_cbs *cbs,
                                    const void *data, size_t bytes)
{
    assert(channel == PCM_MIXER_CHAN_ROCK_STREAM && !data && !bytes && pcm_locked);
    mixer = cbs;
}
static void mixer_channel_set_amplitude(int channel, int amplitude)
{
    assert(channel == PCM_MIXER_CHAN_ROCK_STREAM && amplitude == MIX_AMP_UNITY);
}
static void mixer_channel_play_pause(int channel, bool play)
{
    assert(mixer && channel == PCM_MIXER_CHAN_ROCK_STREAM);
    paused = !play;
}
static void step_codec(void)
{
    unsigned saved = id;
    id = 2;
    void (*run)(void) = callback;
    callback = NULL;
    run();
    id = saved;
}
static void codec_thread_do_callback(void (*fn)(void), unsigned *codec)
{
    if (fn)
    {
        assert(!callback && codec);
        *codec = 2;
        callback = fn;
    }
    else
    {
        ++barriers;
        if (callback)
            step_codec(); /* Revocation can beat entry into the callback. */
        assert(!codec_loaded);
    }
}
static int codec_load_file(const char *name, struct codec_api *api)
{
    assert(!strcmp(name, "aac") && api->data == 42 && id == 2 && !codec_loaded);
    codec_loaded = true;
    return 0;
}
static int codec_run_proc(void) { assert(codec_loaded && id == 2); return 0; }
static void codec_close(void) { assert(codec_loaded && id == 2); codec_loaded = false; }
"""

suffix = r"""
static unsigned decoded;
static void produce(void)
{
    ++decoded;
    assert(rock_h264_audio_api.running());
    struct codec_api api = { .id3 = &api, .filesize = 1234 };
    ci.data = 42;
    assert(rock_h264_audio_api.decode_aac(&api) == 0);
    assert(api.data == 0 && api.id3 == &api && api.filesize == 1234);
    int16_t input[40];
    for (unsigned i = 0; i < 40; ++i)
        input[i] = i + 1;
    assert(rock_h264_audio_api.write(input, 20) == 10);
}

static void consume(int first)
{
    const void *data;
    size_t bytes;
    assert(mixer && !paused);
    mixer->get_more(&data, &bytes);
    assert(bytes == 4 * MIX_FRAME_SAMPLES);
    assert(((const int16_t *)data)[0] == first);
}

int main(void)
{
    const struct rock_h264_audio_api *api = &rock_h264_audio_api;
    int32_t scratch[10];
    rock_h264_audio_revoke();
    rock_h264_audio_begin(scratch, sizeof(scratch));
    assert(!api->open(scratch + 1, sizeof(scratch), 44100));
    assert(!api->open(scratch, sizeof(scratch), 22050));
    audio_state = 1;
    assert(!api->open(scratch, sizeof(scratch), 44100));
    audio_state = 0;
    usb_active = true;
    assert(!api->open(scratch, sizeof(scratch), 44100));
    usb_active = false;
    reserved = true;
    assert(!api->open(scratch, sizeof(scratch), 44100));
    reserved = false;
    assert(api->open(scratch, sizeof(scratch), 44100));
    assert(unloads == 1 && mixer_rate == 44100 && reserved);
    consume(0);
    assert(api->start(produce) && !api->start(produce));
    step_codec();
    assert(decoded == 1 && api->buffered() == 10);
    consume(1);
    consume(9);
    assert(api->buffered() == 2);
    assert(api->pause(true) && paused);
    assert(api->seek(12150));
    assert(api->clock_ms() == 12150 && api->buffered() == 0);
    consume(0);
    assert(api->start(produce));
    /* Native playback or USB revokes before callback dispatch. */
    id = 3;
    rock_h264_audio_revoke();
    assert(decoded == 1 && barriers == 2 && !reserved && !mixer &&
           mixer_rate == 48000);
    assert(!api->running() && !api->start(produce));
    id = 1;
    assert(!api->open(scratch, sizeof(scratch), 44100));
    rock_h264_audio_begin(scratch, sizeof(scratch));
    assert(api->open(scratch, sizeof(scratch), 44100));
    assert(api->start(produce));
    step_codec();
    api->close();
    assert(decoded == 2 && barriers == 3 && !reserved && !mixer);
    puts("PASS: AAC lease, worker and PCM drain, seek clock, USB exclusion and handback");
}
"""

def source(name):
    code = (root / f"apps-ipod/plugins/rock_h264_audio.{name}").read_text()
    return re.sub(r"^#include [^\n]*\n", "", code, flags=re.M)

test = out / "audio-lease.c"
test.write_text(prefix + source("h") + source("c") + suffix)
executable = out / "audio-lease"
subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=undefined", test, "-o", executable], check=True)
subprocess.run([executable], check=True)
