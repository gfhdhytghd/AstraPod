#!/usr/bin/env python3
"""Compile the actual native capture code with deterministic hardware doubles."""
from pathlib import Path
import re
import subprocess
import sys

root = Path(__file__).resolve().parents[2]
out = Path(sys.argv[1]).resolve()
out.mkdir(parents=True, exist_ok=True)
code = (root / "apps-ipod/plugins/rock_pitch.c").read_text()
header = (root / "apps-ipod/plugins/rock_pitch.h").read_text()
target_audio = (root / "firmware/target/arm/s5l8702/ipod6g/audio-6g.c").read_text()
usb = (root / "firmware/usbstack/usb_audio.c").read_text()

def function(text, signature):
    at = text.index(signature + "\n{")
    return text[at:text.index("\n}\n", at) + 3]

def remove_includes(text):
    return re.sub(r'^#include "[^"]+"\n', '', text, flags=re.M)

prefix = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define ASTRAPOD_ROCK_COMPAT
#define IPOD_6G
#define CONFIG_PLATFORM 1
#define PLATFORM_NATIVE 1
#define HAVE_RECORDING
#define HAVE_MIC_REC
#define HAVE_LINE_REC
#define INPUT_SRC_CAPS 3
#define AUDIO_NUM_SOURCES 3
#define USB_ENABLE_AUDIO
#define HZ 100
#define CACHEALIGN_ATTR __attribute__((aligned(32)))
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define TIME_AFTER(a,b) ((a) > (b))
#define SAMPR_TYPE_REC (1U << 24)
enum { PCM_SINK_BUILTIN, PCM_SINK_USB, Q_AUDIO_PREPARE_PITCH,
       AUDIO_SRC_PLAYBACK, AUDIO_SRC_LINEIN, AUDIO_SRC_MIC, SRCF_PLAYBACK,
       SRCF_RECORDING, SOUND_MIC_GAIN, SOUND_LEFT_GAIN, SOUND_RIGHT_GAIN,
       AUDIO_GAIN_MIC, AUDIO_GAIN_LINEIN, PCM_MIXER_CHAN_BEEP };
enum pcm_dma_status { PCM_DMAST_ERR_DMA = -1, PCM_DMAST_OK = 0 };
struct mutex { bool locked; };
static struct {
    int rec_source, rec_mic_gain, rec_left_gain, rec_right_gain;
} global_settings = { AUDIO_SRC_LINEIN, 19, -400, 400 };
static bool usb_audio_playing, recording_reserved, usb_active, mic_supported = true;
static bool jack_present;
static int usb_interface = 2, usb_as_playback_intf_alt;
#define usb_log(...) ((void)0)
#define usb_log_sync() ((void)0)
#define logf(...) ((void)0)
static void usb_audio_start_playback(void)
{
    assert(!recording_reserved && usb_audio_playing);
    usb_as_playback_intf_alt = 1;
}
static void usb_audio_stop_playback(void)
{
    usb_as_playback_intf_alt = 0;
    usb_audio_playing = false;
}
static unsigned int queued_request, native_rate = 48000, rate = 48000;
static int sink = PCM_SINK_BUILTIN, speech_depth, releases, dma_stops, config_checks;
static bool rec_on, dma_locked, deferred_takeover, fail_start, monitor, tones_stopped;
int rec_hw_ver = 1;
static int route = AUDIO_SRC_PLAYBACK, codec_voltage;
static bool mic_bias, adc_enabled, recording_rate_applied;
static volatile unsigned int GPIOCMD;
static void mikey_set_mic_capture(bool enable) { mic_bias = enable; }
static void pmu_ldo_set_voltage(int ldo, int voltage)
{
    assert(ldo == 3);
    codec_voltage = voltage;
}
static long current_tick;
static void (*more)(void **, size_t *);
static enum pcm_dma_status (*status_cb)(enum pcm_dma_status);
static void *dma_buffer;
static size_t dma_size;
static int disable_irq_save(void) { return 0; }
static void restore_irq(int irq) { assert(irq == 0); }
static void mutex_init(struct mutex *m) { m->locked = false; }
static void mutex_lock(struct mutex *m) { assert(!m->locked); m->locked = true; }
static void mutex_unlock(struct mutex *m) { assert(m->locked); m->locked = false; }
static bool usb_audio_get_active(void) { return usb_active; }
static bool mikey_supported(void) { return mic_supported; }
static bool headphones_inserted(void) { return jack_present; }
static int pcm_current_sink(void) { return sink; }
static void audio_queue_post(int event, unsigned int req)
{
    assert(event == Q_AUDIO_PREPARE_PITCH && recording_reserved);
    queued_request = req;
}
static void talk_disable(bool suppress)
{
    speech_depth += suppress ? 1 : -1;
    assert(speech_depth == (int)suppress);
}
static void playback_prepare_recording(void);

