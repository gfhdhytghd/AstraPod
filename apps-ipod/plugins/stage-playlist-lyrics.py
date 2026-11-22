#!/usr/bin/env python3
"""Stage foreground CUE export and Lrcplayer adapters."""
from hashlib import sha256
from pathlib import Path
import sys


DIGESTS = {
    "cue_playlist": "86886077a7114bd71378b8fd3fc6a0f7703cb943c4eff2b2d2b3381c53cfcd81",
    "lrcplayer": "3d59456f8d5d7d54d51a17bfa1de8e4e7b2634f577e6308ae100321c6216c4e6",
}


def replace(text, before, after):
    if text.count(before) != 1:
        raise ValueError(f"expected one upstream match for: {before!r}")
    return text.replace(before, after)


def cue(text, reader, adapter):
    header, _ = text.split('#include "plugin.h"', 1)
    start = text.index("static uint32_t write_metadata_tags")
    end = text.index("static bool current_playlist_filename_cb", start)
    metadata = text[start:end]
    begin = metadata.index("#define PERFORMER_TITLE_SZ")
    end = metadata.index('    cps_printf(TDINDENT "INDEX', begin)
    metadata = metadata[:begin] + '''    cps_printf(TDINDENT "PERFORMER ");
    cue_quoted(performer);
    cps_printf("\\n" TDINDENT "TITLE ");
    cue_quoted(title);
    cps_printf("\\n" TDINDENT "SIZE_INFO %lu\\n", id3->filesize);
    if (ISVALID(id3->composer))
    {
        cps_printf(TDINDENT "COMPOSER ");
        cue_quoted(id3->composer);
        cps_printf("\\n");
    }

''' + metadata[end:]
    start = metadata.index("#define ID3_TAG_STR")
    end = metadata.index("\n\n", start)
    metadata = metadata[:start] + '''#define ID3_TAG_STR(value, tag, flag) if (ISVALID(value)) \\
    {cps_printf(TDINDENT "REM %s ", tag); cue_quoted(value); \\
     cps_printf("\\n"); tag_flags |= flag;}''' + metadata[end:]
    metadata = metadata.replace("#undef IS_VALID", "#undef ISVALID")
    adapter = replace(adapter, "@UPSTREAM_METADATA@", metadata)
    return f'{header}#include "plugin.h"\n#define TDINDENT "   "\n\n{reader}\n{adapter}'


