#!/usr/bin/env python3
"""Stage the stock editable and sealed .link viewers."""
from pathlib import Path
import sys


def replace(text, before, after):
    if text.count(before) != 1:
        raise SystemExit(f"review Shortcuts adapter for {before!r}")
    return text.replace(before, after)


source, target = map(Path, sys.argv[1:3])
text = source.read_text()
if source.name == "shortcuts_common.c":
    text = replace(text, '#include "shortcuts.h"',
                   '''#include "shortcuts.h"
#include "plugins/rock-line-reader.inc"
#include "plugins/phase3-file-io.h"''')
    text = replace(text, "void write_entry_to_file(int fd, sc_entry_t *entry);\n"
                   "void write_int_instruction_to_file(int fd, char *instr, int value);\n",
                   "")
    begin = text.index("    int fd = -1;")
    end = text.index("\n}\n", begin)
    text = text[:begin] + r'''    init_sc_file(file, entry_buf, entry_bufsize);

    off_t bom;
    int fd = open_text(filename, &bom);
    if (fd < 0)
        return !must_exist && !rb->file_exists(filename);

    struct line_reader reader = {
        .fd = fd, .size = rb->ffilesize(fd), .position = bom,
    };
    char line[2 * MAX_PATH + 2];
    bool ok = reader.size >= bom;
    while (ok)
    {
        size_t used;
        int status = input_line(&reader, (unsigned char *)line,
                                sizeof(line), &used, 1, false);
        if (status <= 0)
        {
            ok = status == 0;
            break;
        }
        line[used] = '\0';
        if (!*line || is_control(line, file))
            continue;
        sc_entry_t entry;
        if (line[0] != '/' || !parse_entry_content(line, &entry,
                                                   file->show_last_segments) ||
            !append_entry(file, &entry))
            ok = false;
    }

    return rb->close(fd) == 0 && ok;''' + text[end:]
    text = text.replace("rb->strcasestr(line, PATH_DISP_SEPARATOR)",
                        "rb->strchr(line, '\\t')")
    text = text.replace("rb->strcasestr(line, NAME_VALUE_SEPARATOR)",
                        "rb->strchr(line, '=')")
    begin = text.index("    DEBUGF(\"Dumping shortcuts")
    text = text[:begin] + r'''    char temp[MAX_PATH];
    int fd = begin_replace(filename, temp);
    if (fd < 0)
        return false;

    bool ok = rb->fdprintf(fd, "%s%s%s%d\n", CONTROL_PREFIX,
                           INSTR_DISPLAY_LAST_SEGMENTS,
                           NAME_VALUE_SEPARATOR,
                           file->show_last_segments) >= 0;
    for (int i = 0; ok && i < file->entry_cnt; ++i)
    {
        const sc_entry_t *entry = file->entries + i;
        if (entry->explicit_disp)
            ok = rb->fdprintf(fd, "%s%s%s\n", entry->path,
                              PATH_DISP_SEPARATOR, entry->disp) >= 0;
        else
            ok = rb->fdprintf(fd, "%s\n", entry->path) >= 0;
    }
    return end_replace(fd, temp, filename, ok);
}
'''
elif source.name == "shortcuts_append.c":
    begin = text.index("    /* This is a viewer,")
    end = text.index("    DEBUGF(", begin)
    text = text[:begin] + r'''    char selected[MAX_PATH];
    const char *path = void_parameter;
    if (!path)
    {
        int status = rb->pick_file(selected, sizeof(selected), "/",
                                  "Select shortcut target", NULL, 0, false);
        if (status <= 0)
            return rb->plugin_usb_taken() ? PLUGIN_USB_CONNECTED : PLUGIN_OK;
        path = selected;
    }

    if (*path != '/' || rb->strchr(path, '\t') || rb->strchr(path, '\r') ||
        rb->strchr(path, '\n'))
        return PLUGIN_ERROR;
    char *parameter = (char *)path;
''' + text[end:]
    text = replace(text, "    return PLUGIN_OK;\n}",
                   "    rb->splash(HZ, \"Shortcut added\");\n    return PLUGIN_OK;\n}")
elif source.name == "shortcuts_view.c":
    text = replace(text, "static size_t root_len;\n", "")
    text = replace(text, "bool ends_with(char *str, char *suffix);\n", "")
    text = replace(text, "            case ACTION_STD_MENU:",
                   "            case ACTION_STD_CONTEXT:")
    begin = text.index("#if 0")
    end = text.index("enum plugin_status plugin_start", begin)
    text = text[:begin] + '''int goto_entry(char *path)
{
    if (!rb->dir_exists(path) && !rb->file_exists(path))
    {
        rb->splash(HZ, "Shortcut target is unavailable");
        return PLUGIN_OK;
    }
    size_t length = rb->strlen(path);
    if (!rb->dir_exists(path) && length > 5 &&
        !rb->strcasecmp(path + length - 5, ".rock"))
        return rb->queue_handoff(path, NULL);
    return rb->queue_native_handoff(ROCK_HANDOFF_BROWSE, path);
}

''' + text[end:]
    begin = text.index("    /* This is a viewer,")
    end = text.index("    user_file =", begin)
    text = text[:begin] + '''    char path[MAX_PATH];
    if (!void_parameter && !rb->file_exists(SHORTCUTS_FILENAME))
    {
        int status = rb->pick_file(path, sizeof(path), "/",
                                  "Select a shortcut list", ".link", 0, false);
        if (status <= 0)
            return rb->plugin_usb_taken() ? PLUGIN_USB_CONNECTED : PLUGIN_OK;
        void_parameter = path;
    }
    link_filename = void_parameter ? (char *)void_parameter : SHORTCUTS_FILENAME;
''' + text[end:]
    text = replace(text, """            rb->splashf(HZ, "Deleting %s", sc_file.entries[*selected_item].disp);
            remove_entry(&sc_file, *selected_item);
            dump_sc_file(&sc_file, link_filename);""",
                   """            {
                const char *lines[] = { "Remove this shortcut?",
                                        sc_file.entries[*selected_item].disp };
                const struct text_message prompt = {lines, ARRAYLEN(lines)};
                if (rb->gui_syncyesno_run(&prompt, NULL, NULL) != YESNO_YES)
                    return rb->plugin_usb_taken() ? PLUGIN_USB_CONNECTED : PLUGIN_OK;
                remove_entry(&sc_file, *selected_item);
                if (!dump_sc_file(&sc_file, link_filename))
                    return PLUGIN_ERROR;
            }""")
    text = replace(text, "    action = draw_sc_list(&gui_sc);",
                   "    action = draw_sc_list(&gui_sc);\n"
                   "    rb->gui_synclist_scroll_stop(&gui_sc);")
else:
    raise SystemExit(f"unexpected Shortcuts source: {source}")
target.write_text(text)
