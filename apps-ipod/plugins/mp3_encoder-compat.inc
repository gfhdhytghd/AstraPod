/* AstraPod file and menu bindings for Shine. GNU LGPL version 2 or later. */
#include "encoder-file.h"

static uint32_t wave_bytes;
static int total_frames;
static bool cancelled;

static bool read_samples(int fd, short *buffer, unsigned int frames_requested)
{
    static uint16_t samples[MAX_SAMP_PER_FRAME * 2];
    unsigned int frame_size = cfg.channels * sizeof(*samples);
    unsigned int bytes = MIN(wave_bytes, frame_size * frames_requested);
    if (rb->read(fd, samples, bytes) != (ssize_t)bytes)
        return false;
    wave_bytes -= bytes;

    rb->memset((unsigned char *)samples + bytes, 0,
               frame_size * frames_requested - bytes);
    if (cfg.channels == 1)
    {
        for (unsigned int i = 0; i < frames_requested; i++)
        {
            buffer[2 * i] = samples[i];
            buffer[2 * i + 1] = 0;
        }
    }
    else
        rb->memcpy(buffer, samples, frame_size * frames_requested);
    return true;
}

/* ENCODER ENTRY */
enum plugin_status plugin_start(const void *parameter)
{
    const char *path = parameter;
    if (!path)
    {
        rb->splash(2 * HZ, "Hold Select on a WAV file and choose Encode MP3");
        return PLUGIN_OK;
    }

    char destination[MAX_PATH];
    struct encoder_file file = {.input = -1, .output = -1};
    if (!encoder_destination(destination, path, ".mp3"))
        return encoder_result(&file, false, false, NULL);

    file.input = rb->open(path, O_RDONLY);
    struct wave_data wave = {0};
    if (file.input < 0 || !read_wave_header(file.input, &wave) ||
        rb->lseek(file.input, wave.offset, SEEK_SET) != wave.offset)
    {
        encoder_discard(&file);
        rb->splash(2 * HZ, "Requires a mono or stereo 16-bit PCM WAV");
        return PLUGIN_ERROR;
    }

    switch (wave.rate)
    {
        case 16000: case 22050: case 24000:
        case 32000: case 44100: case 48000:
            break;
        default:
            encoder_discard(&file);
            rb->splash(2 * HZ, "Unsupported MP3 sample rate");
            return PLUGIN_ERROR;
    }

    static const int rates[] = {64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
    MENUITEM_STRINGLIST(standard, "MP3 bitrate", NULL, "64 kbps", "80 kbps",
                        "96 kbps", "112 kbps", "128 kbps", "160 kbps",
                        "192 kbps", "224 kbps", "256 kbps", "320 kbps");
    MENUITEM_STRINGLIST(limited, "MP3 bitrate", NULL, "64 kbps", "80 kbps",
                        "96 kbps", "112 kbps", "128 kbps", "160 kbps");
    MENUITEM_STRINGLIST(rate32k, "MP3 bitrate", NULL, "64 kbps", "80 kbps",
                        "96 kbps", "112 kbps", "128 kbps", "160 kbps",
                        "192 kbps", "224 kbps");
    const struct menu_item_ex *menu = &standard;
    if (wave.rate < 32000 || wave.channels == 1)
        menu = &limited;
    else if (wave.rate == 32000) /* Keep each frame in Shine's bit buffer. */
        menu = &rate32k;
    int selected = 4;
    int rate = rb->do_menu(menu, &selected, NULL, false);
    if ((unsigned int)rate >= ARRAYLEN(rates) ||
        (rb->file_exists(destination) && !encoder_confirm("Replace MP3 file?")))
        return encoder_result(&file, false, true, NULL);

    enc_buffer = rb->plugin_get_buffer(&enc_buffer_size);
    if (!enc_buffer || enc_buffer_size < 4096 ||
        !encoder_open_output(&file, destination))
        return encoder_result(&file, false, false, NULL);

    enc_buffer_size = 4096;
    rb->lcd_setfont(FONT_SYSFIXED);
    init_mp3_encoder_engine(wave.channels == 2, rates[rate], wave.rate);
    wave_bytes = wave.bytes;
    unsigned int frame_bytes = cfg.smpl_per_frm * wave.channels * 2;
    /* The playback scanner needs two consecutive MPEG headers. */
    total_frames = MAX(2u, (wave_bytes - 1) / frame_bytes + 1);
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    rb->cpu_boost(true);
#endif
    bool ok = compress(&file);
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
    return encoder_result(&file, ok, cancelled, "MP3 saved");
}
