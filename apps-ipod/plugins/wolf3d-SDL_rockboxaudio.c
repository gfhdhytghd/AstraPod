/* SDL mixer worker to foreground PCM handoff. GPL-2.0-or-later. */
#include "SDL_config.h"
#include "SDL_audio.h"
#include "../SDL_sysaudio.h"
#include "../SDL_audio_c.h"

struct SDL_PrivateAudioData {
    uint32_t samples[ROCK_STREAM_BLOCK_BYTES / sizeof(uint32_t)];
};

static SDL_AudioDevice *device;
static volatile bool ready;
static volatile bool revoked;
static unsigned int rate;

unsigned int wolf_audio_rate(void)
{
    return rate;
}

void wolf_audio_pump(void)
{
    if (!device || revoked)
        return;

    int pending = rb->rock_stream_pending();
    if (pending < 0)
    {
        revoked = true;
        ready = false;
        return;
    }
    if (ready && pending < ROCK_STREAM_QUEUE_SIZE &&
        rb->rock_stream_submit(device->hidden->samples, device->spec.size))
        ready = false;
}

static int open_audio(SDL_AudioDevice *audio, SDL_AudioSpec *spec)
{
    if (!rate || spec->freq != (int)rate || spec->format != AUDIO_S16SYS ||
        spec->channels != 2 || !spec->samples ||
        spec->samples > ROCK_STREAM_BLOCK_BYTES / 4)
        return -1;

    SDL_CalculateAudioSpec(spec);
    ready = false;
    revoked = false;
    device = audio;
    return 0;
}

static Uint8 *get_buffer(SDL_AudioDevice *audio)
{
    return (Uint8 *)audio->hidden->samples;
}

static void play_audio(SDL_AudioDevice *audio)
{
    (void)audio;
    if (!revoked)
        ready = true;
}

static void wait_audio(SDL_AudioDevice *audio)
{
    unsigned long deadline = *rb->current_tick +
        (audio->spec.samples * HZ + rate - 1) / rate;
    do
    {
        if (rb->rock_worker_stopping())
        {
            audio->enabled = 0;
            return;
        }
        if (!ready && (!revoked || !TIME_BEFORE(*rb->current_tick, deadline)))
            return;
        rb->sleep(1);
    }
    while (audio->enabled);
}

static void close_audio(SDL_AudioDevice *audio)
{
    (void)audio;
    device = NULL;
    ready = false;
}

static void delete_device(SDL_AudioDevice *audio)
{
    SDL_free(audio->hidden);
    SDL_free(audio);
}

static int available(void)
{
    return 1;
}

static SDL_AudioDevice *create_device(int index)
{
    (void)index;
    SDL_AudioDevice *audio = SDL_calloc(1, sizeof(*audio));
    if (!audio)
        return NULL;
    audio->hidden = SDL_calloc(1, sizeof(*audio->hidden));
    if (!audio->hidden)
    {
        SDL_free(audio);
        return NULL;
    }

    audio->OpenAudio = open_audio;
    audio->GetAudioBuf = get_buffer;
    audio->PlayAudio = play_audio;
    audio->WaitAudio = wait_audio;
    audio->CloseAudio = close_audio;
    audio->free = delete_device;
    return audio;
}

AudioBootStrap ROCKBOXAUD_bootstrap = {
    "rockbox", "AstraPod PCM stream", available, create_device
};

unsigned int wolf_open_audio(void)
{
    rate = rb->rock_stream_open();
    return rate;
}
