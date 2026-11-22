/* Keep the outgoing sink's prefetched plugin samples off the next sink. */
#ifdef ASTRAPOD_ROCK_COMPAT
static bool downmix_has_plugin_stream;

static inline void mixer_mark_plugin_mix(const struct mixer_channel *chan)
{
    if (chan == &channels[PCM_MIXER_CHAN_ROCK_STREAM])
        downmix_has_plugin_stream = true;
}

void mixer_discard_plugin_prefetch(void)
{
    /* The caller holds both sink locks before the route changes. */
    if (downmix_has_plugin_stream)
    {
        memset(downmix_buf[downmix_index], 0, next_size);
        downmix_has_plugin_stream = false;
    }
}
#endif
