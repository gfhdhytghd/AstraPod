#!/usr/bin/env python3
"""Stage checked adapters for the Phase 3 foreground file utilities."""
from hashlib import sha256
from pathlib import Path
import sys


HERE = Path(__file__).resolve().parent

SOURCE_DIGESTS = {
    "otp": "bdfd54a7adb4830b302ab6e97980a282bef8a37ceb1d18eb583793d4adb71b7b",
    "sort": "35612af1eb462ac8691fa12f4a20fc3c1f99adf995081b4ba17ee3b0ec82be51",
    "speedread": "404f5ebe069a3f79444ca4a58e273e497a4e35376bec8daf20d9228ff215d71b",
    "text_editor": "3661509ba9dad158876e22d88f2a8ad6973faa0556be0e51d58e9b1e024d4ad9",
}


def replace(source, before, after, count=1):
    if source.count(before) != count:
        raise ValueError(f"upstream source changed: {before!r}")
    return source.replace(before, after)


def between(source, first, last, replacement):
    start = source.index(first)
    finish = source.index(last, start + len(first))
    return source[:start] + replacement + source[finish:]


def adapter(name):
    return (HERE / name).read_text() + "\n"


def sort(source):
    first = source.index("/* Compare function for sorting backwards */")
    last = source.index("static int read_buffer", first)
    header = source[:source.index("/*****", source.index('#include "plugin.h"'))]
    return (header + "static int num_entries;\nstatic char **pointers;\n\n"
            + source[first:last] + adapter("sort-compat.inc"))


