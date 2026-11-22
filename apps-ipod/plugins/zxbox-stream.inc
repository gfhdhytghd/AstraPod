/* ZXBox frame audio through the foreground stream. GPL-2.0-or-later. */
static unsigned int stream_rate;
static int16_t output[ROCK_STREAM_BLOCK_BYTES / sizeof(int16_t)];

void init_spect_sound(void)
{
    stream_rate = rb->rock_stream_open();
    sound_avail = stream_rate != 0;
}

void play_sound(int evenframe)
{
    if (evenframe || !sound_avail)
        return;

    int pending = rb->rock_stream_pending();
    if (pending < 0)
    {
        sound_avail = 0;
        return;
    }

    z80_proc.sound_change = 0;
    process_sound();
    unsigned int frames = stream_rate / 50;
    for (unsigned int i = 0; i < frames; i++)
    {
        int sample = (int)sp_sound_buf[i * TMNUM / frames] - 128;
        output[i * 2] = output[i * 2 + 1] = sample * 128;
    }
    if (pending < ROCK_STREAM_QUEUE_SIZE)
        rb->rock_stream_submit(output, frames * 4);
}

void setbufsize(void) {}
void autoclose_sound(void) {}
