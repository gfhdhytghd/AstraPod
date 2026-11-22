/* Foreground PCM stream ownership. GNU GPL version 2 or later. */
#include <stdint.h>
#include <string.h>
#include "audio.h"
#include "kernel.h"
#include "pcm_mixer.h"
#include "plugins/rock_compat.h"
#include "plugins/rock_stream.h"
#ifdef USB_ENABLE_AUDIO
#include "usbstack/usb_audio.h"
#endif

#define STREAM_CHANNEL PCM_MIXER_CHAN_ROCK_STREAM

/* The mixer and foreground thread share state under the PCM playback lock.
 * The callback never holds a module pointer or invokes module code. */
static uint32_t samples[ROCK_STREAM_QUEUE_SIZE][ROCK_STREAM_BLOCK_BYTES / 4];
static size_t lengths[ROCK_STREAM_QUEUE_SIZE];
static unsigned int head, count, rate;
static volatile unsigned int owner;
static bool permitted, playing, paused;

static void clear_queue(void)
{
    head = 0;
    count = 0;
    playing = false;
}

static void discard(void)
{
    mixer_channel_stop(STREAM_CHANNEL);
    clear_queue();
    rate = 0;
    paused = false;
}

static void refill(const void **start, size_t *size)
{
    *start = NULL;
    *size = 0;
    if (playing && (rate != mixer_get_frequency() ||
                    pcm_current_sink() != PCM_SINK_BUILTIN))
    {
        permitted = false;
        rate = 0;
        clear_queue();
    }
    if (!playing)
        return;
    head = (head + 1) % ROCK_STREAM_QUEUE_SIZE;
    playing = --count != 0;
    if (playing)
    {
        *start = samples[head];
        *size = lengths[head];
    }
}

static const struct mixer_play_cbs callbacks = { .get_more = refill };

void rock_stream_session_open(void)
{
    pcm_play_lock();
    owner = thread_self();
    permitted = true;
    pcm_play_unlock();
}

void rock_stream_revoke(void)
{
    if (!owner)
        return;
    pcm_play_lock();
    permitted = false;
    owner = 0;
    if (rate)
        discard();
    pcm_play_unlock();
}

static bool available(void)
{
    if (!permitted || !owner || thread_self() != owner || rock_usb_taken() ||
        pcm_current_sink() != PCM_SINK_BUILTIN ||
        (audio_status() & (AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE |
                           AUDIO_STATUS_RECORD | AUDIO_STATUS_PRERECORD)))
        return false;
#ifdef USB_ENABLE_AUDIO
    if (usb_audio_get_active())
        return false;
#endif
    return true;
}

unsigned int rock_stream_open(void)
{
    pcm_play_lock();
    unsigned int result = 0;
    if (!rate && available())
    {
        unsigned int current = mixer_get_frequency();
        if (current >= 8000 && current <= 48000)
            result = rate = current;
    }
    pcm_play_unlock();
    return result;
}

static bool usable(void)
{
    if (!rate || thread_self() != owner)
        return false;
    if (!available())
    {
        permitted = false;
        discard();
        return false;
    }
    if (rate == mixer_get_frequency() &&
        (!playing || mixer_channel_status(STREAM_CHANNEL) != CHANNEL_STOPPED))
        return true;
    permitted = false;
    discard();
    return false;
}

int rock_stream_pending(void)
{
    pcm_play_lock();
    int result = usable() ? (int)count : -1;
    pcm_play_unlock();
    return result;
}

static void start_queue(void)
{
    playing = true;
    mixer_channel_set_amplitude(STREAM_CHANNEL, MIX_AMP_UNITY);
    mixer_channel_play_data(STREAM_CHANNEL, &callbacks,
                            samples[head], lengths[head]);
}

bool rock_stream_submit(const void *data, size_t size)
{
    if (!data || !size || size > ROCK_STREAM_BLOCK_BYTES || size % 4)
        return false;

    pcm_play_lock();
    bool accepted = usable() && count < ROCK_STREAM_QUEUE_SIZE;
    if (accepted)
    {
        unsigned int tail = (head + count) % ROCK_STREAM_QUEUE_SIZE;
        memcpy(samples[tail], data, size);
        lengths[tail] = size;
        count++;
        if (!playing && !paused)
            start_queue();
    }
    pcm_play_unlock();
    return accepted;
}

bool rock_stream_pause(bool stop)
{
    pcm_play_lock();
    bool valid = usable();
    if (valid && stop != paused)
    {
        paused = stop;
        if (playing)
            mixer_channel_play_pause(STREAM_CHANNEL, !paused);
        else if (!paused && count)
            start_queue();
    }
    pcm_play_unlock();
    return valid;
}

void rock_stream_close(void)
{
    pcm_play_lock();
    if (owner && thread_self() == owner && rate)
        discard();
    pcm_play_unlock();
}