def text_editor(source):
    source = between(source, "#if defined(DEBUG)", "#define MAX_LINE_LEN",
                     "#define logf(...) do { } while (0)\n\n")
    source = replace(source, "static bool audio_buf = false;\n", "")
    source = between(source, "static void get_eol_string", "static void setup_lists",
                     adapter("text-editor-io.inc"))
    source = replace(source, "    switch (action)\n",
                     """    if (line < 0 || (action == ACTION_INSERT
        ? line > line_count : line >= line_count))
        return action == ACTION_GET ? "" : NULL;
    switch (action)
""")
    source = replace(source, """            if (line > line_count)
                return &buffer[0];
""", "")
    source = replace(source, """            if (line > line_count)
                return NULL;
""", "", count=3)
    source = replace(source, """            line_count--;
            break;""", """            line_count--;
            last_action_line = last_char_index = 0;
            return buffer;""", count=2)
    source = replace(source, """            rb->memmove(&buffer[c-1],&buffer[c],char_count-c);
""", """            if (!c)
                return NULL;
            int previous = c - 1;
            while (previous > 0 && buffer[previous - 1])
                previous--;
            if ((size_t)(c - previous - 1) + rb->strlen(&buffer[c])
                >= MAX_LINE_LEN)
                return NULL;
            rb->memmove(&buffer[c-1],&buffer[c],char_count-c);
""")
    source = replace(source, "&buf[buf_len-10]", "&buf[buf_len-5]")
    source = replace(source, "if (rb->strlcpy(buf, b, buf_len) >= buf_len)",
                     "if (rb->strlcpy(buf, b, buf_len) >= buf_len && buf_len >= 5)")
    source = replace(source, """    rb->gui_synclist_set_nb_items(lists,line_count);""",
                     """    rb->gui_synclist_set_title(lists, "Text Editor", Icon_Plugin);
    rb->gui_synclist_set_nb_items(lists,line_count);""")
    source = replace(source, """            if (!audio_buf)
                playback_control(NULL);
            else
                rb->splash(HZ, "Cannot restart playback");""",
                     """            playback_control(NULL);""")
    source = replace(source, """                            if (!audio_buf)
                                playback_control(NULL);
                            else
                                rb->splash(HZ, "Cannot restart playback");""",
                     """                            playback_control(NULL);""")
    for pos in ("cur_sel", "cur_sel+1"):
        source = replace(source, f"""                do_action(ACTION_INSERT,copy_buffer,{pos});
                copy_buffer[0]='\\0';
                ret = MENU_RET_UPDATE;""",
                         f"""                if (do_action(ACTION_INSERT, copy_buffer,
                              {pos if pos == "cur_sel" else "line_count ? cur_sel + 1 : 0"}))
                {{
                    copy_buffer[0] = '\\0';
                    ret = MENU_RET_UPDATE;
                }}
                else
                    rb->splash(HZ, "Document exceeds plugin memory");""")
    source = replace(source, """                do_action(ACTION_CONCAT,0,cur_sel);
                ret = MENU_RET_UPDATE;""",
                     """                if (do_action(ACTION_CONCAT, NULL, cur_sel))
                    ret = MENU_RET_UPDATE;
                else
                    rb->splash(HZ, "Joined line is too long");""")
    source = between(source, "#ifdef HAVE_LCD_COLOR\n\n#define hex2dec",
                     "/* this is the plugin entry point */", "")
    source = replace(source, "    int fd;\n\n    bool exit", "    bool exit")
    source = between(source, "#ifdef HAVE_LCD_COLOR\n    bool edit_colors_file",
                     "    /* now dump it in the list */",
                     r"""    copy_buffer[0] = '\0';
    temp_line[0] = '\0';
    buffer = rb->plugin_get_buffer(&buffer_size);
    if (!buffer || !buffer_size)
        return PLUGIN_ERROR;

    char path[MAX_PATH];
    if (!parameter)
    {
        MENUITEM_STRINGLIST(menu, "Text Editor", NULL,
                            "Open Text File", "New File", "Quit");
        int selected = rb->do_menu(&menu, NULL, NULL, false);
        if (selected == 0)
        {
            if (rb->pick_file(path, sizeof(path), "/", "Choose text to edit",
                              ".txt.md", 0, false) <= 0)
                return rb->plugin_usb_taken() ? PLUGIN_USB_CONNECTED : PLUGIN_OK;
            parameter = path;
        }
        else if (selected != 1)
            return rb->plugin_usb_taken() ? PLUGIN_USB_CONNECTED : PLUGIN_OK;
    }

    if (parameter)
    {
        if (rb->strlcpy(filename, parameter, sizeof(filename)) >= sizeof(filename)
            || !load_document(filename))
        {
            rb->splash(HZ * 2, "Invalid text or document exceeds editor limits");
            return PLUGIN_ERROR;
        }
        newfile = false;
    }
    else
    {
        rb->strcpy(filename, "/");
        rb->strcpy(eol, "\n");
        newfile = true;
    }

""")
    source = between(source, "#ifdef HAVE_LCD_COLOR\n                if (edit_colors_file",
                     "            }\n            break;\n            case ACTION_STD_CONTEXT:",
                     r"""                else
                    temp_line[0] = '\0';
                if (!rb->kbd_input(temp_line, sizeof(temp_line), NULL))
                {
                    int action = line_count ? ACTION_UPDATE : ACTION_INSERT;
                    if (do_action(action, temp_line, cur_sel))
                        changed = true;
                    else
                        rb->splash(HZ, "Document exceeds plugin memory");
                }
""")
    source = between(source, "            case ACTION_STD_CONTEXT:",
                     "            {\n                rb->gui_synclist_scroll_stop",
                     """            case ACTION_STD_CONTEXT:
""")
    source = replace(source, "            case ACTION_STD_CANCEL:\n",
                     "            case ACTION_STD_MENU:\n            case ACTION_STD_CANCEL:\n")
    source = replace(source, "rb->kbd_input(copy_buffer,MAX_LINE_LEN, NULL)",
                     "edit_text(copy_buffer, sizeof(copy_buffer))", count=2)
    source = replace(source, "rb->kbd_input(temp_line, sizeof(temp_line), NULL)",
                     "edit_text(temp_line, sizeof(temp_line))")
    source = replace(source, "    while (!exit)\n",
                     "    while (!exit && !rb->plugin_usb_taken())\n")
    source = replace(source, """        if (rb->gui_synclist_do_button(&lists, &button))""",
                     """        if (IS_SYSEVENT(button) &&
            rb->default_event_handler(button) == SYS_USB_CONNECTED)
            break;
        if (rb->gui_synclist_do_button(&lists, &button))""")
    return replace(source, "    return PLUGIN_OK;\n}",
                   "    return rb->plugin_usb_taken() ? PLUGIN_USB_CONNECTED : PLUGIN_OK;\n}")


