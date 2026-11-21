/* Foreground plugin click channel. GNU GPL version 2 or later. */

#ifdef ASTRAPOD_ROCK_COMPAT
/* A firmware-owned marker. The foreground module supplies samples only. */
static const struct mixer_play_cbs click_owner;
#endif

static inline unsigned int channel_amplitude(const struct mixer_channel *chan)
{
#ifdef ASTRAPOD_ROCK_COMPAT
    if (chan->play_cbs == &click_owner)
        return MIX_AMP_UNITY;
#endif
    return chan->amplitude;
}

#ifdef ASTRAPOD_ROCK_COMPAT
static void channel_stopped(struct mixer_channel *chan);

void mixer_click_play(const void *samples, size_t size, unsigned int rate)
{
    const struct mixer_channel *chan = &channels[PCM_MIXER_CHAN_PLAYBACK];

    pcm_play_lock();
    if (mixer_get_frequency() == rate &&
        (chan->status == CHANNEL_STOPPED || chan->play_cbs == &click_owner))
        mixer_channel_play_data(PCM_MIXER_CHAN_PLAYBACK, &click_owner,
                                samples, size);
    pcm_play_unlock();
}

void mixer_click_stop(void)
{
    struct mixer_channel *chan = &channels[PCM_MIXER_CHAN_PLAYBACK];

    pcm_play_lock();
    if (chan->play_cbs == &click_owner)
        channel_stopped(chan);
    pcm_play_unlock();
    /* PCM and DMA play copies in downmix_buf, never the module's buffer. */
}
#endif