def stage_lrc(text, reader, adapter):
    # Embedded lyrics are available in the native player. The compatibility
    # module uses checked sidecar parsing and does not run the legacy frame reader.
    begin = text.index("#define LRC_BUFFER_SIZE")
    end = text.index("/* define this to show debug info", begin)
    text = text[:begin] + text[end:]
    start = text.index("static void load_lrc_file(void)")
    end = text.index("static void display_state(void)", start)
    text = text[:start] + adapter + "\n" + text[end:]
    start = text.index("static void save_changes", text.index("static const char *get_lrc_timeline"))
    end = text.index("static int timetag_editor", start)
    text = text[:start] + text[end:]
    text = replace(text, '#include <ctype.h>',
                   '#include <ctype.h>\n\n' + reader +
                   '\n#define LRC_BYTE_LIMIT (512 * 1024)\n#define LRC_LINE_LIMIT 2048\n'
                   'static int source_width = 1;\n'
                   'static int source_encoding;\n'
                   'static const char *snc_text;\n'
                   'static bool lrc_integer(const char *, int, int, int *);')
    replacements = (
        ("    current.title = NULL;",
         "    snc_text = NULL;\n    current.title = NULL;"),
        ("    if (join) lrc_buffer_used--;",
         "    if (join)\n    {\n        if (!lrc_buffer_used)\n            return NULL;\n"
         "        lrc_buffer_used--;\n    }"),
        ("    siz = (siz+3) & ~3;",
         "    siz = ALIGN_UP(siz, _Alignof(struct lrc_line));"),
        ("    lrc_buffer_used = (lrc_buffer_used+3)&~3; /* 4 bytes aligned */",
         "    lrc_buffer_used = ALIGN_UP(lrc_buffer_used, _Alignof(struct lrc_brpos));"),
        ("    lrc_brpos = (struct lrc_brpos *) &lrc_buffer[lrc_buffer_used];\n"
         "    max_lrcbrpos = (lrc_buffer_end-lrc_buffer_used) / sizeof(struct lrc_brpos);",
         "    lrc_brpos = (struct lrc_brpos *)&lrc_buffer[lrc_buffer_used];\n"
         "    max_lrcbrpos = lrc_buffer_used < lrc_buffer_end\n"
         "        ? (lrc_buffer_end - lrc_buffer_used) / sizeof(*lrc_brpos) : 0;"),
        ("    /* calculate number of lines, line width and char count for each line. */",
         "    static struct lrc_brpos wrap_scratch[MAX_LINE_LEN + 1];\n"
         "    struct lrc_brpos *cache = lrc_brpos;\n"
         "    int cache_slots = max_lrcbrpos;\n"
         "    lrc_brpos = wrap_scratch;\n"
         "    max_lrcbrpos = ARRAYLEN(wrap_scratch) - 1;\n"
         "    /* calculate number of lines, line width and char count for each line. */"),
        ("    return lrc_brpos-nlrcbrpos;",
         "    *lrc_brpos = (struct lrc_brpos){0};\n"
         "    if (nlrcbrpos < cache_slots)\n"
         "        rb->memcpy(cache, wrap_scratch, (nlrcbrpos + 1) * sizeof(*cache));\n"
         "    return wrap_scratch;"),
        ("                    lrc_buffer_used += lrc_line->nline[i]*sizeof(struct lrc_brpos);",
         "                    lrc_buffer_used += lrc_line->nline[i] * sizeof(struct lrc_brpos);"),
        ("                lrc_brpos++;\n                str = lrc_skip_space(str);\n"
         "                xpos = (vp_lyrics[i].width - lrc_brpos->width)*prefs.align/2;",
         "                lrc_brpos++;\n                str = lrc_skip_space(str);\n"
         "                if (word_count > 0 || nword > 0)\n"
         "                    xpos = (vp_lyrics[i].width - lrc_brpos->width)*prefs.align/2;"),
        ("    time_start -= current.offset;\n    time_start -= time_start%10;",
         "    time_start = MAX(0, time_start - current.offset);\n"
         "    time_start -= time_start % 10;"),
        ("#define get_word_time_start(x)      get_time_start((struct lrc_line *)(x))\n"
         "#define set_word_time_start(x, t)   set_time_start((struct lrc_line *)(x), (t))",
         "#define get_word_time_start(word) MAX(0, (word)->time_start + current.offset)"),
        ("    *val = rb->atoi(ptr);\n    while (isdigit(*ptr)) ptr++;",
         "    *val = 0;\n    while (isdigit((unsigned char)*ptr))\n"
         "    {\n        if (*val <= 1000)\n            *val = *val * 10 + *ptr - '0';\n"
         "        ptr++;\n    }"),
        ("            current.offset = rb->atoi(&tag[7]);",
         "            int offset;\n"
         "            if (!lrc_integer(tag + 7, -60000, 60000, &offset))\n"
         "                return -10;\n            current.offset = offset;"),
        ("        file_offset += (intptr_t)tagend - (intptr_t)str;\n        str = tagend;",
         "        file_offset += (tagend - str) * source_width;\n        str = tagend;"),
        ("    if (ptr-tag != 2 || (*ptr != '.'", "    if (val >= 60 || ptr-tag != 2 || (*ptr != '.'"),
        ("    names[0] = rb->strrchr(temp_buf, '/')+1;",
         "    names[0] = rb->strrchr(temp_buf, '/');\n"
         "    if (!names[0]) return false;\n    names[0]++;"),
        ("            if (!rb->dir_exists(current.lrc_file))",
         "            if (len < 0 || len >= MAX_PATH ||\n"
         "                !rb->dir_exists(current.lrc_file))"),
        ("                    rb->snprintf(&current.lrc_file[len], MAX_PATH-len,",
         "                    int used = rb->snprintf(&current.lrc_file[len], MAX_PATH-len,"),
        ("                    if (rb->file_exists(current.lrc_file))",
         "                    if (used >= 0 && used < MAX_PATH - len &&\n"
         "                        rb->file_exists(current.lrc_file))"),
        ("        rb->iso_decode(pos, line, prefs.encoding, rb->strlen(pos)+1);",
         "        unsigned char *end = rb->iso_decode(pos, line, source_encoding,\n"
         "                                             rb->strlen(pos));\n"
         "        if (end - (unsigned char *)line >= MAX_LINE_LEN - 1)\n"
         "            return false;\n        *end = '\\0';"),
        ("        if (pos[0]==0)\n            return true;",
         "        snc_text = get_lrc_str(lrc_line);\n"
         "        if (!pos[0])\n            return true;"),
        ("    if (current.ll_head)\n    {\n        rb->strcat(line, \"\\n\");",
         "    if (snc_text)\n    {\n"
         "        if (rb->strlen(snc_text) + rb->strlen(line) >= MAX_LINE_LEN - 1)\n"
         "            return false;\n        rb->strcat(line, \"\\n\");"),
        ("                while (elapsed > lrc_brpos->width)",
         "                for (int rows = lrc_center->nline[i];\n"
         "                     rows > 1 && elapsed > lrc_brpos->width; rows--)"),
        ("        while (y < -lrc_line->nline[i])",
         "        while (lrc_line && y < -lrc_line->nline[i])"),
        ("        ret = handle_button();",
         "        if (prefs.statusbar_on)\n"
         "            rb->send_event(GUI_EVENT_ACTIONUPDATE, NULL);\n"
         "        ret = handle_button();"),
        ("    if (AUDIO_PLAY && prefs.display_title)", "    if (current.id3 && prefs.display_title)"),
        ("                usb = rb->set_color(NULL, \"Inactive Colour\",",
         "                usb = rb->set_color(\"Inactive Colour\","),
        ("        configfile_load(config_file, config, ARRAYLEN(config), 0);",
         "        lrc_config_load(config_file, config, ARRAYLEN(config));"),
        ("        configfile_save(config_file, config, ARRAYLEN(config), 0);",
         "        if (!lrc_config_save(config_file, config, ARRAYLEN(config)))\n"
         "            rb->splash(HZ, \"Could not save settings\");"),
        ("                    save_changes();\n                    if (current.type < NUM_TYPES)",
         "                    if (current.type < NUM_TYPES)"),
        ("    rb->audio_ff_rewind(time);", "    rb->audio_seek_now(time);"),
        ("                rb->audio_pre_ff_rewind();\n", ""),
        ("            save_changes();\n            ret = PLUGIN_OK;",
         "            ret = PLUGIN_OK;"),
        ("                save_changes();\n                reset_current_data();",
         "                reset_current_data();\n                current.ff_rewind = -1;"),
        ("            if (AUDIO_STOP)\n            {\n                current.id3 = NULL;",
         "            if (!current.id3 || AUDIO_STOP)\n            {\n                current.id3 = NULL;"),
        ("        button = rb->get_action(CONTEXT_TREE, TIMEOUT_BLOCK);",
         "        button = rb->get_action(CONTEXT_TREE, TIMEOUT_BLOCK);\n"
         "        if (IS_SYSEVENT(button))\n"
         "        {\n"
         "            if (rb->default_event_handler(button) == SYS_USB_CONNECTED)\n"
         "                return PLUGIN_USB_CONNECTED;\n"
         "            continue;\n"
         "        }\n"
         "        current.id3 = rb->audio_current_track();\n"
         "        if (!current.id3 || rb->strcmp(current.mp3_file, current.id3->path))\n"
         "            return LRC_GOTO_MAIN;"),
        ("    lrc_buffer = ALIGN_UP(lrc_buffer, 4); /* 4 bytes aligned */\n"
         "    lrc_buffer_size = (lrc_buffer_size - 4)&~3;",
         "    if (!lrc_buffer || lrc_buffer_size < 4096)\n        return PLUGIN_ERROR;\n"
         "    lrc_buffer_size &= ~((size_t)_Alignof(struct lrc_line) - 1);"),
        ("    if (parameter && check_audio_status())\n    {",
         "    if (parameter)\n    {\n        check_audio_status();\n"
         "        if (!current.id3 || AUDIO_STOP)\n        {\n"
         "            rb->splash(HZ * 2, \"Play a track before opening lyrics\");\n"
         "            return PLUGIN_OK;\n        }"),
        ("        rb->strcpy(current.lrc_file, parameter);",
         "        if (rb->strlcpy(current.lrc_file, parameter, sizeof(current.lrc_file))\n"
         "            >= sizeof(current.lrc_file))\n            return PLUGIN_ERROR;"),
        ("    load_or_save_settings(true);",
         "    if (!rb->plugin_usb_taken())\n        load_or_save_settings(true);"),
        ("                            \"Yes\", \"No (save later)\", \"Discard All Changes\")",
         "                            \"Yes\", \"Keep in memory\", \"Discard All Changes\");"),
        ("                case 1:\n                    sort_lrcs();",
         "                case GO_TO_PREVIOUS:\n                case 1:\n                    sort_lrcs();"),
        ("            case ACTION_TREE_STOP:\n            case ACTION_STD_CANCEL:",
         "            case ACTION_TREE_STOP:\n            case ACTION_STD_MENU:\n"
         "            case ACTION_STD_CANCEL:"),
    )
    for before, after in replacements:
        if before == "                rb->audio_pre_ff_rewind();\n":
            if text.count(before) != 2:
                raise ValueError("expected two upstream pre-seeks")
            text = text.replace(before, after)
        elif before != after:
            text = replace(text, before, after)
    text = replace(text, "        case ACTION_WPS_MENU:\n            ret = LRC_GOTO_MENU;",
                   "        case ACTION_WPS_QUICKSCREEN:\n            ret = PLUGIN_OK;\n"
                   "            break;\n        case ACTION_WPS_MENU:\n            ret = LRC_GOTO_MENU;")
    start = text.index("static int lrc_main(void)")
    end = text.index("/* this is the plugin entry point */", start)
    suffix = text[start:end]
    suffix = replace(suffix, "    return ret;", "    current.ff_rewind = -1;\n    return ret;")
    return text[:start] + suffix + text[end:]


if __name__ == "__main__":
    source, adapter, reader, target = map(Path, sys.argv[1:])
    data = source.read_bytes()
    if sha256(data).hexdigest() != DIGESTS[source.stem]:
        raise SystemExit(f"{source}: review the AstraPod adapter for this source")
    stage = cue if source.stem == "cue_playlist" else stage_lrc
    target.write_text(stage(data.decode(), reader.read_text(), adapter.read_text()))
