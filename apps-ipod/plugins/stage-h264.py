#!/usr/bin/env python3
"""Stage the upstream MP4 player for an foreground video and AAC."""
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
    replace('#include "plugin.h"', '#include "plugin.h"\n#include "plugins/rock_compat.h"')
    replace('    const char *path = parameter;',
            """    struct rock_video_request fallback = {0};
    struct rock_video_request *request = parameter ?
        (void *)parameter : &fallback;
    char selected[MAX_PATH];
    const char *path = request->path;""")
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

#ifdef HAVE_HW_H264
    size_t needed = video_h264_scratch_size(path);
    if (!needed)
    {
        rb->splash(2 * HZ, "Unsupported MP4/M4V");
        return PLUGIN_OK;
    }
    buffer = rb->plugin_claim_scratch(needed, needed, &buffer_size);
#else
    buffer = NULL;
#endif
""")
    replace("    rb->plugin_release_audio_buffer();\n", "")
    replace('    result = video_h264_play(path, buffer, buffer_size);',
            '''#ifdef HAVE_HW_H264
    const struct video_play_options options = {
        .title = request->title,
        .battery_level = request->battery_level,
        .chapters = request->chapters,
        .chapter_count = request->chapter_count,
        .has_previous = request->has_previous,
        .max_volume = rb->sound_max(SOUND_VOLUME),
        .themed_controls = request->themed_controls,
        .accent = request->accent,
        .resume_menu = request->resume_menu,
    };
    result = video_h264_play_named(path, buffer, buffer_size, &options);
    switch (result)
    {
    case VIDEO_FINISHED: request->result = ROCK_VIDEO_FINISHED; break;
    case VIDEO_EXIT: request->result = ROCK_VIDEO_EXIT; break;
    case VIDEO_PREVIOUS: request->result = ROCK_VIDEO_PREVIOUS; break;
    case VIDEO_NEXT: request->result = ROCK_VIDEO_NEXT; break;
    default: break;
    }
#else
    result = -1;
#endif''')

elif src.name == "video_playback.c":
    for header in ("backlight.h", "button.h", "crc32.h", "dir.h", "file.h",
                   "font.h", "kernel.h", "lang.h", "lcd.h", "menu.h", "misc.h",
                   "pcmbuf.h", "rbpaths.h", "settings.h", "sound.h", "splash.h",
                   "system.h", "video_pcm.h"):
        replace(f'#include "{header}"\n', "")
    replace("    uint32_t crc = rb->crc_32(path, rb->strlen(path), 0xffffffff);",
            """    uint32_t crc = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)path; *p; ++p)
        crc = (crc ^ *p) * 16777619u;""")
    cut("static void video_volume_change", "static void video_set_paused")
    replace("            video_volume_change(direction);",
            "            rb->adjust_volume(direction);")
    replace("    fd = rb->open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0666);",
            """    char temporary[MAX_PATH];
    rb->snprintf(temporary, sizeof(temporary), "%s.tmp", filename);
    fd = rb->open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0666);""")
    replace("    (void)rb->write(fd, &record, sizeof(record));\n    rb->close(fd);",
            """    bool complete = rb->write(fd, &record, sizeof(record)) == sizeof(record);
    if (rb->close(fd) == 0 && complete)
        rb->rename(temporary, filename);
    rb->remove(temporary);""")
    cut("enum video_menu_item", "static void video_scale_plane")
    replace("            video_pcm_pause(true);\n", "")
    replace("            video_pcm_pause(false);\n", "")
    replace("""    case ACTION_WPS_MENU:
        return VIDEO_INPUT_MENU;""", """    case ACTION_WPS_MENU:
    case ACTION_STD_CANCEL:
        return VIDEO_INPUT_EXIT;""")
    replace("int action = rb->get_action(CONTEXT_WPS, TIMEOUT_NOBLOCK);",
            "int action = rb->get_action(CONTEXT_VIDEO_PLAYBACK, TIMEOUT_NOBLOCK);")

    for line in (
        "    rb->pcmbuf_fade(false, true);", "    rb->pcmbuf_fade(false, false);",
        "    rb->sound_set(SOUND_VOLUME, rb->global_status->volume);",
    ):
        replace(line + "\n", "")
    cut("    if (demux.audio_format == MAKEFOURCC('m', 'p', '4', 'a') &&",
        "    rb->lcd_set_background", """\
    if (rb->h264_audio && audio_codec_workspace && pcm_buffer &&
        demux.audio_codecdata_len > 0 &&
        video_audio_init(filepath, &demux, audio_codec_workspace,
                         audio_codec_workspace_size, pcm_buffer,
                         VIDEO_PCM_BUFFER) == 0)
    {
        have_audio = audio_master = true;
        video_audio_play();
        if (play_ms)
            video_audio_seek(play_ms);
    }

