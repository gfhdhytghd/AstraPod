/* Native playback snapshot for foreground plugins. GNU GPL v2 or later. */
#ifdef ASTRAPOD_ROCK_COMPAT
#include "pcm_waveform.h"

/* Sink reconfiguration can yield while it replaces the active buffer.
 * The mixer control thread paths bracket that interval. */
static unsigned int waveform_reconfiguring;

static inline void waveform_reconfigure_begin(void)
{
    waveform_reconfiguring++;
}

static inline void waveform_reconfigure_end(void)
{
    waveform_reconfiguring--;
}

size_t mixer_waveform_read(int16_t *samples, unsigned int window_ms)
{
    static int16_t snapshot[PCM_WAVEFORM_FRAMES * 2];
    struct mixer_channel *chan = &channels[PCM_MIXER_CHAN_PLAYBACK];
    size_t frames = 0;

    if (!window_ms || waveform_reconfiguring)
        return 0;

    pcm_play_lock();
    if (pcm_is_playing() && chan->status == CHANNEL_PLAYING &&
        chan->start && chan->play_cbs != &click_owner &&
        *find_array_ptr((void **)active_channels, chan))
    {
        size_t frame_bytes = 2 * sizeof(*samples);
        size_t requested = SAMPR_NUM(pcm_get_frequency()) *
                           MIN(window_ms, 10u) / 1000;
        frames = MIN(MIN(requested, PCM_WAVEFORM_FRAMES),
                     chan->size / frame_bytes);
        memcpy(snapshot, chan->start, frames * frame_bytes);
    }
    pcm_play_unlock();

    /* Copy to the caller after releasing the sink's PCM lock. */
    if (frames)
        memcpy(samples, snapshot, frames * 2 * sizeof(*samples));
    return frames;
}
#else
static inline void waveform_reconfigure_begin(void) {}
static inline void waveform_reconfigure_end(void) {}
#endif
