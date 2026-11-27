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
                   "system.h", "video_pcm.h"):
        replace(f'#include "{header}"\n', "")
    replace("    VIDEO_INPUT_MENU,\n", "")
    replace("    uint32_t crc = rb->crc_32(path, rb->strlen(path), 0xffffffff);",
            """    uint32_t crc = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)path; *p; ++p)
        crc = (crc ^ *p) * 16777619u;""")
    cut("static void video_volume_change", "static void video_set_paused")
    replace("        video_volume_change(1);", "        rb->adjust_volume(1);")
    replace("        video_volume_change(-1);", "        rb->adjust_volume(-1);")
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
    cut("    if (action == ACTION_WPS_MENU)", "    if (action == ACTION_WPS_SKIPPREV",
        """    if (action == ACTION_WPS_MENU || action == ACTION_STD_CANCEL ||
        action == ACTION_WPS_STOP)
        return VIDEO_INPUT_EXIT;
    if (action == ACTION_WPS_BROWSE)
    {
        *overlay_until = *rb->current_tick + VIDEO_OSD_SHOW_TICKS;
        *redraw = true;
        return VIDEO_INPUT_NONE;
    }
""")

    for line in (
        "    rb->pcmbuf_fade(false, true);", "    rb->pcmbuf_fade(false, false);",
        "    rb->sound_set(SOUND_VOLUME, rb->global_status->volume);",
    ):
        replace(line + "\n", "")
    replace("""        scale_buffer == NULL ||
        (demux.audio_num_samples > 0 &&
         (audio_codec_workspace == NULL || pcm_buffer == NULL)))""",
            """        scale_buffer == NULL || decoder_size == 0)""")
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
    cut("    if (demux.audio_format", "    rb->lcd_set_foreground", """\
    if (rb->h264_audio && audio_codec_workspace && pcm_buffer &&
        demux.audio_codecdata_len > 0 &&
        video_audio_init(filepath, &demux, audio_codec_workspace,
                         audio_codec_workspace_size, pcm_buffer,
                         VIDEO_PCM_BUFFER) == 0)
    {
        have_audio = audio_master = true;
        video_audio_play();
        uint32_t position = video_pts_ms(&demux, &timing);
        if (position)
            video_audio_seek(position);
    }

""")
    cut("            if (action == VIDEO_INPUT_MENU)", "            if (redraw_requested")
    replace("                    decoder = NULL;\n", "")
    replace("                if (have_audio)\n                    video_audio_seek(pts_ms);",
            """                if (have_audio)
                    video_audio_seek(pts_ms);
                have_frame = false;
                overlay_drawn = false;
                if (paused)
                    pause_started = *rb->current_tick;""")
    replace("(int64_t)(audio_master ? video_pcm_get_clock_ms() :",
            "(int64_t)(audio_master ? rb->h264_audio->clock_ms() :")
    replace("clock_ms = audio_master ? video_pcm_get_clock_ms() :",
            "clock_ms = audio_master ? rb->h264_audio->clock_ms() :")
    replace("(uint32_t)(((*rb->current_tick) - start_tick) * 1000 / HZ);",
            """(uint32_t)((uint64_t)(unsigned long)
                    ((*rb->current_tick) - start_tick) * 1000 / HZ);""")
    replace("(!video_audio_is_active() && video_pcm_empty())",
            "(!video_audio_is_active() && rb->h264_audio->buffered() == 0)")
    replace("    else\n        video_resume_save(filepath, last_sample, demux.num_samples);",
            "    else if (result == 1)\n        video_resume_save(filepath, last_sample, demux.num_samples);")
    cut("    char volume[20];", "    if (!visible)",
        """    int top = LCD_HEIGHT - 30;
    int bar_x = 55;
    int bar_width = LCD_WIDTH - 2 * bar_x;
    int fill_width, duration_width;

""")
    cut("    rb->snprintf(volume, sizeof(volume)", "    fill_width =")
    replace("    video_time_text(duration_ms, duration, sizeof(duration));",
            """    duration[0] = '-';
    video_time_text(duration_ms > position_ms ? duration_ms - position_ms : 0,
                    duration + 1, sizeof(duration) - 1);""")
    cut("    /* Match MPEG player's raised purple OSD", "}\n\nstatic void video_draw_frame",
        """    for (int i = 0; i < 30; i++)
        video_yuv_rect(planes, 0, top + i, LCD_WIDTH, 1,
                       MAX(20, 68 - i * 3), 128, 128);
    video_yuv_rect(planes, 0, top, LCD_WIDTH, 1, 160, 128, 128);
    video_yuv_text(planes, 12, top + 9, current, 235, 128, 128);
    video_yuv_text(planes, LCD_WIDTH - duration_width - 12, top + 9,
                   duration, 235, 128, 128);
    video_yuv_rect(planes, bar_x, top + 9, bar_width, 12, 89, 128, 128);
    video_yuv_rect(planes, bar_x + 1, top + 10, bar_width - 2, 10,
                   198, 128, 128);
    video_yuv_rect(planes, bar_x + 1, top + 10, fill_width, 10,
                   102, 161, 80);
    video_draw_status_icon(planes, LCD_WIDTH / 2 - 3, top + 11, paused);
""")
    replace("    draw_width = LCD_WIDTH;", """    draw_width = rb->global_settings->video_fit ? LCD_WIDTH : MIN(width, LCD_WIDTH);""")
    replace("    draw_height = (int)((int64_t)height * LCD_WIDTH / width);",
            "    draw_height = (int)((int64_t)height * draw_width / width);")
    replace("    uint32_t last_sample = 0;", "    uint32_t last_sample = 0, resume_sample, resume_ms;")
    replace("    sample = video_resume_load(filepath, demux.num_samples);",
            """    resume_sample = video_resume_load(filepath, demux.num_samples);
    last_sample = resume_sample;
    video_timing_for_sample(&demux, resume_sample, &timing);
    resume_ms = video_pts_ms(&demux, &timing);
    sample = resume_sample;""")
    replace("(long)((uint64_t)video_pts_ms(&demux, &timing) * HZ / 1000u);",
            "(long)((uint64_t)resume_ms * HZ / 1000u);")
    replace("        uint32_t position = video_pts_ms(&demux, &timing);\n"
            "        if (position)\n            video_audio_seek(position);",
            "        if (resume_ms)\n            video_audio_seek(resume_ms);")
    replace("                pts_ms = video_pts_ms(&demux, &timing);",
            "                resume_sample = sample;\n"
            "                pts_ms = video_pts_ms(&demux, &timing);")
    replace("        if (decoded > 0)", "        if (decoded > 0 && sample >= resume_sample)")
    replace("        last_sample = sample;\n        video_timing_advance",
            "        if (sample >= resume_sample)\n            last_sample = sample;\n"
            "        video_timing_advance")
    replace("record.frame >= (int32_t)(total_samples * 95u / 100u)",
            "record.frame >= (int32_t)total_samples")
    replace("sample >= total_samples * 95u / 100u", "sample >= total_samples")
    replace("    if (result == 0)\n        video_resume_clear(filepath);",
            "    if (result == 0 && have_frame)\n        video_resume_clear(filepath);")
    replace("            size == 0 || size > VIDEO_READ_BUFFER ||",
            """            size == 0 || size > VIDEO_READ_BUFFER ||
            offset < demux.mdat_offset ||
            (uint64_t)offset + size >
                (uint64_t)demux.mdat_offset + demux.mdat_len ||""")

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