def speedread(source):
    source = replace(source, """    if(rb->utf8length(word) > 13)
        return 4;
    else
    {
        int tab[] = {0,1,1,1,1,2,2,2,2,3,3,3,3};
        return tab[len - 1];
    }""", """    static const int table[] = {0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3};
    return len <= 1 ? 0 : (len > (int)ARRAYLEN(table) ? 4 : table[len - 1]);""")
    source = replace(source, 'rb->utf8length(word)',
                     'rb->utf8length((const unsigned char *)word)', count=3)
    source = source.replace("rb->utf8seek(word,", "rb->utf8seek((const unsigned char *)word,")
    source = replace(source, """    rb->strlcpy(half, word, rb->utf8seek((const unsigned char *)word, focus + 1));""",
                     """    rb->strlcpy(half, word, MIN(sizeof(half), (size_t)idx + 1));""")
    source = replace(source, """    const char *word = get_last_word();

    int focus""", """    const char *word = get_last_word();
    if (!word)
        return;

    int focus""")
    source = replace(source, """        rb->lcd_update_rect(0, 0, LCD_WIDTH, h);
    }""", """        rb->lcd_update_rect(0, 0, LCD_WIDTH, h);
        rb->sleep(1);
    }""")
    source = replace(source, "    atexit(cleanup);\n\n", "")
    source = replace(source, """    case PLA_CANCEL:
        return QUIT;""", """#ifdef HAVE_SCROLLWHEEL
    case PLA_UP:
#endif
    case PLA_CANCEL:
        return QUIT;""")
    source = replace(source, 'rb->splash(0, "Loaded bookmark.");',
                     'rb->splash(HZ, "Loaded bookmark.");')
    source = replace(source, """    if(response == YESNO_NO)
        return false;
    else
        return true;""", """    return response == YESNO_YES;""")
    source = replace(source, """    while(!rc)
""", """    while (!rc && !rb->plugin_usb_taken())
""")
    source = replace(source, """        default:
            break;
        }
    }
    return rc - 1;""", """        default:
            rc = 1;
            break;
        }
    }
    return rc != 1;""")
    source = replace(source, """        if(config_menu())
        {
            save_bookmark(fname, *wpm);
            return FINISH;
        }""", """        if (config_menu())
        {
            if (!rb->plugin_usb_taken() && !save_bookmark(fname, *wpm))
                rb->splash(HZ, "Reading position not saved");
            return FINISH;
        }""")
    source = replace(source, """            rb->lseek(fd, offs, SEEK_CUR);
""", """            off_t target = MAX(begin_offs, MIN(base_offs + offs, file_size));
            rb->lseek(fd, target, SEEK_SET);
""", count=2)
    start = source.index("    case BACK:\n        if(fd >= 0)")
    source = source[:start] + replace(source[start:],
                                     "while(get_useraction() == FFWD",
                                     "while(get_useraction() == BACK")
    source = between(source, "static void save_bookmark",
                     "static bool confirm_restart", adapter("speedread-io.inc"))
    source = replace(source, 'rb->strtok_r(line_buf, " ", &end)',
                     'rb->strtok_r(line_buf, " \\t", &end)')
    source = replace(source, 'char *word = rb->strtok_r(NULL, " ", &end)',
                     'char *word = rb->strtok_r(NULL, " \\t", &end)')
    source = between(source, "    const char *fname = param;", "    init_drawing();",
                     """    const char *fname = param;
    char selected[MAX_PATH];
    if (!fname)
    {
        if (rb->pick_file(selected, sizeof(selected), "/", "Choose text to read",
                          ".txt.md", 0, false) <= 0)
            return rb->plugin_usb_taken() ? PLUGIN_USB_CONNECTED : PLUGIN_OK;
        fname = selected;
    }

    fd = open_text(fname, &begin_offs);
    off_t file_size = fd < 0 ? -1 : rb->ffilesize(fd);
    if (file_size < 0)
    {
        if (fd >= 0)
            rb->close(fd);
        rb->splash(HZ, "Unable to open document");
        return PLUGIN_ERROR;
    }

    atexit(cleanup);
    load_font();
    int wpm = DEF_WPM;
    bool loaded = load_bookmark(fname, file_size, &wpm);

""")
    source = replace(source, "    return PLUGIN_OK;\n}",
                     "    return rb->plugin_usb_taken() ? PLUGIN_USB_CONNECTED : PLUGIN_OK;\n}")
    return source

