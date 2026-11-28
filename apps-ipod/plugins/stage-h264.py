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
    replace('enum plugin_status plugin_start', """\
#ifdef HAVE_HW_H264
int video_h264_probe(const char *path, void *buffer, size_t size,
                     unsigned limit);
void video_log_budget(const char *path, size_t needed);
#endif

enum plugin_status plugin_start""")
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
        if (!request->probe)
            rb->splash(2 * HZ, "Unsupported MP4/M4V");
        return request->probe ? PLUGIN_ERROR : PLUGIN_OK;
    }
    buffer = rb->plugin_claim_scratch(needed, needed, &buffer_size);
    if (!buffer && !rb->plugin_usb_taken())
        video_log_budget(path, needed);
#else
    buffer = NULL;
#endif
""")
    replace("    rb->plugin_release_audio_buffer();\n", "")
    replace('        rb->splash(HZ * 2, "Not enough video memory");',
            '        if (!request->probe)\n'
            '            rb->splash(HZ * 2, "Not enough video memory");')
    replace('    result = video_h264_play(path, buffer, buffer_size);',
            '''#ifdef HAVE_HW_H264
    if (request->probe)
    {
        request->result = video_h264_probe(path, buffer, buffer_size,
                                           request->probe_limit);
        return request->result < 0 ? PLUGIN_ERROR : PLUGIN_OK;
    }
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
        .control_centre = request->control_centre,
        .picture_mode = request->picture_mode,
        .stock_osd = request->stock_osd,
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
        return VIDEO_INPUT_EXIT;
    case ACTION_WPS_QUICKSCREEN:
    {
        if (!osd->options || !osd->options->control_centre)
            break;
        bool resume = !*paused;
        long opened = *rb->current_tick;
        video_set_paused(true, paused, start_tick, pause_started,
                         have_audio, cpu_boosted, osd);
        if (!osd->options->control_centre())
            return VIDEO_INPUT_USB;
        long elapsed = *rb->current_tick - opened;
        if (osd->mode != VIDEO_OVERLAY_HIDDEN)
            osd->until += elapsed;
        if (osd->pending_seek)
            osd->seek_until += elapsed;
        if (resume)
            video_set_paused(false, paused, start_tick, pause_started,
                             have_audio, cpu_boosted, osd);
        *redraw = true;
        break;
    }""")
    replace("int action = rb->get_action(CONTEXT_WPS, TIMEOUT_NOBLOCK);",
            "int action = rb->get_action(CONTEXT_VIDEO_PLAYBACK, TIMEOUT_NOBLOCK);")
    # Inline remote codes bypass the action keymap. Keep their volume keys
    # independent of the wheel, which also scrubs and changes picture scaling.
    replace("    case ACTION_WPS_VOLUP:\n", """\
    case BUTTON_MULTIMEDIA_VOLUME_UP:
    case BUTTON_MULTIMEDIA_VOLUME_UP | BUTTON_REPEAT:
    case BUTTON_MULTIMEDIA_VOLUME_DOWN:
    case BUTTON_MULTIMEDIA_VOLUME_DOWN | BUTTON_REPEAT:
        rb->adjust_volume((action & ~BUTTON_REPEAT) ==
                          BUTTON_MULTIMEDIA_VOLUME_UP ? 1 : -1);
        video_osd_show(osd, VIDEO_OVERLAY_VOLUME);
        *redraw = true;
        break;
    case ACTION_WPS_VOLUP:
""")
    replace("    case ACTION_WPS_SKIPPREV:\n    case ACTION_WPS_SKIPNEXT:",
            """    case BUTTON_MULTIMEDIA_PREV:
    case ACTION_WPS_SKIPPREV:
    case BUTTON_MULTIMEDIA_NEXT:
    case ACTION_WPS_SKIPNEXT:""")
    replace("        bool forward = action == ACTION_WPS_SKIPNEXT;",
            """        bool forward = action == ACTION_WPS_SKIPNEXT ||
                       action == BUTTON_MULTIMEDIA_NEXT;""")
    replace("    case ACTION_WPS_PLAY:", """    case BUTTON_MULTIMEDIA_PLAYPAUSE:
    case ACTION_WPS_PLAY:""")

    for line in (
        "    rb->pcmbuf_fade(false, true);", "    rb->pcmbuf_fade(false, false);",
        "    rb->sound_set(SOUND_VOLUME, rb->global_status->volume);",
    ):
        replace(line + "\n", "")
    cut("    if (demux.audio_format == MAKEFOURCC('m', 'p', '4', 'a') &&",
        "    rb->lcd_set_background", """\
    if (demux.audio_format == MAKEFOURCC('m', 'p', '4', 'a'))
    {
        int audio_code = -1;
        if (rb->h264_audio && audio_codec_workspace && pcm_buffer &&
            demux.audio_codecdata_len > 0)
            audio_code = video_audio_init(filepath, &demux, audio_codec_workspace,
                                          audio_codec_workspace_size, pcm_buffer,
                                          VIDEO_PCM_BUFFER);
        if (audio_code == 0)
        {
            have_audio = audio_master = true;
            video_audio_output_pause(true);
            if (play_ms)
                video_audio_seek(play_ms);
            else
                video_audio_play();
        }
        else
            video_log_event(filepath, "aac-init", audio_code, play_ms);
    }

""")
    cut("        if (action == VIDEO_INPUT_MENU)", "        if (redraw_requested")
    replace("return audio_master ? video_pcm_get_clock_ms() :",
            "return audio_master ? rb->h264_audio->clock_ms() :")
    replace("(!video_audio_is_active() && video_pcm_empty())",
            "(!video_audio_is_active() && rb->h264_audio->buffered() == 0)")
    replace('#include "plugin.h"',
            '#include "plugin.h"\n#include "plugins/rock_video_log.h"\n'
            '#include "settings/video_picture.h"\n'
            '#define VIDEO_SYNC_LOG ROCK_VIDEO_SYNC_LOG\n'
            '#define VIDEO_ROCK_WORKER')
    replace("size_t video_h264_scratch_size(const char *path)", r"""
static int video_log_open(const char *path, const char *stage, int code)
{
    const char *log = rb->strncmp(path, "/Videos/_probe/", 15) == 0 ?
                      "/Videos/_probe/results.log" : ROCK_VIDEO_LOG;
    int fd = rb->open(log, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd >= 0)
        rb->fdprintf(fd, "\n%s stage=%s code=%d\n", path, stage, code);
    return fd;
}

static void video_log_event(const char *path, const char *stage, int code,
                            uint32_t position_ms)
{
    int fd = video_log_open(path, stage, code);
    if (fd >= 0)
    {
        rb->fdprintf(fd, "position_ms=%u\n", (unsigned)position_ms);
        rb->close(fd);
    }
}

static void video_log_layout(int fd, size_t decoder_size, size_t audio_size)
{
    rb->fdprintf(fd, "avc=%ux%u coded=%ux%u profile=%u level=%u avcC=%u "
                 "sps=%u pps=%u nal_length=%u mdat=%u+%u\n",
                 demux.width, demux.height, (demux.width + 15) & ~15,
                 (demux.height + 15) & ~15, demux.avc_profile, demux.avc_level,
                 (unsigned)demux.codecdata_len, demux.num_sps, demux.num_pps,
                 demux.nalu_len_size, (unsigned)demux.mdat_offset,
                 (unsigned)demux.mdat_len);
    rb->fdprintf(fd, "video samples=%u stsz_default=%u stts=%u stsc=%u "
                 "chunks=%u chunk_words=%u stss=%u window_words=%u "
                 "table_words(stsz/stsc/chunks/stss)=%u/%u/%u/%u\n",
                 (unsigned)demux.num_samples, (unsigned)demux.default_sample_size,
                 (unsigned)demux.num_stts, (unsigned)demux.num_stsc,
                 (unsigned)demux.num_stco, (unsigned)demux.chunk_words,
                 (unsigned)demux.num_stss, MP4V_WINDOW_WORDS,
                 (unsigned)demux.sample_sizes.words, (unsigned)demux.stsc.words,
                 (unsigned)demux.chunk_offsets.words,
                 (unsigned)demux.sync_samples.words);
    rb->fdprintf(fd, "audio samples=%u chunks=%u stsc=%u rate=%u trim=%u\n"
                 "bytes decoder=%lu sample=%lu scale=%lu aac=%lu pcm=%lu\n",
                 (unsigned)demux.audio_num_samples, (unsigned)demux.audio_num_stco,
                 (unsigned)demux.audio_num_stsc, (unsigned)demux.audio_sample_rate,
                 (unsigned)demux.audio_lead_trim, (unsigned long)decoder_size,
                 (unsigned long)VIDEO_READ_BUFFER, (unsigned long)VIDEO_OUTPUT_BUFFER,
                 (unsigned long)audio_size,
                 audio_size ? (unsigned long)VIDEO_PCM_BUFFER : 0ul);
}

void video_log_budget(const char *path, size_t needed)
{
    int fd = video_log_open(path, "budget", -1);
    if (fd < 0)
        return;
    rb->fdprintf(fd, "requested=%lu granted=0\n", (unsigned long)needed);
    video_log_layout(fd, rb->hw_h264->buf_size((demux.width + 15) & ~15,
                                               (demux.height + 15) & ~15),
                     video_audio_workspace_size(&demux));
    rb->close(fd);
}

static const char *video_read_header(int fd)
{
    if (fd < 0)
        return "open";
    rb->memset(&demux, 0, sizeof(demux));
    if (mp4v_demux_read(fd, &demux) < 0)
        return "demux";
    if (demux.format != MAKEFOURCC('a', 'v', 'c', '1'))
        return "avc-sample-entry";
    if (demux.avc_profile != 66)
        return "avc-profile";
    if (demux.avc_level > VIDEO_MAX_LEVEL)
        return "avc-level";
    if (demux.width <= 0 || demux.height <= 0 ||
        demux.width > VIDEO_MAX_WIDTH || demux.height > VIDEO_MAX_HEIGHT)
        return "video-size";
    if (!video_format_supported(&demux))
        return "avc-nal-length";
    if (!video_tables_sane(&demux))
        return "table-counts";
    return NULL;
}

size_t video_h264_scratch_size(const char *path)""")
    replace("""    if (fd < 0)
        return 0;
    int result = mp4v_demux_read(fd, &demux);
    rb->close(fd);
    if (result < 0 || !video_format_supported(&demux) ||
        !video_tables_sane(&demux))
        return 0;""", """    const char *failure = video_read_header(fd);
    if (fd >= 0)
        rb->close(fd);
    if (failure)
    {
        int log = video_log_open(path, failure, -1);
        if (log >= 0)
        {
            if (fd >= 0)
                video_log_layout(log, 0, 0);
            rb->close(log);
        }
        return 0;
    }""")
    replace("""    if (!decoder)
        return 0;""", """    if (!decoder)
    {
        int log = video_log_open(path, "decoder-size", -1);
        if (log >= 0)
        {
            video_log_layout(log, 0, 0);
            rb->close(log);
        }
        return 0;
    }""")
    replace("""int video_h264_play_named(const char *filepath, void *buffer,
                          size_t buffer_size,
                          const struct video_play_options *options)""",
            """static int video_run(const char *filepath, void *buffer,
                     size_t buffer_size, const struct video_play_options *options,
                     bool probe, unsigned probe_limit)""")
    replace("""    size_t decoder_size;
    size_t audio_codec_workspace_size;""",
            """    size_t decoder_size = 0;
    size_t audio_codec_workspace_size = 0;
    uint32_t offset = 0, size = 0;
    int code = -1;
    const char *failure = NULL;""")
    replace('    if (!options)\n        rb->splash(0, "Loading video...");',
            '    if (!options && !probe)\n        rb->splash(0, "Loading video...");')
    cut("    if (video_fd < 0 || mp4v_demux_read", "    pool.cursor", """\
    failure = video_read_header(video_fd);
    if (failure)
    {
        if (!probe)
            rb->splash(HZ * 2, "Unsupported MP4/M4V");
        goto cleanup;
    }

""")
    for text, stage in (("Not enough video memory", "pool-layout"),
                        ("MP4 table parse failed", "index"),
                        ("Unsupported movie duration", "duration")):
        replace(f'        rb->splash(HZ * 2, "{text}");',
                f'        failure = "{stage}";\n'
                f'        if (!probe)\n            rb->splash(HZ * 2, "{text}");')
    replace('    if (!options)\n        rb->splash(0, "Loading movie index...");',
            '    if (!options && !probe)\n        rb->splash(0, "Loading movie index...");')
    replace('    show_sample = video_resume_load(filepath, demux.num_samples);',
            '    show_sample = probe ? 0 : video_resume_load(filepath, demux.num_samples);')
    replace("""    if (!mp4v_find_keyframe(video_fd, &demux, sample, &sample))
        goto cleanup;""", """    if (!mp4v_find_keyframe(video_fd, &demux, sample, &sample))
    {
        failure = "initial-keyframe";
        goto cleanup;
    }""")
    replace('    if (!options)\n        rb->splash(0, "Starting video...");',
            '    if (!options && !probe)\n        rb->splash(0, "Starting video...");')
    cut("    int configure_code = decoder", "    start_tick =", r"""    code = decoder ? rb->hw_h264->configure(decoder, demux.codecdata,
                                            demux.codecdata_len) : -1;
    if (code < 0)
    {
        failure = decoder ? "decoder-configure" : "decoder-open";
        if (!probe)
        {
            char message[64];
            rb->splash(HZ * 2, decoder ?
                       video_decoder_reason(code, message, sizeof(message)) :
                       "Cannot open H.264 decoder");
        }
        goto cleanup;
    }
    if (probe)
    {
        int log = video_log_open(filepath, "decoder-configure", code);
        if (log < 0)
            goto cleanup;
        video_log_layout(log, decoder_size, audio_codec_workspace_size);
        rb->fdprintf(log, "pool granted=%lu used=%lu\n",
                     (unsigned long)buffer_size,
                     (unsigned long)(pool.cursor - (uint8_t *)buffer));
        struct mp4v_read_window read = {read_buffer, VIDEO_READ_BUFFER, 0, 0};
        unsigned frames = 0;
        unsigned a = 1, b = 0;
        uint32_t limit = demux.num_samples;
        if (probe_limit && probe_limit < limit - sample)
            limit = sample + probe_limit;
        for (; sample < limit; sample++)
        {
            offset = size = 0;
            const uint8_t *compressed;
            code = mp4v_read_sample(video_fd, &demux, sample, &read,
                                    &compressed, &offset, &size);
            if (code < 0)
            {
                failure = "sample-read";
                break;
            }
            code = rb->hw_h264->decode_sample(decoder, compressed, size,
                                              demux.nalu_len_size);
            if (code > 0)
            {
                const uint8_t *planes[3];
                int w, h, stride;
                rb->hw_h264->get_frame(decoder, &planes[0], &planes[1],
                                       &planes[2], &w, &h, &stride);
                for (int plane = 0; plane < 3; plane++)
                {
                    int shift = plane != 0;
                    for (int row = 0; row < (h >> shift); row++)
                    {
                        const uint8_t *pixels = planes[plane] +
                                                row * (stride >> shift);
                        for (int col = 0; col < (w >> shift); col++)
                        {
                            a += pixels[col];
                            b += a;
                        }
                        a %= 65521;
                        b %= 65521;
                    }
                }
                frames++;
            }
            if (code < 0)
            {
                failure = "sample-decode";
                break;
            }
        }
        rb->fdprintf(log, "samples=%u frames=%u decode=%d adler32=%08lx\n",
                     (unsigned)sample, frames, code,
                     (unsigned long)((b << 16) | a));
        rb->close(log);
        if (!failure && frames)
            result = frames;
        goto cleanup;
    }

""")
    replace("""            if (!mp4v_find_keyframe(video_fd, &demux, show_sample, &sample))
                goto cleanup;""",
            """            if (!mp4v_find_keyframe(video_fd, &demux, show_sample, &sample))
            {
                failure = "seek-keyframe";
                code = -1;
                goto cleanup;
            }""")
    replace("""            if (decoder == NULL ||
                rb->hw_h264->configure(decoder, demux.codecdata,
                                   demux.codecdata_len) < 0)
            {""",
            """            code = decoder ? rb->hw_h264->configure(
                decoder, demux.codecdata, demux.codecdata_len) : -1;
            if (code < 0)
            {
                failure = decoder ? "seek-configure" : "seek-open";""")
    replace("""            video_audio_stop();
            have_audio = false;""", """            if (video_audio_failed())
                video_log_event(filepath, "aac-decode", -1, clock_ms);
            video_audio_stop();
            have_audio = false;""")
    replace('        rb->splash(HZ * 2, "Video worker failed");',
            '        failure = "worker-start";\n'
            '        code = -1;\n'
            '        rb->splash(HZ * 2, "Video worker failed");')
    replace("        if (pipe.error)\n        {", """        if (pipe.error)
        {
            failure = pipe.read_failed ? "sample-read" : "sample-decode";
            code = pipe.error;
            sample = pipe.sample;
            offset = pipe.offset;
            size = pipe.size;""")
    replace("cleanup:\n", r"""cleanup:
    if (failure)
    {
        if (probe && code < 0)
            result = code;
        int log = video_log_open(filepath, failure, code);
        if (log >= 0)
        {
            if ((!rb->strcmp(failure, "decoder-configure") ||
                 !rb->strcmp(failure, "sample-decode")) && code < 0)
            {
                char message[64];
                rb->fdprintf(log, "%s\n",
                             video_decoder_reason(code, message, sizeof(message)));
            }
            rb->fdprintf(log, "pool granted=%lu sample=%u offset=%u bytes=%u\n",
                         (unsigned long)buffer_size, (unsigned)sample,
                         (unsigned)offset, (unsigned)size);
            if (decoder_size)
                video_log_layout(log, decoder_size, audio_codec_workspace_size);
            rb->close(log);
        }
    }
""")
    replace("""    rb->backlight_set_timeout(rb->global_settings->backlight_timeout);
    if (!options)""",
            """    if (!probe)
        rb->backlight_set_timeout(rb->global_settings->backlight_timeout);
    if (!options && !probe)""")
    replace("int video_h264_play(const char *filepath,", """\
int video_h264_probe(const char *path, void *buffer, size_t size,
                     unsigned limit)
{
    return video_run(path, buffer, size, NULL, true, limit);
}

int video_h264_play_named(const char *path, void *buffer, size_t size,
                          const struct video_play_options *options)
{
    return video_run(path, buffer, size, options, false, 0);
}

int video_h264_play(const char *filepath,""")

    replace("    VIDEO_OVERLAY_VOLUME,",
            "    VIDEO_OVERLAY_VOLUME,\n    VIDEO_OVERLAY_PICTURE,\n"
            "    VIDEO_OVERLAY_BRIGHTNESS,")
    replace("    enum video_overlay mode;",
            "    enum video_overlay mode;\n    int picture_mode;")
    replace("    bool stock = osd->options != NULL;",
            "    bool stock = osd->options != NULL;\n"
            "    bool stock_osd = stock && osd->options->stock_osd;")
    replace("""static enum video_input_action video_input(bool *paused, long *start_tick,""",
            """static const char *video_picture_label(const struct video_osd *osd)
{
    static const char *const labels[] = {"Fit", "Fill", "Stretch"};
    return labels[osd->picture_mode];
}

#include "plugins/video_stock_input.h"

static enum video_input_action video_input(bool *paused, long *start_tick,""")
    replace("""    case ACTION_NONE:
        if (osd->pending_seek &&""",
            """    case ACTION_NONE:
        if (stock_osd && video_stock_moving && *paused)
            *redraw = true;
        if (osd->pending_seek &&""")
    replace("""    case ACTION_WPS_BROWSE:
    {
        enum video_overlay mode = VIDEO_OVERLAY_PROGRESS;""",
            """    case ACTION_WPS_BROWSE:
    {
        if (stock_osd)
        {
            *redraw = true;
            return video_stock_select(osd);
        }
        enum video_overlay mode = VIDEO_OVERLAY_PROGRESS;""")
    replace("    struct video_osd osd = {.options = options, .title = title};",
            """    struct video_osd osd = {
        .options = options,
        .title = title,
        .picture_mode = options && options->picture_mode ?
            options->picture_mode(0) : VIDEO_PICTURE_FIT,
    };""")
    replace("""        if (osd->mode == VIDEO_OVERLAY_SCRUB)
            mode = VIDEO_OVERLAY_HIDDEN;""",
            """        if (osd->mode == VIDEO_OVERLAY_SCRUB)
            mode = stock && osd->options->picture_mode ?
                VIDEO_OVERLAY_PICTURE : VIDEO_OVERLAY_HIDDEN;
        if (osd->mode == VIDEO_OVERLAY_PICTURE)
            mode = VIDEO_OVERLAY_HIDDEN;""")
    replace("""        *redraw = true;
        if (osd->mode == VIDEO_OVERLAY_SCRUB)
        {""",
            """        *redraw = true;
        if (osd->mode == VIDEO_OVERLAY_PICTURE)
        {
            osd->picture_mode = osd->options->picture_mode(direction);
            video_osd_show(osd, VIDEO_OVERLAY_PICTURE);
        }
        else if (osd->mode == VIDEO_OVERLAY_BRIGHTNESS)
        {
            osd->options->stock_osd->brightness(direction);
            video_osd_show(osd, VIDEO_OVERLAY_BRIGHTNESS);
        }
        else if (osd->mode == VIDEO_OVERLAY_SCRUB)
        {""")
    replace("""            video_preview_seek(osd, position, duration, direction * step,
                               HZ / 3);""",
            """            long delay = stock_osd ? HZ / 2 : HZ / 3;
            video_preview_seek(osd, position, duration, direction * step,
                               delay);""")
    replace("""        video_osd_show(osd, VIDEO_OVERLAY_PROGRESS);
        *redraw = true;
        break;
    default:""",
            """        if (!stock_osd)
            video_osd_show(osd, VIDEO_OVERLAY_PROGRESS);
        else if (osd->mode != VIDEO_OVERLAY_HIDDEN)
            video_osd_show(osd, osd->mode);
        *redraw = true;
        break;
    default:""")
    replace("""    video_draw_overlay(planes, paused, position_ms, duration_ms,
                       osd);
    bool overlay""",
            """    bool stock = osd->options && osd->options->stock_osd;
    if (!stock)
        video_draw_overlay(planes, paused, position_ms, duration_ms, osd);
    bool overlay""")
    replace("""    if (rb->hw_h264->present)
    {
        struct h264_lcd_timing timing = {0};
        rb->hw_h264->present(planes, &timing);
        if (timing.submitted)
        {
            video_measure(overlay ? STAGE_LCD_PREP_OSD : STAGE_LCD_PREP,
                          timing.submit_us - submitted);
            video_measure(overlay ? STAGE_LCD_OSD : STAGE_LCD,
                          timing.complete_us - timing.submit_us);
        }
    }
    else
        rb->lcd_blit_yuv(planes, 0, 0, LCD_WIDTH, 0, 0, LCD_WIDTH, LCD_HEIGHT);""",
            """    struct h264_lcd_timing timing = {0};
    if (stock)
        video_stock_present(planes, paused, position_ms, duration_ms, osd, &timing);
    else if (rb->hw_h264->present)
        rb->hw_h264->present(planes, &timing);
    else
        rb->lcd_blit_yuv(planes, 0, 0, LCD_WIDTH, 0, 0, LCD_WIDTH, LCD_HEIGHT);
    if (timing.submitted)
    {
        video_measure(overlay ? STAGE_LCD_PREP_OSD : STAGE_LCD_PREP,
                      timing.submit_us - submitted);
        video_measure(overlay ? STAGE_LCD_OSD : STAGE_LCD,
                      timing.complete_us - timing.submit_us);
    }""")
    replace("""        rb->snprintf(right_text, sizeof(right_text), "%d%%", percent);
    }
    else""",
            """        rb->snprintf(right_text, sizeof(right_text), "%d%%", percent);
    }
    else if (osd->mode == VIDEO_OVERLAY_PICTURE)
    {
        fill = (osd->picture_mode + 1) * width / VIDEO_PICTURE_COUNT;
        rb->snprintf(left_text, sizeof(left_text), "Picture Scaling");
        rb->snprintf(right_text, sizeof(right_text), "%s",
                     video_picture_label(osd));
    }
    else""")
    replace("""        video_yuv_text(planes, left, bottom + 7, right, "Volume",
                       241, 245, 245);
    }
    else""",
            """        video_yuv_text(planes, left, bottom + 7, right, "Volume",
                       241, 245, 245);
    }
    else if (osd->mode == VIDEO_OVERLAY_PICTURE)
    {
        const char *label = video_picture_label(osd);
        fill = (osd->picture_mode + 1) * bar_width / VIDEO_PICTURE_COUNT;
        video_yuv_text(planes, left, bottom + 7, right, "Picture Scaling",
                       241, 245, 245);
        video_yuv_text(planes, right - video_yuv_text_width(label),
                       bottom + 7, right, label, 241, 245, 245);
    }
    else""")
    cut("    draw_width = LCD_WIDTH;", "    draw_width &= ~1;", """\
    draw_width = LCD_WIDTH;
    draw_height = LCD_HEIGHT;
    if (osd->picture_mode == VIDEO_PICTURE_FILL)
    {
        /* Crop on even pixel boundaries for the subsampled chroma planes. */
        int crop_w = MIN(width, height * LCD_WIDTH / LCD_HEIGHT) & ~1;
        int crop_h = MIN(height, width * LCD_HEIGHT / LCD_WIDTH) & ~1;
        int crop_x = ((width - crop_w) / 2) & ~1;
        int crop_y = ((height - crop_h) / 2) & ~1;
        y += crop_y * stride + crop_x;
        cb += crop_y / 2 * (stride / 2) + crop_x / 2;
        cr += crop_y / 2 * (stride / 2) + crop_x / 2;
        width = crop_w;
        height = crop_h;
    }
    else if (osd->picture_mode != VIDEO_PICTURE_STRETCH)
    {
        draw_height = height * LCD_WIDTH / width;
        if (draw_height > LCD_HEIGHT)
        {
            draw_height = LCD_HEIGHT;
            draw_width = width * LCD_HEIGHT / height;
        }
    }
""")

