/* Foreground tuner capture. GNU GPL version 2 or later. */
#include "plugins/rock_pitch.h"
#include "kernel.h"

#ifdef HAVE_ROCK_PITCH_DMA
#include <string.h>
#include "system.h"
#include "audio.h"
#include "audio/audio_thread.h"
#include "button.h"
#include "pcm.h"
#include "pcm_mixer.h"
#include "pcm_sampr.h"
#include "settings/settings.h"
#include "sound.h"
#include "speech/talk.h"
#include "usbstack/usb_audio.h"
#endif

static struct mutex pitch_mutex;
static bool initialized;
static bool allowed;
static bool running;

#ifdef HAVE_ROCK_PITCH_DMA
static bool reserved;
static bool speech_disabled;
static unsigned int request;
static unsigned int saved_rate;
static long deadline;

/* The completion callback publishes the finished block and selects the other
 * block for DMA. Only the reader copies data into module memory. */
static int16_t capture[2][ROCK_PITCH_SAMPLES] CACHEALIGN_ATTR;
static int writing;
static volatile int ready;

static void pitch_next_block(void **addr, size_t *size)
{
    ready = writing;
    writing ^= 1;
    *addr = capture[writing];
    *size = sizeof(capture[0]);
}

static enum pcm_dma_status pitch_dma_status(enum pcm_dma_status status)
{
    return status;
}

static int pitch_gain(int setting, int gain)
{
    return MAX(sound_min(setting), MIN(sound_max(setting), gain));
}
#endif

static void pitch_stop_locked(void)
{
#ifdef HAVE_ROCK_PITCH_DMA
    if (running)
    {
        /* Stops RX and clears the firmware callbacks under the DMA lock. */
        pcm_close_recording();
        audio_input_mux(AUDIO_SRC_PLAYBACK, SRCF_PLAYBACK);
        audio_set_output_source(AUDIO_SRC_PLAYBACK);
        mixer_set_frequency(saved_rate);
    }
    if (speech_disabled)
    {
        talk_disable(false);
        speech_disabled = false;
    }
    if (reserved)
    {
        usb_audio_release_recording();
        reserved = false;
    }
#endif
    running = false;
}

void rock_pitch_init(void)
{
    mutex_init(&pitch_mutex);
    initialized = true;
}

void rock_pitch_session_open(void)
{
    mutex_lock(&pitch_mutex);
    allowed = true;
    mutex_unlock(&pitch_mutex);
}

void rock_pitch_revoke(void)
{
    if (!initialized)
        return;

    mutex_lock(&pitch_mutex);
    allowed = false;
    pitch_stop_locked();
    mutex_unlock(&pitch_mutex);
}

void rock_pitch_pause(void)
{
    mutex_lock(&pitch_mutex);
    pitch_stop_locked();
    mutex_unlock(&pitch_mutex);
}

bool rock_pitch_start(void)
{
    mutex_lock(&pitch_mutex);
    bool start = allowed && !running;
#ifdef HAVE_ROCK_PITCH_DMA
    start = start && !reserved && !usb_audio_get_active() &&
        pcm_current_sink() == PCM_SINK_BUILTIN &&
        (global_settings.rec_source == AUDIO_SRC_LINEIN ||
         (global_settings.rec_source == AUDIO_SRC_MIC && mikey_supported())) &&
        usb_audio_reserve_recording();
    if (start)
    {
        reserved = true;
        deadline = current_tick + 2 * HZ;
        /* The audio thread must release playback before changing the route.
         * A queued USB takeover can discard this request without blocking UI. */
        audio_queue_post(Q_AUDIO_PREPARE_PITCH, ++request);
    }
#else
    if (start)
        running = true;
#endif
    mutex_unlock(&pitch_mutex);
    return start;
}

#ifdef HAVE_ROCK_PITCH_DMA
bool rock_pitch_suppresses_tones(void)
{
    if (!initialized)
        return false;

    mutex_lock(&pitch_mutex);
    bool quiet = speech_disabled;
    mutex_unlock(&pitch_mutex);
    return quiet;
}

void rock_pitch_prepare(unsigned int pending)
{
    mutex_lock(&pitch_mutex);
    if (!reserved || pending != request)
        goto done;

    /* Do not hold the lease mutex across codec and playback teardown. Any
     * takeover during that wait invalidates this request before route setup. */
    mutex_unlock(&pitch_mutex);
    playback_prepare_recording();
    mutex_lock(&pitch_mutex);
    if (!reserved || pending != request)
        goto done;

    talk_disable(true);
    speech_disabled = true;
    /* UI tones can start the playback clock while capture owns it. */
    mixer_channel_stop(PCM_MIXER_CHAN_BEEP);
    saved_rate = mixer_get_frequency();
    pcm_init_recording();
    audio_set_output_source(AUDIO_SRC_PLAYBACK);
    int source = global_settings.rec_source;
    /* Use the analog mux only. A tuner session can end on the USB thread,
     * which must not enter the serial dock's shared iAP transmit path. */
    audio_input_mux(source, SRCF_RECORDING);
    audiohw_set_monitor(false);
    if (source == AUDIO_SRC_MIC)
    {
        int gain = pitch_gain(SOUND_MIC_GAIN, global_settings.rec_mic_gain);
        audio_set_recording_gain(gain, 0, AUDIO_GAIN_MIC);
    }
    else
    {
        int left = pitch_gain(SOUND_LEFT_GAIN, global_settings.rec_left_gain);
        int right = pitch_gain(SOUND_RIGHT_GAIN, global_settings.rec_right_gain);
        audio_set_recording_gain(left, right, AUDIO_GAIN_LINEIN);
    }

    mixer_set_frequency(ROCK_PITCH_RATE | SAMPR_TYPE_REC);
    ready = -1;
    writing = 0;
    running = true;
    pcm_record_data(pitch_next_block, pitch_dma_status,
                    capture[0], sizeof(capture[0]));
    deadline = current_tick + 2 * HZ;

done:
    mutex_unlock(&pitch_mutex);
}

/* The sink switch holds this lock until the new sink is installed. Keep the
 * built-in sink selected through PCM close and rate restoration. */
bool rock_pitch_sink_lock(void)
{
    if (!initialized)
        return false;
    mutex_lock(&pitch_mutex);
    if (reserved)
    {
        allowed = false;
        pitch_stop_locked();
    }
    return true;
}

void rock_pitch_sink_unlock(void)
{
    mutex_unlock(&pitch_mutex);
}
#endif

enum rock_pitch_result rock_pitch_read(int16_t (*samples)[ROCK_PITCH_SAMPLES])
{
    enum rock_pitch_result result = ROCK_PITCH_STOPPED;
    mutex_lock(&pitch_mutex);
#ifdef HAVE_ROCK_PITCH_DMA
    if (reserved)
    {
        result = ROCK_PITCH_WAITING;
        if (running)
        {
            pcm_rec_lock();
            if (pcm_is_recording() && ready >= 0)
            {
                memcpy(*samples, capture[ready], sizeof(*samples));
                ready = -1;
                deadline = current_tick + 2 * HZ;
                result = ROCK_PITCH_READY;
            }
            pcm_rec_unlock();
        }
        if ((running && !pcm_is_recording()) || TIME_AFTER(current_tick, deadline))
        {
            pitch_stop_locked();
            result = ROCK_PITCH_FAILED;
        }
    }
#else
    (void)samples;
    if (running)
        result = ROCK_PITCH_WAITING; /* SDL displays a labelled preview. */
#endif
    mutex_unlock(&pitch_mutex);
    return result;
}