static void mixer_channel_stop(int channel)
{
    assert(channel == PCM_MIXER_CHAN_BEEP && speech_depth == 1 && !rec_on);
    tones_stopped = true;
}
static unsigned int mixer_get_frequency(void) { return rate; }
static void mixer_set_frequency(unsigned int value)
{
    assert(sink == PCM_SINK_BUILTIN && !rec_on && recording_reserved);
    assert(speech_depth == 1);
    assert(value == native_rate && route == AUDIO_SRC_PLAYBACK && !monitor);
    rate = value;
}
static void pcm_set_frequency(unsigned int value)
{
    assert(adc_enabled && monitor && value == (44100 | SAMPR_TYPE_REC));
    rate = value & ~SAMPR_TYPE_REC;
}
static void pcm_apply_settings(void)
{
    assert(adc_enabled && rate == 44100 && recording_reserved);
    recording_rate_applied = true;
}
static void pcm_init_recording(void)
{
    assert(!rec_on && recording_reserved && speech_depth == 1 && releases > 0);
}
static void pcm_close_recording(void)
{
    assert(recording_reserved && speech_depth == 1 && route != AUDIO_SRC_PLAYBACK);
    ++dma_stops;
    tones_stopped = false;
    rec_on = false;
    recording_rate_applied = false;
    more = NULL;
    status_cb = NULL;
    dma_buffer = NULL;
}
static void audiohw_set_monitor(bool on)
{
    assert(!rec_on && recording_reserved);
    monitor = on;
}
static void audiohw_enable_recording(bool mic)
{
    assert(!rec_on && !adc_enabled && monitor && codec_voltage == 0xf);
    assert(mic_bias == mic && GPIOCMD == (mic ? 0xe070f : 0xe070e));
    route = mic ? AUDIO_SRC_MIC : AUDIO_SRC_LINEIN;
    adc_enabled = true;
}
static void audiohw_disable_recording(void)
{
    assert(!rec_on && !monitor);
    route = AUDIO_SRC_PLAYBACK;
    adc_enabled = false;
}
static int sound_min(int field) { return field == SOUND_MIC_GAIN ? -8 : -12; }
static int sound_max(int field) { return field == SOUND_MIC_GAIN ? 18 : 20; }
static void audio_set_recording_gain(int left, int right, int type)
{
    assert(adc_enabled && monitor && route == (mic_supported && jack_present ? AUDIO_SRC_MIC : AUDIO_SRC_LINEIN));
    if (route == AUDIO_SRC_MIC)
        assert(type == AUDIO_GAIN_MIC && left == 18 && right == 0);
    else
        assert(type == AUDIO_GAIN_LINEIN && left == -12 && right == 20);
    ++config_checks;
}
static void pcm_record_data(void (*callback)(void **, size_t *),
                            enum pcm_dma_status (*status)(enum pcm_dma_status),
                            void *buffer, size_t size)
{
    assert((uintptr_t)buffer % 32 == 0 && size == 16384);
    assert(rate == 44100 && recording_rate_applied && config_checks > 0 &&
           recording_reserved && adc_enabled && monitor && tones_stopped);
    more = callback;
    status_cb = status;
    dma_buffer = buffer;
    dma_size = size;
    rec_on = !fail_start;
}
static bool pcm_is_recording(void) { return rec_on; }
static void pcm_rec_lock(void) { assert(!dma_locked); dma_locked = true; }
static void pcm_rec_unlock(void) { assert(dma_locked); dma_locked = false; }
"""
suffix = r"""
static void playback_prepare_recording(void)
{
    assert(!pitch_mutex.locked && recording_reserved);
    ++releases;
    if (deferred_takeover)
        rock_pitch_revoke();
}