elif src.name == "video_audio.c":
    for name in ("codec_thread.h", "video_pcm.h"):
        replace(f'#include "{name}"\n', "")
    replace("#define AUDIO_START_BUFFER_MS  1000\n", "")
    replace("static volatile bool audio_is_ready;\n", "")
    code = code.replace("    audio_is_ready = false;\n", "")
    replace("    audio_is_ready = true;\n    audio_is_playing = false;", "    audio_is_playing = false;")
    cut("        if (!audio_is_ready", "    }\n}")
    replace("""bool video_audio_ready(void)
{
    return audio_is_ready || audio_has_error;
}""", """bool video_audio_ready(void)
{
    return rb->h264_audio->buffered() >= audio_sample_rate / 8 ||
           !audio_is_playing;
}""")
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
        ("(audio_gap || consumed < count) && audio_action != CODEC_ACTION_HALT", "(audio_gap || consumed < count) && rb->h264_audio->running() && !audio_has_error"),
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
    replace("    video_pcm_pause(pause);", "    rb->h264_audio->pause(pause);")
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
    replace("    audio_paused = false;\n}", """    audio_paused = false;
    if (!audio_hold_output)
        rb->h264_audio->pause(false);
}""")
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
    audio_seek_target_ms = audio_edit_seek(target_ms);
    audio_action = audio_seek_target_ms ? CODEC_ACTION_SEEK_TIME :
                                         CODEC_ACTION_NULL;
    audio_paused = false;
    video_audio_output_pause(true);
    video_audio_play();""")
    cut("    audio_action = CODEC_ACTION_HALT;\n", "    if (audio_fd >= 0)",
        "    rb->h264_audio->close();\n")

dst.write_text(code)
