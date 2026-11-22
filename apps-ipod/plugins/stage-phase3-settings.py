#!/usr/bin/env python3
"""Stage Phase 3 utility adapters. GNU GPL version 2 or later."""
from pathlib import Path
import sys


def replace(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f"Expected one occurrence of {old!r}")
    return text.replace(old, new, 1)


def windows_lnk(source, path):
    boundary = source.index("/**\n *  Read one byte from file")
    return source[:boundary] + path.with_name("windows-link-compat.inc").read_text()


def eq_optimizer(source):
    source = replace(source, "static int32_t tan_l(int32_t p)",
                     "static int32_t l2_pi_rate = L2_PI_FS;\n"
                     "static unsigned response_rate = 44100;\n\n"
                     "static int32_t tan_l(int32_t p)")
    source = replace(source, "int32_t lt = p + L2_PI_FS;",
                     "int32_t lt = p + l2_pi_rate;")
    source = replace(source, "    init_tables();\n    setup_grid();",
                     "    init_tables();\n"
                     "    l2_pi_rate = L2_PI_FS + log2_u64(44100) - log2_u64(response_rate);\n"
                     "    setup_grid();")
    source = replace(source, "static int progress_done, progress_total;",
                     "static int progress_done, progress_total;\n"
                     "static bool cancelled;")
    source = replace(source, """    rb->yield();
#endif
}""", """    if (!cancelled && rb->action_userabort(TIMEOUT_NOBLOCK))
        cancelled = true;
    rb->yield();
#endif
}""")
    source = replace(source, "for (int it = 0; it < maxit; it++)",
                     "for (int it = 0; it < maxit && !cancelled; it++)")
    source = replace(source, "for (int pass = 0; pass < 25; pass++)",
                     "for (int pass = 0; pass < 25 && !cancelled; pass++)")
    source = replace(source, "            bump_progress();",
                     "            if (cancelled)\n"
                     "                return;\n"
                     "            bump_progress();")
    source = replace(source, "        quantise_polish(cur, ncur, &fits[ncur]);",
                     "        if (cancelled)\n"
                     "            return;\n"
                     "        quantise_polish(cur, ncur, &fits[ncur]);\n"
                     "        if (cancelled)\n"
                     "            return;")
    source = replace(source, '    rb->splash_progress(done, total, "Optimising EQ...");',
                     '    rb->splash_progress(done, total, "Optimising EQ... Menu to cancel");')
    source = replace(source, """    rb->button_clear_queue();
    rb->button_get(true);
    rb->button_clear_queue();""", """    rb->button_clear_queue();
    while (true)
    {
        int action = rb->get_action(CONTEXT_STD, TIMEOUT_BLOCK);
        if (rb->default_event_handler(action) == SYS_USB_CONNECTED ||
            action == ACTION_STD_OK || action == ACTION_STD_CANCEL)
            break;
    }""")
    start = source.index("static void apply_settings(")
    stop = source.index("static void apply_fit(", start)
    source = source[:start] + source[stop:]
    source = replace(source, """    apply_settings(set, fit_precut(f, saved_precut));
    rb->splashf(HZ, "Applied %d-band EQ", n);""", """    if (rb->mixer_get_frequency() != response_rate)
        rb->splash(HZ * 2, "Output rate changed. Reopen EQ Optimizer");
    else if (rb->apply_eq(set, fit_precut(f, saved_precut)))
        rb->splashf(HZ, "Applied %d-band EQ", n);
    else
        rb->splash(HZ * 2, "Could not apply the fit");""")
    source = replace(source, """            apply_settings(saved_eq, saved_precut);
            rb->splash(HZ, "Original EQ restored");""", """            if (rb->apply_eq(saved_eq, saved_precut))
                rb->splash(HZ, "Original EQ restored");
            else
                rb->splash(HZ * 2, "Could not restore EQ");""")
    source = replace(source, "        case 3:\n            return true;",
                     "        case 3:\n        case MENU_ATTACHED_USB:\n"
                     "            return true;")
    source = replace(source, "    optimise();",
                     "    cancelled = false;\n    optimise();")
    source = replace(source, '''    (void)parameter;

    rb->memcpy(saved_eq''', '''    (void)parameter;

    response_rate = rb->mixer_get_frequency();
    if (response_rate < 44100)
    {
        rb->splash(HZ * 2, "EQ Optimizer needs a 44.1 kHz or higher output rate");
        return PLUGIN_OK;
    }
    rb->memcpy(saved_eq''')
    source = replace(source, '    rb->splashf(HZ, "Done in %ld.%02lds", dt / HZ, (dt % HZ) * 100 / HZ);',
                     '    if (cancelled)\n'
                     '        return rb->plugin_usb_taken() ? PLUGIN_USB_CONNECTED : PLUGIN_OK;\n'
                     '    rb->splashf(HZ, "Done in %ld.%02lds", dt / HZ, (dt % HZ) * 100 / HZ);')
    source = replace(source, "                    return PLUGIN_OK;\n                break;\n            case PLA_CANCEL:",
                     "                    return rb->plugin_usb_taken() ? PLUGIN_USB_CONNECTED : PLUGIN_OK;\n"
                     "                break;\n            case PLA_CANCEL:")
    return replace(source, "            case PLA_CANCEL:\n",
                   "#if CONFIG_KEYPAD == IPOD_4G_PAD\n"
                   "            case PLA_UP: /* Menu */\n"
                   "#endif\n"
                   "            case PLA_CANCEL:\n")


