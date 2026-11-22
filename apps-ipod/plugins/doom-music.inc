/* Foreground DMX MUS synthesis. GPL-2.0-or-later. */
#define MUS_HZ 140
#define MUS_VOICES 12

static const uint8_t *song_start, *song_end, *score;
static bool song_loop, song_paused;
static int64_t until_event;
static unsigned int next_voice;
static uint8_t velocity[16];
static uint8_t patch[16];
static uint32_t noise_state = 1;

static struct mus_voice {
    uint32_t phase, step;
    uint8_t channel, note, level;
    unsigned int decay;
} mus_voices[MUS_VOICES];

static void mus_reset(void)
{
    memset(mus_voices, 0, sizeof(mus_voices));
    memset(velocity, 64, sizeof(velocity));
    memset(patch, 0, sizeof(patch));
    next_voice = 0;
}

static int mus_byte(void)
{
    return score < song_end ? *score++ : -1;
}

static int mus_data(void)
{
    return mus_byte() & 0x7f; /* Caller checks the remaining event length. */
}

static void mus_note_off(unsigned int channel, int note)
{
    for (unsigned int i = 0; i < MUS_VOICES; i++)
        if (mus_voices[i].channel == channel &&
            (note < 0 || mus_voices[i].note == note))
            mus_voices[i].level = 0;
}

static void mus_note_on(unsigned int channel, int note)
{
    static const unsigned int octave_millihz[] = {
        440000, 466164, 493883, 523251, 554365, 587330,
        622254, 659255, 698456, 739989, 783991, 830609,
    };
    int offset = (note + 3) / 12 - 6;
    uint32_t frequency = octave_millihz[(note + 3) % 12];
    frequency = offset < 0 ? frequency >> -offset : frequency << offset;
    if (!velocity[channel] || frequency >= (unsigned int)samplerate * 500)
        return;

    mus_note_off(channel, note);
    unsigned int slot = next_voice;
    for (unsigned int i = 0; i < MUS_VOICES; i++)
        if (!mus_voices[(next_voice + i) % MUS_VOICES].level)
        {
            slot = (next_voice + i) % MUS_VOICES;
            break;
        }

    mus_voices[slot] = (struct mus_voice) {
        .step = ((uint64_t)frequency << 32) / ((unsigned int)samplerate * 1000),
        .channel = channel,
        .note = note,
        .level = velocity[channel],
        .decay = channel == 15 ? samplerate / 16 : 0,
    };
    next_voice = (slot + 1) % MUS_VOICES;
}

/* Continue through zero-delay events, within a fixed per-frame budget. */
static void mus_events(void)
{
    for (unsigned int count = 0; count < 256 && score && until_event <= 0; count++)
    {
        int descriptor = mus_byte();
        if (descriptor < 0)
            break;

        unsigned int channel = descriptor & 15;
        unsigned int type = (descriptor >> 4) & 7;
        if (type == 6)
        {
            mus_reset();
            score = song_loop ? song_start : NULL;
            until_event = 0;
            return;
        }
        if (type > 4 || score == song_end)
            break;

        int value = mus_byte();
        switch (type)
        {
            case 0: /* Release note. */
                mus_note_off(channel, value & 127);
                break;
            case 1: /* Play note, with an optional channel velocity. */
                if (value & 128)
                {
                    if (score == song_end)
                        goto invalid;
                    velocity[channel] = mus_data();
                }
                mus_note_on(channel, value & 127);
                break;
            case 2: /* Pitch wheel is optional in this synthesizer. */
                break;
            case 3:
                if (value == 10 || value == 11 || value == 14)
                    mus_note_off(channel, -1);
                break;
            case 4:
                if (score == song_end)
                    goto invalid;
                if (value == 0)
                    patch[channel] = mus_data();
                else
                    mus_byte();
                break;
        }

        if (descriptor & 128)
        {
            uint32_t ticks = 0;
            for (int i = 0; i < 4; i++)
            {
                int byte = mus_byte();
                if (byte < 0)
                    goto invalid;
                ticks = (ticks << 7) | (byte & 127);
                if (!(byte & 128))
                {
                    until_event += (int64_t)ticks * samplerate;
                    if (until_event > 0)
                        return;
                    break;
                }
                if (i == 3)
                    goto invalid;
            }
        }
    }

invalid:
    mus_reset();
    score = NULL;
}

static void mus_mix(void)
{
    if (song_paused)
        return;
    for (unsigned int frame = 0; frame < SAMPLECOUNT && score; frame++)
    {
        if (until_event <= 0)
            mus_events();
        int sum = 0;
        for (unsigned int i = 0; i < MUS_VOICES; i++)
        {
            struct mus_voice *voice = &mus_voices[i];
            if (!voice->level)
                continue;
            int phase = voice->phase >> 24;
            int wave;
            if (voice->channel == 15)
            {
                noise_state = noise_state * 1664525u + 1013904223u;
                wave = (int)(noise_state >> 24) - 128;
                if (--voice->decay == 0)
                    voice->level = 0;
            }
            else if (patch[voice->channel] & 8)
                wave = phase < 128 ? 96 : -96;
            else
                wave = 2 * (phase < 128 ? phase : 255 - phase) - 127;
            voice->phase += voice->step;
            sum += wave * voice->level;
        }
        int music = sum * MIN(127, MAX(0, snd_MusicVolume)) / 2048;
        for (unsigned int channel = 0; channel < 2; channel++)
        {
            int mixed = mixbuffer[2 * frame + channel] + music;
            mixbuffer[2 * frame + channel] = MIN(32767, MAX(-32768, mixed));
        }
        until_event -= MUS_HZ;
    }
}

void I_InitMusic(void) {}

void I_StopSong(int handle)
{
    if (handle == 1)
    {
        score = NULL;
        mus_reset();
    }
}

void I_UnRegisterSong(int handle)
{
    I_StopSong(handle);
    if (handle == 1)
        song_start = song_end = NULL;
}

void I_ShutdownMusic(void)
{
    I_UnRegisterSong(1);
}

int I_RegisterRockSong(const void *data, int size)
{
    const uint8_t *bytes = data;
    if (!stream_active || !bytes || size < 16 || memcmp(bytes, "MUS\x1a", 4))
        return 0;
    unsigned int length = bytes[4] | bytes[5] << 8;
    unsigned int start = bytes[6] | bytes[7] << 8;
    if (start < 16 || start > (unsigned int)size ||
        !length || length > (unsigned int)size - start)
        return 0;
    song_start = bytes + start;
    song_end = song_start + length;
    return 1;
}

void I_PlaySong(int handle, int looping)
{
    if (handle == 1)
    {
        mus_reset();
        score = song_start;
        until_event = 0;
        song_loop = looping;
        song_paused = false;
    }
}

void I_PauseSong(int handle)
{
    if (handle == 1)
        song_paused = true;
}

void I_ResumeSong(int handle)
{
    if (handle == 1)
        song_paused = false;
}

int I_QrySongPlaying(int handle)
{
    return handle == 1 && score != NULL;
}
