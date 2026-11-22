/* Decode one stock VSWAP sample into the SDL mixer format. GPL-2.0-or-later. */
#define WOLF_MAX_DIGI_FRAMES (128u * 1024)

static Mix_Chunk *wolf_decode_digi(unsigned int which)
{
    if (!DigiList || which >= NumDigi)
        return NULL;

    unsigned int size = DigiList[which].length;
    const byte *source = PM_GetSound(DigiList[which].startpage);
    if (!size || size > (size_t)(PM_GetEnd() - source))
        return NULL;

    unsigned int rate = param_samplerate;
    unsigned int frames = (uint64_t)size * rate / ORIGSAMPLERATE;
    if (!frames || frames > WOLF_MAX_DIGI_FRAMES)
        return NULL;

    Mix_Chunk *chunk = malloc(sizeof(*chunk));
    if (!chunk)
        return NULL;
    Sint16 *samples = malloc(frames * 2 * sizeof(*samples));
    if (!samples)
    {
        free(chunk);
        return NULL;
    }

    unsigned int index = 0;
    unsigned int phase = 0;
    unsigned int reciprocal = (1u << 24) / rate;
    for (unsigned int i = 0; i < frames; ++i)
    {
        int a = source[index] - 128;
        int b = source[MIN(index + 1, size - 1)] - 128;
        int fraction = phase * reciprocal >> 16;
        int sample = a * 256 + (b - a) * fraction;
        samples[2 * i] = samples[2 * i + 1] = sample;
        phase += ORIGSAMPLERATE;
        if (phase >= rate)
        {
            phase -= rate;
            ++index;
        }

        if ((i & 4095) == 4095)
        {
            wolf_service();
            rb->yield();
        }
    }
    *chunk = (Mix_Chunk){
        .allocated = 1,
        .abuf = (Uint8 *)samples,
        .alen = frames * 2 * sizeof(*samples),
        .volume = MIX_MAX_VOLUME,
    };
    return chunk;
}
