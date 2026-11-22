#!/usr/bin/env python3
"""Bind upstream FFT to an owned playback snapshot."""
from pathlib import Path
import sys


def replace(text, before, after):
    if text.count(before) != 1:
        raise SystemExit(f"review FFT adapter for {before!r}")
    return text.replace(before, after)


source, target = map(Path, sys.argv[1:3])
text = source.read_text()
if source.name == "fft.c":
    text = replace(text, "rb->mixer_channel_status(PCM_MIXER_CHAN_PLAYBACK) == CHANNEL_PLAYING",
                   "rb->audio_status() == AUDIO_STATUS_PLAY")
    text = replace(text, "static void apply_window_func(enum fft_window_func mode)",
                   "static void apply_window_func(enum fft_window_func mode,\n"
                   "                              unsigned int frames)")
    text = replace(text, """    for(int i = 0; i < ARRAYLEN_IN; ++i)
        input[i].r = (input[i].r * c[i] + 16384) >> 15;""",
                   """    /* Stretch the window across the contiguous snapshot. */
    for (unsigned i = 0; i < frames; ++i)
    {
        unsigned index = i * (ARRAYLEN_IN - 1) / (frames - 1);
        input[i].r = (input[i].r * c[index] + 16384) >> 15;
    }""")
    begin = text.index("    int count;\n    const int16_t *value")
    end = text.index("    rb->yield();", begin)
    text = text[:begin] + """    static int16_t samples[PCM_WAVEFORM_FRAMES * 2];
    size_t frames = rb->waveform_read(samples, 10);
    if (frames < 2)
        return false;
    frames = MIN(frames, ARRAYLEN_IN);
    rb->memset(input, 0, sizeof(input));
    for (size_t i = 0; i < frames; ++i)
        input[i].r = (samples[i * 2] + samples[i * 2 + 1]) >> 1;
    apply_window_func(fft.window_func, frames);

""" + text[end:]
    for old, value in (("cancel", "false"), ("trigger", "true")):
        text = replace(text, f"    rb->{old}_cpu_boost();",
                       f"""#ifdef CPU_BOOST_LOGGING
    rb->cpu_boost_({value}, __FILE__, __LINE__);
#else
    rb->cpu_boost({value});
#endif""")
    text = replace(text, "    if (rb->memcmp(&fft, &fft_disk, sizeof(fft)))",
                   "    if (!rb->plugin_usb_taken() &&\n"
                   "        rb->memcmp(&fft, &fft_disk, sizeof(fft)))")
    text = replace(text, """        configfile_save(cfg_filename, disk_config, ARRAYLEN(disk_config),
                        CFGFILE_VERSION);""",
                   """        if (configfile_save(cfg_filename, disk_config, ARRAYLEN(disk_config),
                            CFGFILE_VERSION) < 0)
            rb->splash(HZ, "Could not save FFT settings");""")
elif source.name == "osd.c":
    text = replace(text, "rb->viewport_set_fullscreen(&osd->vp, SCREEN_MAIN);",
                   "rb->viewport_set_defaults(&osd->vp, SCREEN_MAIN);")
    text = replace(text, "rb->viewport_set_buffer(NULL, &native_osd.framebuf, SCREEN_MAIN);",
                   "rb->screens[SCREEN_MAIN]->viewport_set_buffer(NULL, &native_osd.framebuf);")
    text = replace(text, "rb->viewport_set_buffer(NULL, NULL, SCREEN_MAIN);",
                   "rb->screens[SCREEN_MAIN]->viewport_set_buffer(NULL, NULL);")
elif source.name == "configfile.c":
    text = replace(text, '#include "configfile.h"',
                   '#include "configfile.h"\n#include "plugins/phase3-file-io.h"')
    start = text.index("    fd = rb->creat(buf, 0666);")
    end = text.index("\n}\n", start)
    text = text[:start] + r'''    char temp[MAX_PATH];
    fd = begin_replace(buf, temp);
    if (fd < 0)
        return -1;

    bool ok = rb->fdprintf(fd, "file version: %10d\n", version) >= 0;
    for (i = 0; ok && i < num_items; ++i)
        ok = cfg[i].type == TYPE_ENUM &&
             rb->fdprintf(fd, "%s: %s\n", cfg[i].name,
                          cfg[i].values[*cfg[i].int_p]) >= 0;
    return end_replace(fd, temp, buf, ok) ? 0 : -1;''' + text[end:]
    text = replace(text, """        rb->settings_parseline(buf, &name, &val);

        /* Bail""", """        if (!rb->settings_parseline(buf, &name, &val))
            continue;

        /* Bail""")
else:
    raise SystemExit(f"unexpected FFT source: {source}")
target.write_text(text)
