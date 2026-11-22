/* AstraPod file bindings for WavPack. GNU GPL version 2 or later. */
#include "encoder-file.h"

#define WAV_HEADER_SIZE 44
#define BLOCK_FRAMES 4096
#define INPUT_SIZE (BLOCK_FRAMES * 2 * sizeof(int16_t))
#define OUTPUT_SIZE (4 * INPUT_SIZE)

static uint16_t wave_u16(const unsigned char *bytes)
{
    return bytes[0] | (bytes[1] << 8);
}

static uint32_t wave_u32(const unsigned char *bytes)
{
    return wave_u16(bytes) | ((uint32_t)wave_u16(bytes + 2) << 16);
}

static bool supported_rate(uint32_t rate)
{
    static const uint32_t rates[] = {
        6000, 8000, 9600, 11025, 12000, 16000, 22050, 24000,
        32000, 44100, 48000, 64000, 88200, 96000, 192000,
    };
    for (unsigned int i = 0; i < ARRAYLEN(rates); i++)
        if (rate == rates[i])
            return true;
    return false;
}

static bool canonical_wave(const unsigned char *header, off_t size)
{
    unsigned int channels = wave_u16(header + 22);
    uint32_t rate = wave_u32(header + 24);
    uint32_t data_size = wave_u32(header + 40);
    return !rb->memcmp(header, "RIFF", 4) &&
           !rb->memcmp(header + 8, "WAVEfmt ", 8) &&
           !rb->memcmp(header + 36, "data", 4) &&
           (uint64_t)wave_u32(header + 4) + 8 == (uint64_t)size &&
           wave_u32(header + 16) == 16 && wave_u16(header + 20) == 1 &&
           (channels == 1 || channels == 2) && supported_rate(rate) &&
           wave_u16(header + 32) == channels * 2 &&
           wave_u32(header + 28) == rate * channels * 2 &&
           wave_u16(header + 34) == 16 &&
           data_size > 0 && data_size % (channels * 2) == 0 &&
           (uint64_t)data_size + WAV_HEADER_SIZE == (uint64_t)size;
}

static bool pack_wave(struct encoder_file *file, WavpackContext *context,
                      unsigned char *scratch, unsigned int channels,
                      uint32_t frames, bool *cancelled)
{
    static int32_t samples[BLOCK_FRAMES * 2];
    unsigned char *encoded = scratch + INPUT_SIZE;
    for (uint32_t done = 0; done < frames;)
    {
        if (encoder_cancel(file))
        {
            *cancelled = true;
            return false;
        }

        unsigned int count = MIN(frames - done, BLOCK_FRAMES);
        size_t bytes = count * channels * 2;
        if (rb->read(file->input, scratch, bytes) != (ssize_t)bytes)
            return false;
        for (unsigned int i = 0; i < count * channels; i++)
            samples[i] = (int16_t)wave_u16(scratch + i * 2);
        if (!WavpackStartBlock(context, encoded, encoded + OUTPUT_SIZE) ||
            !WavpackPackSamples(context, samples, count))
            return false;
        size_t length = WavpackFinishBlock(context);
        if (!length || length > OUTPUT_SIZE ||
            rb->write(file->output, encoded, length) != (ssize_t)length)
            return false;
        done += count;
        rb->splash_progress(done, frames, "Encoding WavPack\nMenu cancels");
    }
    return true;
}

enum plugin_status plugin_start(const void *parameter)
{
    const char *path = parameter;
    if (!path)
    {
        rb->splash(2 * HZ,
                   "Hold Select on a WAV file and choose Encode WavPack");
        return PLUGIN_OK;
    }

    char destination[MAX_PATH];
    struct encoder_file file = {.input = -1, .output = -1};
    if (!encoder_destination(destination, path, ".wv"))
        return encoder_result(&file, false, false, NULL);

    unsigned char header[WAV_HEADER_SIZE];
    file.input = rb->open(path, O_RDONLY);
    if (file.input < 0 ||
        rb->read(file.input, header, sizeof(header)) != sizeof(header) ||
        !canonical_wave(header, rb->ffilesize(file.input)))
    {
        encoder_discard(&file);
        rb->splash(2 * HZ, "Requires a standard 16-bit PCM WAV");
        return PLUGIN_ERROR;
    }
    if (rb->file_exists(destination) &&
        !encoder_confirm("Replace WavPack file?"))
        return encoder_result(&file, false, true, NULL);

    size_t capacity;
    unsigned char *scratch = rb->plugin_get_buffer(&capacity);
    if (!scratch || capacity < INPUT_SIZE + OUTPUT_SIZE ||
        !encoder_open_output(&file, destination))
        return encoder_result(&file, false, false, NULL);

    WavpackConfig config = {
        .bits_per_sample = 16,
        .bytes_per_sample = 2,
        .sample_rate = wave_u32(header + 24),
        .num_channels = wave_u16(header + 22),
    };
    uint32_t frames = wave_u32(header + 40) / (config.num_channels * 2);
    WavpackContext *context = WavpackOpenFileOutput();
    if (!WavpackSetConfiguration(context, &config, frames))
        return encoder_result(&file, false, false, NULL);
    WavpackAddWrapper(context, header, sizeof(header));

#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    rb->cpu_boost(true);
#endif
    bool cancelled = false;
    rb->splash_progress_set_delay(0);
    bool ok = pack_wave(&file, context, scratch, config.num_channels, frames,
                        &cancelled);
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    rb->cpu_boost(false);
#endif
    if (ok && encoder_cancel(&file))
    {
        cancelled = true;
        ok = false;
    }
    if (ok)
        ok = encoder_commit(&file, destination);
    return encoder_result(&file, ok, cancelled, "WavPack saved");
}