def otp(source):
    source = between(source, "/***********************************************************************\n * File browser",
                     "static void add_acct_manual(void)", adapter("otp-io.inc"))
    source = replace(source, "    unsigned long totp_period;", "    uint32_t totp_period;")
    source = replace(source, "    unsigned char hash[20];",
                     "    _Alignas(uint32_t) unsigned char hash[20];")
    source = replace(source, "HOTP(\"12345678901234567890\",",
                     "HOTP((unsigned char *)\"12345678901234567890\",")
    source = replace(source, "  int buffer = 0;", "  uint32_t buffer = 0;")
    source = replace(source, "    int buffer = data[0];",
                     "    uint32_t buffer = data[0];")
    source = replace(source, "  int count = 0;\n  for (const uint8_t *ptr",
                     "  int count = 0;\n  bool padding = false;\n  for (const uint8_t *ptr")
    source = replace(source, "count < bufSize && *ptr", "*ptr")
    source = replace(source, "    buffer <<= 5;\n", """    if (ch == '=')
    {
        padding = true;
        continue;
    }
    if (padding)
        return -1;
    buffer <<= 5;
""")
    source = replace(source, "      result[count++] = buffer >> (bitsLeft - 8);",
                     """      if (count == bufSize)
          return -1;
      result[count++] = buffer >> (bitsLeft - 8);""")
    source = replace(source, """    if(acct_exists(buf))
""", """    if (!*buf || acct_exists(buf))
""")
    source = replace(source, "base32_decode(accounts[next_slot].secret, SECRET_MAX, temp_buf)",
                     "base32_decode(accounts[next_slot].secret, SECRET_MAX,\n"
                     "                     (const uint8_t *)temp_buf)")
    source = replace(source, "    if(response == YESNO_NO)\n",
                     "    if (response == YESNO_USB)\n        return;\n    if(response == YESNO_NO)\n")
    source = replace(source, """    if(!accounts[next_slot].is_totp)
        accounts[next_slot].hotp_counter = rb->atoi(temp_buf);
    else
        accounts[next_slot].totp_period = rb->atoi(temp_buf);""", """    uintmax_t number;
    if (!parse_unsigned(temp_buf, accounts[next_slot].is_totp ?
                       UINT32_MAX : UINT64_MAX, &number)
        || (accounts[next_slot].is_totp && !number))
    {
        rb->splash(HZ, "Invalid period or counter");
        return;
    }
    if (accounts[next_slot].is_totp)
        accounts[next_slot].totp_period = number;
    else
        accounts[next_slot].hotp_counter = number;""")
    source = replace(source, """    accounts[next_slot].digits = rb->atoi(temp_buf);

    if(accounts[next_slot].digits < 1 || accounts[next_slot].digits > 9)""",
                     """    if (!parse_unsigned(temp_buf, 9, &number) || !number)""")
    source = replace(source, """    ++next_slot;

    save_accts();

    rb->splashf(HZ * 2, "Success.");""", """    accounts[next_slot++].digits = number;
    if (save_accts())
        rb->splash(HZ, "Account saved");""")
    source = replace(source, '#include "lib/pluginlib_actions.h"\n', "")
    source = between(source, "static void show_code(int acct)",
                     "static bool danger_confirm", adapter("otp-ui.inc"))
    source = between(source, "static void edit_accts(void)", "#if CONFIG_RTC",
                     """static void edit_accts(void)
{
    choose_account("Edit Account", edit_menu);
}

""")
    source = replace(source, "if(acct_exists(data_buf))",
                     "if (!*data_buf || acct_exists(data_buf))")
    source = replace(source, """            if(rb->kbd_input(data_buf, sizeof(data_buf), NULL) < 0)
                break;
            if (!*data_buf""", """            if (rb->kbd_input(data_buf, MAX_NAME, NULL) < 0)
                break;
            if (!*data_buf""")
    source = replace(source, """                save_accts();
                rb->splashf(HZ, "Deleted.");""",
                     """                if (save_accts())
                    rb->splash(HZ, "Deleted");""")
    source = between(source, "        case 2: // HOTP counter OR TOTP period",
                     "        case 4: // secret", """        case 2:
        {
            struct account_t *account = &accounts[acct];
            uint64_t current = account->is_totp ? account->totp_period
                                              : account->hotp_counter;
            rb->snprintf(data_buf, sizeof(data_buf), "%llu",
                         (unsigned long long)current);
            if (rb->kbd_input(data_buf, sizeof(data_buf), NULL) < 0)
                break;

            uintmax_t number;
            if (!parse_unsigned(data_buf, account->is_totp ?
                               UINT32_MAX : UINT64_MAX, &number)
                || (account->is_totp && !number))
            {
                rb->splash(HZ, "Invalid period or counter");
                break;
            }
            if (account->is_totp)
                account->totp_period = number;
            else
                account->hotp_counter = number;
            save_accts();
            break;
        }
        case 3:
        {
            rb->snprintf(data_buf, sizeof(data_buf), "%d", accounts[acct].digits);
            if (rb->kbd_input(data_buf, sizeof(data_buf), NULL) < 0)
                break;
            uintmax_t number;
            if (!parse_unsigned(data_buf, 9, &number) || !number)
            {
                rb->splash(HZ, "Invalid digit count");
                break;
            }
            accounts[acct].digits = number;
            save_accts();
            break;
        }
""")
    source = replace(source, "base32_encode(accounts[acct].secret, accounts[acct].sec_len, data_buf, sizeof(data_buf));",
                     "base32_encode(accounts[acct].secret, accounts[acct].sec_len,\n"
                     "                          (uint8_t *)data_buf, sizeof(data_buf));")
    source = replace(source, "base32_decode(accounts[acct].secret, sizeof(accounts[acct].secret), data_buf);",
                     "base32_decode(accounts[acct].secret, sizeof(accounts[acct].secret),\n"
                     "                                    (const uint8_t *)data_buf);")
    source = replace(source, "            rb->splash(HZ, \"Success.\");\n", "")
    source = replace(source, "return 3600 * hr + 60 * min;",
                     "return 3600 * hr + 60 * min * (label[3] == '-' ? -1 : 1);")
    source = replace(source, """    /* relies on menu internals */
    rb->do_menu(&menu, &sel, NULL, false);

    /* see apps/menu.h */
    const char *label = menu_[sel];

    return get_time_seconds(label);""", """    int selected = rb->do_menu(&menu, &sel, NULL, false);
    return selected >= 0 && (size_t)selected < ARRAYLEN(menu_)
           ? get_time_seconds(menu_[selected]) : time_offs;""")
    source = replace(source, """                save_accts();
                rb->splash(HZ, "It is done, my master.");""",
                     """                if (save_accts())
                    rb->splash(HZ, "Accounts deleted");""")
    source = replace(source, """            time_offs = get_time_offs();
            break;""", """        {
            int offset = get_time_offs();
            if (offset != time_offs && !rb->plugin_usb_taken())
            {
                time_offs = offset;
                save_accts();
            }
            break;
        }""")
    source = replace(source, "    while(!quit)\n", "    while (!quit && !save_failed && !rb->plugin_usb_taken())\n", count=4)
    source = replace(source, """        case 5:
            quit = true;
            break;
        default:
            break;
""", """        case 5:
        default:
            quit = true;
            break;
""")
    source = replace(source, """            quit = 1;
            break;
        default:
            break;
""", """        default:
            quit = 1;
            break;
""", count=2)
    source = replace(source, "enum plugin_status plugin_start(const void* parameter)",
                     """static void wipe_accounts(void)
{
    rb->memset(accounts, 0, max_accts * sizeof(*accounts));
    rb->memset(data_buf, 0, sizeof(data_buf));
    rb->memset(temp_sec, 0, sizeof(temp_sec));
}

enum plugin_status plugin_start(const void* parameter)""")
    source = replace(source, """    max_accts = bufsz / sizeof(struct account_t);

    if(!read_accts())""", """    max_accts = bufsz / sizeof(struct account_t);
    if (!accounts || max_accts < 1)
        return PLUGIN_ERROR;

    atexit(wipe_accounts);
    int loaded = read_accts();
    if (loaded < 0)
    {
        rb->splash(HZ * 2, "Invalid OTP account data");
        return PLUGIN_ERROR;
    }
    if (!loaded)""")
    return replace(source, """    /* save to disk */
    save_accts();

    /* tell Rockbox that we have completed successfully */
    return PLUGIN_OK;""", """    if (rb->plugin_usb_taken())
        return PLUGIN_USB_CONNECTED;
    return save_failed ? PLUGIN_ERROR : PLUGIN_OK;""")


if __name__ == "__main__":
    src, dst = map(Path, sys.argv[1:])
    converters = {"sort": sort, "text_editor": text_editor,
                  "speedread": speedread, "otp": otp}
    data = src.read_bytes()
    if sha256(data).hexdigest() != SOURCE_DIGESTS[src.stem]:
        raise SystemExit(f"{src}: review the AstraPod file adapter for this source")
    dst.write_text(converters[src.stem](data.decode()))
