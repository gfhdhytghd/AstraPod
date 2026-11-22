/* XRick foreground WAV mixer. GNU GPL version 2 or later. */
#include "plugin.h"
#include "xrick/system/system.h"

enum { CHANNELS = 5, READ_BYTES = 2048 };

static struct {
    const sound_t *sound;
    file_t file;
    size_t remaining, next, end;
    int loop;
    int sample;
    unsigned phase;
    unsigned char data[READ_BYTES];
} channels[CHANNELS];

static int16_t pcm[ROCK_STREAM_BLOCK_BYTES / sizeof(int16_t)];
static unsigned rate;
static bool paused;
const U8 syssnd_period = 10;

static void end_channel(unsigned index)
{
    if (channels[index].file)
        sysfile_close(channels[index].file);
    channels[index].file = NULL;
    channels[index].sound = NULL;
}

bool syssnd_init(void)
{
    rate = rb->rock_stream_open();
    return true;
}

void syssnd_stopAll(void)
{
    for (unsigned i = 0; i < CHANNELS; i++)
        end_channel(i);
}

void syssnd_shutdown(void)
{
    if (rate)
        rb->rock_stream_close();
    rate = 0;
    paused = false;
    syssnd_stopAll();
}

void syssnd_pauseAll(bool pause)
{
    if (rate && !rb->rock_stream_pause(pause))
        syssnd_shutdown();
    paused = pause;
}

void syssnd_stop(sound_t *sound)
{
    for (unsigned i = 0; i < CHANNELS; i++)
        if (channels[i].sound == sound)
            end_channel(i);
}

void syssnd_play(sound_t *sound, S8 loop)
{
    if (!rate || !sound || !loop)
        return;

    /* Restart the matching voice, or use the first free slot. */
    syssnd_stop(sound);
    for (unsigned i = 0; i < CHANNELS; i++)
    {
        if (channels[i].file)
            continue;
        file_t file = sysfile_open(sound->name);
        if (!file)
            return;
        if (sysfile_size(file) != (off_t)(sizeof(wave_header_t) + sound->len) ||
            sysfile_seek(file, sizeof(wave_header_t), SEEK_SET) != sizeof(wave_header_t))
        {
            sysfile_close(file);
            return;
        }
        channels[i].sound = sound;
        channels[i].file = file;
        channels[i].remaining = sound->len;
        channels[i].next = channels[i].end = 0;
        channels[i].loop = loop;
        channels[i].phase = rate;
        return;
    }
}

static bool read_sample(unsigned i)
{
    if (channels[i].next == channels[i].end)
    {
        file_t file = channels[i].file;
        if (!channels[i].remaining)
        {
            if ((channels[i].loop > 0 && --channels[i].loop == 0) ||
                sysfile_seek(file, sizeof(wave_header_t), SEEK_SET) != sizeof(wave_header_t))
                return false;
            channels[i].remaining = channels[i].sound->len;
        }
        size_t size = MIN(channels[i].remaining, sizeof(channels[i].data));
        if (sysfile_read(file, channels[i].data, 1, size) != (int)size)
            return false;
        channels[i].remaining -= size;
        channels[i].next = 0;
        channels[i].end = size;
    }

    channels[i].sample = (channels[i].data[channels[i].next++] - 128) * 256;
    return true;
}

void syssnd_update(void)
{
    if (!rate || paused)
        return;
    int pending = rb->rock_stream_pending();
    if (pending < 0)
    {
        syssnd_shutdown();
        return;
    }

    /* At most two blocks of 20 ms each. Poll input between producer calls. */
    unsigned frames = rate / 50;
    for (; pending < ROCK_STREAM_QUEUE_SIZE; pending++)
    {
        unsigned frame;
        for (frame = 0; frame < frames; frame++)
        {
            bool playing = false;
            int mixed = 0;
            for (unsigned i = 0; i < CHANNELS; i++)
            {
                if (!channels[i].file)
                    continue;
                while (channels[i].phase >= rate)
                {
                    channels[i].phase -= rate;
                    if (!read_sample(i))
                    {
                        end_channel(i);
                        break;
                    }
                }
                if (!channels[i].file)
                    continue;
                playing = true;
                mixed += channels[i].sample;
                channels[i].phase += Wave_SAMPLE_RATE;
            }
            if (!playing)
                break;

            mixed = MAX(-32768, MIN(32767, mixed));
            pcm[frame * 2] = pcm[frame * 2 + 1] = mixed;
        }
        if (!frame)
            break;
        if (!rb->rock_stream_submit(pcm, frame * 4))
        {
            syssnd_shutdown();
            break;
        }
    }
}
