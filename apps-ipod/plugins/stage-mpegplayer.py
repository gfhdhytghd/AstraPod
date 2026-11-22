#!/usr/bin/env python3
"""Apply AstraPod's bridge to an exact upstream MPEGPlayer snapshot."""
import hashlib
from pathlib import Path
import re
import sys

DIGESTS = {
    "decode.c": "8fdbb368dbaabc6d3421a69f24de9e770956c23c97113d8a9138cc9a485e62be",
    "alloc.c": "510bff37d0b1d5b3a6333f59630b67a5f1965b74c9b37d6325ba3a206407634a",
    "audio_thread.c": "bf58813077c3324ae13dd3259949a3ed7b528af713ba9f10e6e9e2dcf720daf1",
    "disk_buf.c": "0a06b88b8283ca431b18bbd530f81d4b422f01b27138ad556ece55850873fe94",
    "mpegplayer.h": "406840595c03c350bcba923b01660fb27e8c5749b1c33fb05ea4ce2422391c1f",
    "stream_mgr.c": "6e75a595a7cb38f050ef61c4b6cfe4fe28ee029c28c4f734c329893d9b1088a4",
    "stream_thread.h": "96fda1cc60b1ce7ca26f4d24d9ddb77050653ee8c96cb25d7c26b1c419456f42",
    "stream_mgr.h": "df2cc1834dda327bc114c63798b61cd27202030eb6cec18c12c6107df8ff640b",
    "mpeg_misc.c": "8c3abc9039ab6f864aee3df9b706e67a3c0b71dd83470fcb4cd07725958f3fe6",
    "video_out_rockbox.c": "087d3147dac00287e5cde0d56be6c3ac5fe3f9f74f534776730b83b3177cf0de",
    "video_thread.c": "50890be127b90698f01831862fc33ead0a4a8e77801d5631206945ba799493e7",
    "header.c": "cd23d651274cd7014c73a98cdbb965b6fcd2560f4af1792de9c0b9c89de4183f",
}
source, output = map(Path, sys.argv[1:])
name = source.name
data = source.read_bytes()
if hashlib.sha256(data).hexdigest() != DIGESTS[name]:
    sys.exit(f"{source}: upstream MPEGPlayer source changed")
code = data.decode()


def replace(old, new):
    global code
    if old not in code:
        sys.exit(f"{source}: staging anchor missing: {old!r}")
    code = code.replace(old, new)


def cut(begin, end, body=""):
    start = code.index(begin)
    finish = code.index(end, start + len(begin))
    replace(code[start:finish], body)


def worker(thread, stack, owner):
    """Replace an unmanaged creation with the namespaced SDK worker."""
    prefix = f"    {owner} = rb->create_thread("
    start = code.index(prefix)
    finish = code.index(";", start) + 1
    replace(code[start:finish],
            f"    {owner} = rb->mpeg->start_worker(\n"
            f"        {thread}, NULL, {stack}, sizeof({stack}));\n"
            f"    if (!{owner})\n        return false;")
    replace(f"static void {thread}(void)\n{{",
            f"static void {thread}(void *arg)\n{{\n    (void)arg;")
    replace(f"static uint32_t {stack}[",
            f"static _Alignas(long) uint32_t {stack}[")
    replace(f"rb->thread_wait({owner});", f"rb->rock_worker_join({owner});")


if name == "mpegplayer.h":
    cut("#ifdef HAVE_SCHEDULER_BOOSTCTRL", "/* Should be enough", '''
#include "mpegplayer-compat.h"

#ifdef HAVE_ADJUSTABLE_CPU_FREQ
#define trigger_cpu_boost() rb->cpu_boost(true)
#define cancel_cpu_boost() rb->cpu_boost(false)
#endif

''')
    replace("#define CLOCK_RATE 44100", "#define CLOCK_RATE (mpeg_pcm_rate())")
    replace("(2*1024*1024)", "(1280*1024)")
    cut("/* Define this as", "/** disk buffer **/")
    replace("(1024*1024)", "(256*1024)")

