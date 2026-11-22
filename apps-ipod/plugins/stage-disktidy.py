#!/usr/bin/env python3
"""Stage owned filesystem operations for upstream Disk Tidy."""
from hashlib import sha256
from pathlib import Path
import sys


SOURCE_SHA256 = "62e1e71c8f0e4efddcc7c0bee6fa6ea1b112c32a9a7252f69050f0a1d29d4f28"


def replace(source, before, after):
    if source.count(before) != 1:
        raise ValueError(f"expected one upstream match for {before!r}")
    return source.replace(before, after, 1)


def section(source, start, end, replacement=""):
    a = source.index(start)
    b = source.index(end, a)
    return source[:a] + replacement + source[b:]


def stage(source, adapter):
    stats_io, scan = adapter.split("/* Directory scan. */\n")
    source = replace(source, '#include "errno.h"', '#include <limits.h>')
    source = replace(source, '#include "lib/display_text.h"\n', "")
    source = section(source, "#define CLEANING_STR", "struct run_statistics")
    source = section(source, "struct stats_text", "struct tidy_type")
    source = section(source, "static bool sbs_has_title;", "static void add_item")
    source = section(source, "static bool save_run_stats", "static bool match",
                     stats_io)
    source = section(source, "static void tidy_lcd_status", "static enum plugin_status tidy_do",
                     scan)
    source = replace(source, '''    if (file[0] == '<' || idx_last_group == -1)''',
                     '''    if (tidy_type_count == ARRAYLEN(tidy_types))
        return -1;
    if (file[0] == '<' || idx_last_group == -1)''')
    source = replace(source, "static void tidy_load_file(const char* file)",
                     "static bool tidy_load_file(const char *file, bool optional)")
    source = replace(source, '''    if (fd < 0)
        return;

    while ((tidy_type_count < sizeof(tidy_types) / sizeof(tidy_types[0])) && rb->read_line(fd, buf, MAX_PATH))
    {''',
                     '''    if (fd < 0)
        return optional && !rb->file_exists(file);

    int length;
    bool valid = true;
    while ((length = rb->read_line(fd, buf, sizeof(buf))) > 0)
    {
        if (length >= (int)sizeof(buf) - 1)
        {
            valid = false;
            break;
        }''')
    source = replace(source, '''        unsigned i = find_file_string(str, last_group);

        tidy_types[i].remove = !rb->strcmp(remove, "yes");''',
                     '''        size_t size = rb->strlen(str);
        bool selected = !rb->strcmp(remove, "yes");
        bool group = str[0] == '<';
        const char *slash = rb->strchr(str, '/');
        const char *wildcard = rb->strchr(str, '*');
        if (size == 0 || size >= sizeof(tidy_types[0].filestring) ||
            (group && str[size - 1] != '>') ||
            (!group && slash && (size == 1 || slash != str + size - 1)) ||
            (!group && wildcard &&
             (rb->strchr(wildcard + 1, '*') ||
              !rb->strcmp(str, "*") || !rb->strcmp(str, "*/"))) ||
            (!selected && rb->strcmp(remove, "no")))
        {
            valid = false;
            break;
        }
        int index = find_file_string(str, last_group);
        if (index < 0)
        {
            valid = false;
            break;
        }
        size_t i = index;
        tidy_types[i].remove = selected;''')
    source = replace(source, '''            rb->strcpy(last_group, str);
    }
    rb->close(fd);
}''',
                     '''            rb->strcpy(last_group, str);
    }
    return rb->close(fd) == 0 && valid && length == 0;
}''')
    source = replace(source, '''    for (struct tidy_type *t = &tidy_types[0]; t < &tidy_types[tidy_type_count]; t++)
        if (match(t, item, rb->strlen(item)))
            return t->remove && ((!!(attr&ATTR_DIRECTORY)) == t->directory);''',
                     '''    bool directory = (attr & ATTR_DIRECTORY) != 0;
    int length = rb->strlen(item);
    for (size_t i = 0; i < tidy_type_count; i++)
    {
        struct tidy_type *type = &tidy_types[i];
        if (type->filestring[0] != '<' && type->directory == directory &&
            match(type, item, length))
            return type->remove;
    }''')
    source = replace(source, '''    sbs_has_title = rb->sb_set_title_text(CLEANING_STR, Icon_NOICON, SCREEN_MAIN);
    if (sbs_has_title)
        rb->send_event(GUI_EVENT_ACTIONREDRAW, (void*)1);

''', '')
    source = section(source, "static const char* get_name",
                     "static int list_action_callback",
                     '''static const char *get_name(int item, void *data,
                            char *buffer, size_t size)
{
    (void)data;
    const struct tidy_type *rule = &tidy_types[item];
    if (rule->filestring[0] == '<')
        return rule->filestring;
    rb->snprintf(buffer, size, "%s%s%s", rule->remove ? "[clean] " : "",
                 rule->filestring, rule->directory ? "/" : "");
    return buffer;
}

''')
    source = replace(source, '''    unsigned selection = rb->gui_synclist_get_sel_pos(lists);
    if (tidy_types[selection].filestring[0] == '<')''',
                     '''    unsigned selection = rb->gui_synclist_get_sel_pos(lists);
    if (selection >= tidy_type_count)
        return action;
    if (tidy_types[selection].filestring[0] == '<')''')
    source = replace(source, '''    int path_length = rb->strlen(path);
    enum plugin_status status = tidy_clean(path, &path_length);''',
                     '''    user_abort = false;
    rb->splash_progress_set_delay(0);
    enum plugin_status status = tidy_clean(path);''')
    source = replace(source, '''    run_stats.run_duration = (*rb->current_tick - start_time) / HZ;
    stats_file_exists = save_run_stats();''',
                     '''    if (status == PLUGIN_USB_CONNECTED)
        return status;

    run_stats.run_duration = (*rb->current_tick - start_time) / HZ;
    if (save_run_stats())
        stats_file_exists = true;
    else
    {
        rb->splash(HZ * 2, "Unable to save last run stats");
        return PLUGIN_ERROR;
    }
    if (status == PLUGIN_ERROR)
        rb->splashf(HZ * 2, "Cleaning stopped near %s", path);''')
    source = replace(source, '''#ifdef HAVE_DIRCACHE
                    rb->splash(0, ID2P(LANG_WAIT));
                    rb->dircache_wait();
#endif
                    disktidy_status = tidy_do();''',
                     '''                    const char *lines[] = {
                        "Delete selected file types?",
                        "This deletes matching folders and their contents."
                    };
                    const struct text_message message = {
                        lines, ARRAYLEN(lines)
                    };
                    enum yesno_res answer =
                        rb->gui_syncyesno_run(&message, NULL, NULL);
                    if (answer == YESNO_USB)
                        return PLUGIN_USB_CONNECTED;
                    if (answer != YESNO_YES)
                        break;

                    disktidy_status = tidy_do();''')
    source = replace(source, '''                bool show_icons = rb->global_settings->show_icons;
                rb->global_settings->show_icons = true;
                rb->simplelist_show_list(&list);
                rb->global_settings->show_icons = show_icons;''',
                     '''                if (rb->simplelist_show_list(&list))
                    return PLUGIN_USB_CONNECTED;''')
    source = replace(source, '''            default:
                exit = true;''',
                     '''            case MENU_ATTACHED_USB:
                return PLUGIN_USB_CONNECTED;
            default:
                exit = true;''')
    source = replace(source, '''    int fd = rb->creat(CUSTOM_FILES, 0666);''',
                     '''    const char *temp = CUSTOM_FILES ".tmp";
    int fd = rb->open(temp, O_WRONLY | O_CREAT | O_EXCL, 0666);''')
    source = replace(source, '''    if (fd < 0)
        return;

    for (unsigned i=0; i<tidy_type_count; i++)
        rb->fdprintf(fd, "%s%s%s: %s\\n",''',
                     '''    if (fd < 0)
    {
        rb->splash(HZ * 2, "Unable to save cleaning rules");
        return;
    }

    bool ok = true;
    for (unsigned i = 0; ok && i < tidy_type_count; i++)
        ok = rb->fdprintf(fd, "%s%s%s: %s\\n",''')
    source = replace(source, '''                     tidy_types[i].remove ? "yes" : "no");
    rb->close(fd);''',
                     '''                     tidy_types[i].remove ? "yes" : "no") >= 0;
    if (!finish_file(fd, temp, CUSTOM_FILES, ok))
        rb->splash(HZ * 2, "Unable to save cleaning rules");''')
    source = replace(source, '''    tidy_load_file(DEFAULT_FILES);
    tidy_load_file(CUSTOM_FILES);

    if (tidy_type_count == 0)
    {
        rb->splash(3*HZ, "Missing disktidy.config file");''',
                     '''    if (!tidy_load_file(DEFAULT_FILES, false) ||
        !tidy_load_file(CUSTOM_FILES, true) || tidy_type_count == 0)
    {
        rb->splash(3 * HZ, "Missing or invalid Disk Tidy rules");''')
    return replace(source, "    if (tidy_loaded_and_changed) {",
                   "    if (tidy_loaded_and_changed && !rb->plugin_usb_taken()) {")


if __name__ == "__main__":
    source, adapter, output = map(Path, sys.argv[1:])
    upstream = source.read_bytes()
    if sha256(upstream).hexdigest() != SOURCE_SHA256:
        raise SystemExit(f"{source}: review the Disk Tidy source adapter")
    output.write_text(stage(upstream.decode(), adapter.read_text()))