def tagnavi_edit(source):
    source = replace(source, '#include "plugin.h"', '''#include "plugin.h"
#include "screens/browse/browser_db.h"
#include "plugins/phase3-file-io.h"''')
    source = replace(source, '#define USER_FILE   ROCKBOX_DIR "/tagnavi_user.config"',
                     '#define ACTIVE_FILE ROCKBOX_DIR "/tagnavi_user.config"\n'
                     '#define USER_FILE   ROCKBOX_DIR "/tagnavi_edit.config"')
    source = replace(source, '#define TMP_FILE    ROCKBOX_DIR "/tagnavi_user.tmp"\n', '')
    source = replace(source, '#define BAK_FILE    ROCKBOX_DIR "/tagnavi_user.bak"\n', '')
    start = source.index("static bool write_file(")
    end = source.index("/* Problems that stop a save.", start)
    source = source[:start] + r'''static bool write_draft(const char *text, size_t length)
{
    char temp[MAX_PATH];
    int fd = begin_replace(USER_FILE, temp);
    return fd >= 0 && end_replace(fd, temp, USER_FILE,
                                  rb->write(fd, text, length) == (ssize_t)length);
}

static int read_file(const char *path, char *buf, size_t capacity)
{
    int fd = rb->open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    off_t size = rb->ffilesize(fd);
    bool fits = size >= 0 && (uintmax_t)size < capacity;
    bool ok = fits && rb->read(fd, buf, size) == size;
    if (rb->close(fd) < 0)
        ok = false;
    if (!ok)
        return !fits && size >= 0 ? -2 : -1;
    buf[size] = '\0';
    return rb->strlen(buf) == (size_t)size ? size : -1;
}

''' + source[end:]
    start = source.index("    if (!write_file(TMP_FILE")
    end = source.index("    dirty = false;", start)
    source = source[:start] + '''    if (!write_draft(ser_buf, len))
    {
        rb->splash(HZ * 2, "Could not save the menu draft");
        return false;
    }
''' + source[end:]
    source = replace(source, """    /* the Database reloads its menus the next time it's opened */
    rb->tagtree_request_reload();
    rb->splash(HZ, "Database Updated");""", '''    rb->splash(HZ, "Saved tagnavi_edit.config draft");''')
    source = replace(source, '''        "Link an existing menu", "Reload menus item", LSTR(LANG_TAGNAVI_EDIT_MENUS),''',
                     '''        "Link an existing menu",''')
    start = source.index('        case 4:\n        case 5:\n            if ((e = k == 4')
    end = source.index('        default:', start)
    source = source[:start] + source[end:]
    source = replace(source, '"Restore stock menus"', '"Start from current menus"')
    source = replace(source, 'static bool more_options(void)\n{', '''static const char *active_menu_file(void)
{
    return rb->file_exists(ACTIVE_FILE) ? ACTIVE_FILE : STOCK_FILE;
}

/* Reloads the model when it returns true. */
static bool more_options(void)
{''')
    source = replace(source, 'const char *path = rb->file_exists(USER_FILE) ? USER_FILE : STOCK_FILE;',
                     'const char *path = rb->file_exists(USER_FILE) ? USER_FILE : active_menu_file();')
    start = source.index('                if (confirm("Go back to the stock Database menus?')
    end = source.index('                break;', start)
    source = source[:start] + '''                if (confirm("Use current Database menus as the new draft?"))
                {
                    if (load(active_menu_file()))
                        dirty = true;
                    return true;
                }
''' + source[end:]
    source = replace(source, 'if (!load(user ? USER_FILE : STOCK_FILE))',
                     'if (!load(user ? USER_FILE : active_menu_file()))')
    source = replace(source, '''    arena_used = arena_base;
    oom = false;
    int n = read_file''', '''    model = (struct tn_model){0};
    dirty = false;
    arena_used = arena_base;
    oom = false;
    int n = read_file''')
    source = replace(source, '''    if (err)
    {
        rb->splashf(HZ * 3, "%s: %s", path, err);''', '''    if (err)
    {
        model = (struct tn_model){0};
        rb->splashf(HZ * 3, "%s: %s", path, err);''')
    source = replace(source, '''        if (is_root)
            m = top_menu();   /* may change from More options */
        rb->snprintf''', '''        if (is_root)
            m = top_menu();   /* may change from More options */
        if (!m)
            return;
        rb->snprintf''')
    source = replace(source, '''        if (!user || !confirm("Your tagnavi_user.config can't be read. Start from the stock menus?")
            || !load(STOCK_FILE))''', '''        if (!confirm("Menu file cannot be read. Start from the stock menus?")
            || !load(STOCK_FILE))''')
    source = replace(source, 'save_and_report();\n    return PLUGIN_OK;',
                     'save_and_report();\n    return rb->plugin_usb_taken() ? PLUGIN_USB_CONNECTED : PLUGIN_OK;')
    return source