elif name == "alloc.c":
    replace("if ((size_t) (*mem_ptr + size) > bufsize)",
            "size = ALIGN_UP(size, 16);\n\n"
            "    if (*mem_ptr > (off_t)bufsize || size > bufsize - *mem_ptr)")
    replace("*mem_ptr += (size + 3) & ~3; /* Keep memory 32-bit aligned */",
            "*mem_ptr += size;")
    replace("CACHEALIGN_UP(4)", "CACHEALIGN_UP(16)")

elif name == "mpeg_misc.c":
    code = code[:code.index("/** System events **/")]
    replace("bool list_is_member(void **list, void *item)",
            '''static void **find_slot(void **list, const void *item)
{
    while (*list && *list != item)
        ++list;
    return list;
}

bool list_is_member(void **list, void *item)''')
    replace("rb->find_array_ptr", "find_slot")
    replace("    return rb->remove_array_ptr(list, item) != -1;",
            '''    void **slot = find_slot(list, item);
    if (!*slot)
        return false;

    do
        slot[0] = slot[1];
    while (*slot++);
    return true;''')

elif name == "stream_mgr.h":
    replace("rb->storage_spin();", "rb->mpeg->storage_keep_active();")

elif name == "stream_mgr.c":
    replace('#include "mpeg_settings.h"\n', '')
    cut("static struct queue_sender_list stream_mgr_queue_send",
        "struct stream_mgr stream_mgr", "static bool manager_ready;\n\n")
    cut("static inline intptr_t stream_mgr_send_msg(", "int str_next_data_not_ready(")
    # Run playback commands on the foreground. Only completion is asynchronous.
    replace("void stream_on_open(", "static int stream_on_open(")
    for handler in ("play", "pause", "resume", "stop", "seek"):
        replace(f"static void stream_on_{handler}(", f"static int stream_on_{handler}(")
    replace("static int stream_on_stop(bool reply)", "static int stream_on_stop(void)")
    replace("stream_on_stop(false)", "stream_on_stop()")
    replace("    stream_mgr_reply_msg(err);", "    return err;")
    cut("        /* Reply with previous state */", "        trigger_cpu_boost();")
    cut("    else\n    {\n        /* Reply with previous state */", "/* Handle STREAM_PAUSE */",
        "\n    stream_mgr_unlock();\n    return status;\n}\n\n")
    replace("    /* Reply with previous state */\n    stream_mgr_reply_msg(status);\n\n", "")
    replace("    if (reply)\n        stream_mgr_reply_msg(status);\n\n", "")
    for following in ("RESUME", "STOP", "SEEK"):
        replace(f"    stream_mgr_unlock();\n}}\n\n/* Handle STREAM_{following} */",
                f"    stream_mgr_unlock();\n    return status;\n}}\n\n/* Handle STREAM_{following} */")
    cut("        /* Have data - reply", "        stream_mgr_lock();")
    replace("        stream_mgr_unlock();\n        return;",
            "        stream_mgr_unlock();\n        return STREAM_OK;")
    replace("    stream_mgr_reply_msg(STREAM_ERROR);", "    return STREAM_ERROR;")
    cut("/* Playback control thread */", "/* Initializes the playback engine */", '''
/* A worker posts at most one completion before waiting for commands. */
void mpeg_stream_poll(void)
{
    while (!rb->mpeg->queue_empty(stream_mgr.q))
    {
        struct queue_event ev;
        rb->mpeg->queue_wait_w_tmo(stream_mgr.q, &ev, 0);
        if (ev.id == STREAM_EV_COMPLETE)
            stream_on_ev_complete((struct stream *)ev.data);
    }
}

int stream_open(const char *filename)
{
    return manager_ready ? stream_on_open(filename) : STREAM_ERROR;
}

int stream_play(void)
{
    return manager_ready ? stream_on_play() : STREAM_ERROR;
}

int stream_pause(void)
{
    return manager_ready ? stream_on_pause() : STREAM_ERROR;
}

int stream_resume(void)
{
    return manager_ready ? stream_on_resume() : STREAM_ERROR;
}

int stream_stop(void)
{
    return manager_ready ? stream_on_stop() : STREAM_ERROR;
}

int stream_seek(uint32_t time, int whence)
{
    struct stream_seek_data seek = { time, whence };
    return manager_ready ? stream_on_seek(&seek) : STREAM_ERROR;
}

int stream_close(void)
{
    return manager_ready ? stream_on_close() : STREAM_ERROR;
}

''')
    replace("    mem = rb->plugin_get_audio_buffer(&memsize);",
            '''    mem = rb->plugin_claim_scratch(2 * 1024 * 1024,
                                   3 * 1024 * 1024, &memsize);
    if (!mem)
        return STREAM_ERROR;''')
    cut("    stream_mgr.thread = rb->create_thread", "    /* Initialise our malloc buffer */")
    replace('        rb->splash(HZ, "Out of memory in stream_init");', '        return STREAM_ERROR;')
    code = re.sub(r'        rb->splash\(HZ, "(?:Could not initialize PCM|Cannot create audio thread|Cannot create video thread|Cannot create buffering thread|Parser init failed)!?"\);',
                  '        return STREAM_ERROR;', code)
    replace("        return STREAM_OK;\n    }\n\n    return STREAM_ERROR;\n}",
            "        manager_ready = true;\n        return STREAM_OK;\n    }\n}")
    cut("    if (stream_mgr.thread != 0)\n", "#ifndef HAVE_LCD_COLOR",
        "    manager_ready = false;\n\n")
    # An EOS posted by an old decode must not stop the new seek epoch.
    replace("        wb = disk_buf_send_msg(STREAM_STOP, 0);",
            '''        wb = disk_buf_send_msg(STREAM_STOP, 0);
        while (!rb->mpeg->queue_empty(stream_mgr.q))
        {
            struct queue_event ev;
            rb->mpeg->queue_wait_w_tmo(stream_mgr.q, &ev, 0);
        }''')

