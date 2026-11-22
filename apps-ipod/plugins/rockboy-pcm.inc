/* Rockboy sound producer. GPL-2.0-or-later. */
struct pcm pcm;
static int16_t samples[ROCK_STREAM_BLOCK_BYTES / sizeof(int16_t)];

void rockboy_pcm_init(void)
{
    unsigned int rate = rb->rock_stream_open();
    if (!rate)
        return;
    pcm.hz = rate;
    pcm.stereo = 1;
    pcm.len = ARRAYLEN(samples);
    pcm.buf = (short *)samples;
}

void rockboy_pcm_close(void)
{
    rb->rock_stream_close();
    memset(&pcm, 0, sizeof(pcm));
    options.sound = 0;
}

int rockboy_pcm_submit(void)
{
    if (!pcm.buf || pcm.pos < pcm.len)
        return 0;

    int pending = rb->rock_stream_pending();
    if (pending < 0)
    {
        rockboy_pcm_close();
        return 0;
    }
    while (pending == ROCK_STREAM_QUEUE_SIZE)
    {
        rb->sleep(1);
        pending = rb->rock_stream_pending();
    }
    if (pending < 0 || !rb->rock_stream_submit(samples, sizeof(samples)))
    {
        rockboy_pcm_close();
        return 0;
    }
    pcm.pos = 0;
    return 1;
}
