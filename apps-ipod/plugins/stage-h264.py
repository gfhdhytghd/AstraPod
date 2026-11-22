#!/usr/bin/env python3
"""Stage the upstream MP4 player for an owned video-only session."""
from pathlib import Path
import sys

src, dst = map(Path, sys.argv[1:])
code = src.read_text()


def replace(before, after):
    global code
    if code.count(before) != 1:
        raise ValueError(f"{src}: expected one instance of {before!r}")
    code = code.replace(before, after)


def cut(start, end, body=""):
    global code
    begin = code.index(start)
    code = code[:begin] + body + code[code.index(end, begin):]


if src.name == "h264_player.c":
    replace('    const char *path = parameter;',
            '    char selected[MAX_PATH];\n    const char *path = parameter;')
    cut("        rb->splash(HZ * 2, \"No video file\");",
        "    if (buffer == NULL", """        if (rb->pick_file(selected, sizeof(selected), "/", "Choose MP4 video",
                          ".mp4,.m4v", 0, false) <= 0)
            return rb->plugin_usb_taken() ? PLUGIN_USB_CONNECTED : PLUGIN_OK;
        path = selected;
    }

    if (!rb->hw_h264)
    {
        rb->splash(2 * HZ, "H.264 requires the iPod video decoder");
        return PLUGIN_OK;
    }

    buffer = rb->plugin_claim_scratch(512 * 1024, SIZE_MAX, &buffer_size);
""")
    replace("    rb->plugin_release_audio_buffer();\n", "")
    replace('result = video_h264_play(path, buffer, buffer_size);',
            '''#ifdef HAVE_HW_H264
    result = video_h264_play(path, buffer, buffer_size);
#else
    result = -1;
#endif''')

elif src.name == "video_playback.c":
    for header in ("backlight.h", "button.h", "crc32.h", "dir.h", "file.h",
                   "font.h", "kernel.h", "lang.h", "lcd.h", "menu.h", "misc.h",
                   "pcmbuf.h", "rbpaths.h", "settings.h", "sound.h", "splash.h",
                   "system.h", "video_audio.h", "video_pcm.h"):
        replace(f'#include "{header}"\n', "")
    cut("#define VIDEO_RESUME_MAGIC", "enum video_input_action")
    replace("#define VIDEO_PCM_BUFFER         (256u * 1024u)\n", "")
    replace("    VIDEO_INPUT_MENU,\n", "")
    cut("struct video_resume_record", "struct video_pool")
    cut("static void video_resume_filename", "static void video_set_paused")
    cut("enum video_menu_item", "static void video_scale_plane")
    replace("long *pause_started, bool have_audio,",
            "long *pause_started,")
    cut("        if (have_audio)\n", "        if (*cpu_boosted)")
    cut("        if (have_audio)\n", "    }\n}")
    replace("                                           bool have_audio,\n", "")
    cut("    if (action == ACTION_WPS_MENU)", "    if (action == ACTION_WPS_SKIPPREV",
        """    if (action == ACTION_WPS_MENU || action == ACTION_STD_CANCEL ||
        action == ACTION_WPS_STOP)
        return VIDEO_INPUT_EXIT;
""")
    replace("                         have_audio, cpu_boosted);", "                         cpu_boosted);")

    for line in (
        "    void *audio_codec_workspace;", "    void *pcm_buffer;",
        "    size_t audio_codec_workspace_size;", "    bool have_audio = false;",
        "    bool audio_master = false;", "    uint32_t last_sample = 0;",
        "    rb->pcmbuf_fade(false, true);", "    rb->pcmbuf_fade(false, false);",
        "    rb->sound_set(SOUND_VOLUME, rb->global_status->volume);",
        "        last_sample = sample;",
    ):
        replace(line + "\n", "")
    cut("    audio_codec_workspace_size = demux.audio_num_samples",
        "    if (video_samples == NULL")
    replace("""        scale_buffer == NULL ||
        (demux.audio_num_samples > 0 &&
         (audio_codec_workspace == NULL || pcm_buffer == NULL)))""",
            """        scale_buffer == NULL || decoder_size == 0)""")
    cut("    sample = video_resume_load", "    video_timing_for_sample")
    replace("demux->timescale > 0;", "demux->timescale > 0 && demux->mdat_len > 0;")
    replace("    return (uint32_t)(ticks * 1000u / demux->timescale);",
            """    uint64_t elapsed = ticks * 1000u / demux->timescale;
    return elapsed <= 24u * 60 * 60 * 1000 ? elapsed : 0;""")
    replace("    duration_ms = video_duration_ms(&demux);",
            """    duration_ms = video_duration_ms(&demux);
    if (!duration_ms)
    {
        rb->splash(HZ * 2, "Unsupported movie duration");
        goto cleanup;
    }""")
    cut("    if (demux.audio_format", "    rb->lcd_set_foreground")
    replace("                                 &pause_started, have_audio, &cpu_boosted,",
            "                                 &pause_started, &cpu_boosted,")
    cut("            if (action == VIDEO_INPUT_MENU)", "            if (redraw_requested")
    replace("""                    (int64_t)(audio_master ? video_pcm_get_clock_ms() :
                              pts_ms) +""", """                    (int64_t)pts_ms +""")
    replace("                    decoder = NULL;\n", "")
    replace("                if (have_audio)\n                    video_audio_seek(pts_ms);",
            """                have_frame = false;
                overlay_drawn = false;
                if (paused)
                    pause_started = *rb->current_tick;""")
    cut("            clock_ms = audio_master", "            if (clock_ms + 2",
        """            clock_ms = (uint32_t)((uint64_t)(unsigned long)
                ((*rb->current_tick) - start_tick) * 1000 / HZ);
""")
    cut("    if (result == 0)", "    if (video_fd >= 0)", "\ncleanup:\n")
    replace("            size == 0 || size > VIDEO_READ_BUFFER ||",
            """            size == 0 || size > VIDEO_READ_BUFFER ||
            offset < demux.mdat_offset ||
            (uint64_t)offset + size >
                (uint64_t)demux.mdat_offset + demux.mdat_len ||""")

dst.write_text(code)