elif name == "audio_thread.c":
    replace('    unsigned samplerate;    /* Current stream sample rate */\n'
            '    int nchannels;          /* Number of audio channels */\n'
            "    struct dsp_config *dsp; /* The DSP we're using */\n"
            "    struct dsp_buffer src;  /* Current audio data for DSP processing */",
            "    struct mpeg_resampler resampler;")
    replace("/* The audio thread is stolen from the core codec thread */",
            "static _Alignas(long) unsigned char audio_stack[MPEG_WORKER_STACK_SIZE];")
    replace("            td->samplerate = 0;\n            td->nchannels = 0;",
            "            td->resampler = (struct mpeg_resampler){0};")
    replace("static void audio_thread(void)\n{", "static void audio_thread(void *arg)\n{\n    (void)arg;")
    cut("#ifdef HAVE_PRIORITY_SCHEDULING", "    rb->memset(&td,")
    cut("    td.dsp = rb->dsp_get_config", "    goto message_wait;")
    cut("#ifdef HAVE_PRIORITY_SCHEDULING", "                return;")
    replace("                pcm_output_drain();\n", "")
    # The EOS loop may already have dequeued a command such as pause.
    replace("                if (td.ev.id != SYS_TIMEOUT)\n                    break;",
            "                if (td.ev.id != SYS_TIMEOUT)\n                    goto message_process;")
    cut("        /** Output **/", "    } /* end decoding loop */", '''
        td.state = TSTATE_RENDER_WAIT;
    render_wait:
        while (!mpeg_pcm_produce(&synth.pcm, &audio_queue.curr->time,
                                 &td.resampler))
        {
            str_get_msg_w_tmo(&audio_str, &td.ev, 1);
            if (td.ev.id != SYS_TIMEOUT)
                goto message_process;
        }
        rb->yield();
''')
    replace("    /* We steal the codec thread for audio */\n"
            "    rb->codec_thread_do_callback(audio_thread, &audio_str.thread);",
            '''    audio_str.thread = rb->mpeg->start_worker(
        audio_thread, NULL, audio_stack, sizeof(audio_stack));
    if (!audio_str.thread)
        return false;''')
    replace("        rb->codec_thread_do_callback(NULL, NULL);",
            "        rb->rock_worker_join(audio_str.thread);")