""")
    cut("            if (action == VIDEO_INPUT_MENU)", "            if (redraw_requested")
    replace("clock_ms = audio_master ? video_pcm_get_clock_ms() :",
            "clock_ms = audio_master ? rb->h264_audio->clock_ms() :")
    replace("(!video_audio_is_active() && video_pcm_empty())",
            "(!video_audio_is_active() && rb->h264_audio->buffered() == 0)")
    replace("    draw_width = LCD_WIDTH;",
            """    draw_width = rb->global_settings->video_fit ?
        LCD_WIDTH : MIN(width, LCD_WIDTH);""")
    replace("    draw_height = (int)((int64_t)height * LCD_WIDTH / width);",
            "    draw_height = (int)((int64_t)height * draw_width / width);")

elif src.name == "video_audio.c":
    for name in ("codec_thread.h", "video_pcm.h"):
        replace(f'#include "{name}"\n', "")
    replace("#define AUDIO_START_BUFFER_MS  1000\n", "")
    replace("static volatile bool audio_is_ready;\n", "")
    code = code.replace("    audio_is_ready = false;\n", "")
    replace("    audio_is_ready = true;\n    audio_is_playing = false;", "    audio_is_playing = false;")
    cut("        if (!audio_is_ready", "    }\n}")
    cut("bool video_audio_ready(void)", "bool video_audio_is_active(void)")
    replace("            if (value > 0)\n                audio_sample_rate = (uint32_t)value;",
            """            if (value <= 0 || (uint32_t)value != audio_sample_rate)
                audio_has_error = true;""")
    replace("    if (position < 0 || position > audio_file_size ||",
            "    if (!rb->h264_audio->running() || position < 0 || position > audio_file_size ||")
    replace("    while (done < size && video_ci.curpos < audio_file_size)",
            "    while (done < size && video_ci.curpos < audio_file_size && rb->h264_audio->running())")
    replace("while (audio_paused && audio_action == CODEC_ACTION_NULL)",
            "while (audio_paused && rb->h264_audio->running() && !audio_has_error)")
    replace("""        /* A seek is a one-shot command.  HALT deliberately remains latched
         * until codec teardown so the producer cannot resume behind us. */
""", "")
    replace("    action = audio_action;", """    if (!rb->h264_audio->running() || audio_has_error)
        return CODEC_ACTION_HALT;
    action = audio_action;""")
    for before, after in (
        ("consumed < count && audio_action != CODEC_ACTION_HALT", "consumed < count && rb->h264_audio->running() && !audio_has_error"),
        ("written < frames && audio_action != CODEC_ACTION_HALT", "written < frames && rb->h264_audio->running() && !audio_has_error"),
    ):
        replace(before, after)
    code = code.replace("while (audio_paused && audio_action != CODEC_ACTION_HALT)",
                        "while (audio_paused && rb->h264_audio->running())")
    replace("if (audio_action == CODEC_ACTION_HALT)\n            break;",
            "if (!rb->h264_audio->running())\n            break;")
    replace("            amount = video_pcm_write", "            amount = rb->h264_audio->write")
    cut("    rb->memset(&video_ci, 0, sizeof(video_ci));",
        "    video_ci.codec_get_buffer", """\
    rb->memset(&video_ci, 0, sizeof(video_ci));
""")
    cut("\n#if defined(ARM_NEED_DIV0)", "}\n\nstatic void audio_codec_worker")
    cut("#ifdef HAVE_PRIORITY_SCHEDULING\n    /* Match mpegplayer",
        "    if (status < 0", "    status = rb->h264_audio->decode_aac(&video_ci);\n")
    replace("status < 0 && audio_action != CODEC_ACTION_HALT",
            "status < 0 && rb->h264_audio->running()")
    replace("if (!video_pcm_init(audio_sample_rate, pcm_buffer, pcm_buffer_size))",
            "if (!rb->h264_audio->open(pcm_buffer, pcm_buffer_size, audio_sample_rate))")
    cut("    audio_action = CODEC_ACTION_NULL;\n    audio_paused = false;\n    audio_is_playing = true;",
        "\n}", """\
    audio_is_playing = true;
    if (!rb->h264_audio->start(audio_codec_worker))
    {
        audio_is_playing = false;
        audio_has_error = true;
    }""")
    replace("    audio_paused = true;\n}", "    audio_paused = true;\n    rb->h264_audio->pause(true);\n}")
    replace("    audio_paused = false;\n}", "    audio_paused = false;\n    rb->h264_audio->pause(false);\n}")
    cut("    if (!audio_initialized || !audio_is_playing)", "\n}",
        """\
    if (!audio_initialized || !rb->h264_audio->seek(target_ms))
    {
        audio_has_error = true;
        return;
    }

    audio_file_buffer_length = 0;
    video_ci.curpos = 0;
    video_ci.filesize = audio_file_size;
    video_id3.offset = 0;
    video_id3.elapsed = 0;
    audio_seek_target_ms = target_ms;
    audio_action = CODEC_ACTION_SEEK_TIME;
    rb->h264_audio->pause(audio_paused);
    video_audio_play();""")
    cut("    audio_action = CODEC_ACTION_HALT;\n", "    if (audio_fd >= 0)",
        "    rb->h264_audio->close();\n")

dst.write_text(code)