static void complete_block(int16_t signal)
{
    assert(rec_on && !dma_locked && more);
    int16_t *samples = dma_buffer;
    for (int i = 0; i < ROCK_PITCH_SAMPLES; i += 2)
    {
        samples[2 * i] = signal + i - 2;
        samples[2 * i + 1] = signal + i - 1;
        samples[2 * i + 2] = signal + i + 2;
        samples[2 * i + 3] = signal + i + 3;
    }
    more(&dma_buffer, &dma_size);
}

static void restored(void)
{
    assert(!reserved && !rec_on && !more && !status_cb && !dma_buffer);
    assert(!recording_reserved && !speech_depth && !monitor && !adc_enabled && !mic_bias);
    assert(GPIOCMD == 0xe070e && codec_voltage == 0x9);
    assert(rate == native_rate && route == AUDIO_SRC_PLAYBACK);
    assert(usb_audio_reserve_recording());
    usb_audio_release_recording();
}

int main(void)
{
    int16_t samples[ROCK_PITCH_SAMPLES];
    assert(!rock_pitch_suppresses_tones());
    rock_pitch_init();
    assert(!rock_pitch_suppresses_tones());
    rock_pitch_session_open();
    assert(usb_audio_reserve_recording()); /* Native recorder owns the claim. */
    assert(!rock_pitch_start() && releases == 0);
    usb_audio_release_recording();
    usb_audio_playing = true;
    assert(!rock_pitch_start());
    usb_audio_playing = false;
    usb_active = true;
    assert(!rock_pitch_start());
    usb_active = false;
    sink = PCM_SINK_USB;
    assert(!rock_pitch_start());
    sink = PCM_SINK_BUILTIN;

    assert(rock_pitch_start() && !usb_audio_reserve_recording());
    assert(usb_audio_set_interface(usb_interface + 1, 1) == -1);
    assert(rock_pitch_read(&samples) == ROCK_PITCH_WAITING);
    rock_pitch_prepare(queued_request);
    assert(rock_pitch_read(&samples) == ROCK_PITCH_WAITING);
    complete_block(500);
    complete_block(900); /* Slow reader keeps newest complete block. */
    assert(rock_pitch_read(&samples) == ROCK_PITCH_READY);
    for (int i = 0; i < ROCK_PITCH_SAMPLES; ++i)
        assert(samples[i] == 900 + i);
    complete_block(-1000);
    assert(samples[0] == 900);
    assert(rock_pitch_read(&samples) == ROCK_PITCH_READY && samples[0] == -1000);
    assert(rock_pitch_suppresses_tones());
    rock_pitch_pause(); /* Settings menu. */
    restored();
    assert(!rock_pitch_suppresses_tones());
    assert(usb_audio_set_interface(usb_interface + 1, 1) == 0);
    assert(!rock_pitch_start());
    assert(usb_audio_set_interface(usb_interface + 1, 0) == 0);
    rock_pitch_pause(); /* Repeated cleanup. */
    assert(dma_stops == 1);

    /* The native recorder defaults to line-in. Tuner uses the plugged MIC. */
    assert(global_settings.rec_source == AUDIO_SRC_LINEIN);
    jack_present = true;
    assert(rock_pitch_start());
    rock_pitch_prepare(queued_request);
    assert(route == AUDIO_SRC_MIC && mic_bias);
    complete_block(4512);
    assert(rock_pitch_read(&samples) == ROCK_PITCH_READY);
    assert(samples[0] == 4512);
    jack_present = false;
    assert(rock_pitch_read(&samples) == ROCK_PITCH_NO_INPUT);
    restored();

    jack_present = true;
    mic_supported = false;
    assert(rock_pitch_start());
    rock_pitch_prepare(queued_request);
    assert(route == AUDIO_SRC_LINEIN);
    rock_pitch_revoke(); /* USB consent, shutdown, or playback handoff. */
    restored();
    assert(!rock_pitch_start() && rock_pitch_read(&samples) == ROCK_PITCH_STOPPED);

    rock_pitch_session_open();
    assert(rock_pitch_start());
    unsigned int stale = queued_request;
    rock_pitch_pause();
    assert(rock_pitch_start());
    rock_pitch_prepare(stale);
    assert(releases == 3 && !rec_on);
    deferred_takeover = true;
    rock_pitch_prepare(queued_request);
    deferred_takeover = false;
    restored();
    assert(rock_pitch_read(&samples) == ROCK_PITCH_STOPPED);

    rock_pitch_session_open();
    assert(rock_pitch_start());
    current_tick += 2*HZ + 1;
    assert(rock_pitch_read(&samples) == ROCK_PITCH_FAILED);
    rock_pitch_prepare(queued_request);
    restored();

    assert(rock_pitch_start());
    rock_pitch_prepare(queued_request);
    current_tick += 2*HZ + 1;
    assert(rock_pitch_read(&samples) == ROCK_PITCH_FAILED);
    restored();

    assert(rock_pitch_start());
    fail_start = true;
    rock_pitch_prepare(queued_request);
    assert(rock_pitch_read(&samples) == ROCK_PITCH_FAILED);
    fail_start = false;
    restored();

    assert(rock_pitch_start());
    rock_pitch_prepare(queued_request);
    assert(status_cb(PCM_DMAST_ERR_DMA) == PCM_DMAST_ERR_DMA);
    assert(rock_pitch_sink_lock());
    restored();
    sink = PCM_SINK_USB;
    rock_pitch_sink_unlock();
    assert(!rock_pitch_start() && rock_pitch_read(&samples) == ROCK_PITCH_STOPPED);
    sink = PCM_SINK_BUILTIN;
    global_settings.rec_source = AUDIO_SRC_LINEIN;
    rock_pitch_session_open();
    assert(rock_pitch_start());
    usb_active = true;
    rock_pitch_prepare(queued_request);
    assert(rock_pitch_read(&samples) == ROCK_PITCH_FAILED);
    restored();
    assert(rock_pitch_read(&samples) == ROCK_PITCH_STOPPED);

    usb_active = false;
    assert(rock_pitch_start());
    current_tick += 2*HZ + 1;
    rock_pitch_prepare(queued_request);
    assert(rock_pitch_read(&samples) == ROCK_PITCH_FAILED);
    restored();

    assert(rock_pitch_start());
    rock_pitch_prepare(queued_request);
    complete_block(100);
    assert(rock_pitch_read(NULL) == ROCK_PITCH_FAILED);
    assert(rock_pitch_read(&samples) == ROCK_PITCH_READY && samples[0] == 100);
    rock_pitch_pause();
    restored();

    printf("PASS: native/USB exclusion, pending invalidation, DMA copy, gain, timeout, egress (%d DMA closes)\n",
           dma_stops);
}
"""

assembly = (prefix + remove_includes(header)
            + function(usb, "bool usb_audio_reserve_recording(void)")
            + function(usb, "void usb_audio_release_recording(void)")
            + function(usb, "static int usb_audio_set_interface(int intf, int alt)")
            + remove_includes(target_audio) + remove_includes(code) + suffix)
source = out / "capture-probe.c"
source.write_text(assembly)
probe = out / "capture-probe"
subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=undefined", source, "-o", probe], check=True)
subprocess.run([probe], check=True)