elif name == "disk_buf.c":
    worker("disk_buf_thread", "disk_buf_stack", "disk_buf.thread")
    replace("static _Alignas(long) uint32_t disk_buf_stack[DEFAULT_STACK_SIZE*2/sizeof(uint32_t)];",
            "static _Alignas(long) unsigned char disk_buf_stack[MPEG_WORKER_STACK_SIZE];")
    replace("rb->storage_sleep()", "rb->mpeg->storage_idle()")
    replace("    if (disk_buf_is_data_ready(sh, 0))\n    {\n        /* Remove",
            "    if (disk_buf.state == TSTATE_EOS || disk_buf_is_data_ready(sh, 0))\n"
            "    {\n        /* Remove")
    replace("        ssize_t filesize = rb->ffilesize(fd);", "        off_t filesize = rb->ffilesize(fd);")
    replace("        if (filesize <= 0)\n        {\n            rb->close(disk_buf.in_file);",
            "        if (filesize <= 0 || filesize > INT32_MAX - 4*MIN_BUFAHEAD)\n"
            "        {\n            rb->close(fd);\n            fd = -1;")
    replace("    if (disk_buf.start == NULL)",
            "    if (disk_buf.start == NULL ||\n"
            "        disk_buf.size < DISK_GUARDBUF_SIZE + 4*MIN_BUFAHEAD)")
    replace("if (length > (size_t)disk_buf.size)\n    {\n        length = disk_buf.size;",
            "size_t capacity = disk_buf.size - (start & DISK_BUF_PAGE_MASK);\n"
            "    if (length > capacity)\n    {\n        length = capacity;")
    replace("    if (disk_buf.thread == 0)\n        return false;\n\n", "")

elif name == "video_thread.c":
    worker("video_thread", "video_stack", "video_str.thread")
    replace("#define VIDEO_STACKSIZE (4*1024)", "#define VIDEO_STACKSIZE MPEG_WORKER_STACK_SIZE")
    replace('#include "mpeg_settings.h"\n', '')
    cut("#if defined(DEBUG) || defined(SIMULATOR)\nstatic unsigned char pic_coding_type_char(", "\n/*")
    # Keep the stock defaults: time frames and skip late frames.
    cut("                /* Settings may have changed", "                /* */",
        "                td->stream_time = TICKS_TO_TS(stream_get_time());\n")
    cut("            if (!settings.limitfps)", "            td.goal_time = td.frame_time;")
    start = code.index("            if (!settings.skipframes)")
    end = code.index("\n            }", start) + len("\n            }")
    replace(code[start:end], "")
    replace("    if (video_str.thread == 0)\n        return false;\n\n", "")

elif name == "video_out_rockbox.c":
    replace("rb->lcd_blit_yuv(", "mpeg_yuv_blit(")

elif name == "decode.c":
    replace("(1194 * 1024)", "(512 * 1024)")

elif name == "header.c":
    # Limit all sequence headers, including replacements mid-stream.
    replace("    sequence->width = (sequence->picture_width + 15) & ~15;",
            '''    if (sequence->picture_width > 352 || sequence->picture_height > 288)
        return 1;

    sequence->width = (sequence->picture_width + 15) & ~15;''')
    replace("    sequence->frame_period = frame_period[buffer[3] & 15];",
            "    sequence->frame_period = frame_period[buffer[3] & 15];\n"
            "    if (!sequence->frame_period)\n        return 1;")
    replace("    if (x)\n        sequence->display_width = x;",
            "    if (x && x <= (int)sequence->picture_width)\n"
            "        sequence->display_width = x;")
    replace("    if (x)\n        sequence->display_height = x;",
            "    if (x && x <= (int)sequence->picture_height)\n"
            "        sequence->display_height = x;")
    replace("    case 0:        /* invalid */\n        return 1;",
            "    default:       /* Only 4:2:0 fits this output path */\n        return 1;")
    replace("        /* fallthrough */\n    case 4:        /* 4:2:2 */\n        sequence->chroma_width >>= 1;",
            "        sequence->chroma_width >>= 1;\n        break;")

if name != "header.c":
    code = re.sub(r"rb->(?=(?:queue_\w+|mutex_\w+)\b)", "rb->mpeg->", code)
output.write_text(code)