def random_folder(source, path):
    start = source.index("struct rfa_scan")
    stop = source.index("static const char* list_get_name_cb(", start)
    source = source[:start] + path.with_name("random-folder-compat.inc").read_text() + "\n" + source[stop:]
    start = source.index("static int load_dirs(", source.index("static const char* list_get_name_cb("))
    stop = source.index("static bool disable_dir(", start)
    source = source[:start] + source[stop:]
    source = replace(source, """    scan->dirty = false;
    rb->gui_synclist_init""", """    if (scan->dirs->count <= 0)
    {
        rb->splash(HZ, "No folders in the list");
        return -1;
    }
    scan->dirty = false;
    rb->gui_synclist_init""")
    source = replace(source, '''        selection = rb->gui_synclist_get_sel_pos(&lists);
        switch (button)
''', '''        selection = rb->gui_synclist_get_sel_pos(&lists);
        if (rb->default_event_handler(button) == SYS_USB_CONNECTED)
            return -1;
        switch (button)
''')
    source = replace(source, """                int len;
                MENUITEM_STRINGLIST""", """                if (scan->dirs->dir[selection][0] != '/')
                    break;
                MENUITEM_STRINGLIST""")
    source = replace(source, """                        len = rb->strlen(scan->path);
                        j = 0;""", "                        j = 0;")
    source = replace(source, "if (!rb->strncmp(scan->dirs->dir[i], scan->path, len))",
                     "if (in_folder(scan->dirs->dir[i], scan->path))")
    source = replace(source, """                        save_dirs(scan);
                        /* fallthrough */""", """                        if (save_dirs(scan) < 0 && !rb->plugin_usb_taken())
                            rb->splash(HZ, "Folder list not saved");
                        /* fallthrough */""")
    source = replace(source, """                    default:
                        set_title(&lists, false, -1);
                        break;
                }
            }
            break;
        }""", """                    default:
                        set_title(&lists, false, -1);
                        break;
                }
            }
            break;
        }
        if (rb->plugin_usb_taken())
            return -1;""")
    start = source.index("static int export(", source.index("static int edit("))
    stop = source.index("static enum plugin_status main_menu(", start)
    source = source[:start] + source[stop:]
    source = replace(source, '"Play Shuffled",', '"Save Shuffled Playlist",')
    start = source.index("            case GO_TO_PREVIOUS_MUSIC:")
    stop = source.index("            case 1: /* Scan Folders */", start)
    source = source[:start] + '''            case MENU_ATTACHED_USB:
                return PLUGIN_USB_CONNECTED;
            case GO_TO_PREVIOUS_MUSIC:
                return PLUGIN_GOTO_WPS;
            case GO_TO_ROOT:
                return PLUGIN_GOTO_ROOT;
            case GO_TO_PREVIOUS:
                return PLUGIN_OK;
            case 0: /* Save Shuffled Playlist */
            {
                int result = save_shuffled(&scan);
                if (!rb->plugin_usb_taken())
                    rb->splash(2 * HZ, result > 0 ? "Saved " RFA_PLAYLIST
                                                  : "Playlist not saved");
                break;
            }
''' + source[stop:]
    source = replace(source, "                export(&scan);", '''                if (export(&scan) > 0)
                    rb->splash(HZ, "Folder list exported");
                else if (!rb->plugin_usb_taken())
                    rb->splash(HZ, "Export stopped");''')
    source = replace(source, "                import(&scan);", '''                if (import(&scan) > 0)
                    rb->splash(HZ, "Folder list imported");
                else if (!rb->plugin_usb_taken())
                    rb->splash(HZ, "Import stopped");''')
    return source


if __name__ == "__main__":
    src, out = (Path(arg) for arg in sys.argv[1:])
    if src.stem == "windows_lnk":
        code = windows_lnk(src.read_text(), Path(__file__))
    elif src.stem == "eq_optimizer":
        code = eq_optimizer(src.read_text())
    elif src.stem == "tagnavi_edit":
        code = tagnavi_edit(src.read_text())
    elif src.stem == "random_folder_advance_config":
        code = random_folder(src.read_text(), Path(__file__))
    else:
        raise ValueError(f"Invalid source: {src}")
    out.write_text(code)
