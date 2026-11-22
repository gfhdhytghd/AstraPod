/* RockDoom sound effects on the foreground PCM queue. GPL-2.0-or-later. */
void I_SubmitSound(void)
{
    if (!stream_active)
        return;

    int pending = rb->rock_stream_pending();
    if (pending < 0)
    {
        I_ShutdownSound();
        return;
    }

    for (int block = pending; block < ROCK_STREAM_QUEUE_SIZE; block++)
    {
        I_UpdateSound();
        if (!rb->rock_stream_submit(mixbuffer, sizeof(mixbuffer)))
            break;
    }
}

void I_ShutdownSound(void)
{
    stream_active = false;
    rb->rock_stream_close();
    for (int i = 0; i < NUM_CHANNELS; i++)
        stopchan(i);
}

void I_InitSound(void)
{
    if (!enable_sound)
        return;
    samplerate = rb->rock_stream_open();
    stream_active = samplerate != 0;
    if (!stream_active)
        samplerate = 44100;
    vol_lookup = malloc(128 * 256 * sizeof(*vol_lookup));
    if (!vol_lookup)
        I_Error("Not enough memory for sound");
}
