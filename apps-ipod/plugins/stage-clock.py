#!/usr/bin/env python3
"""Stage the Clock plugin against AstraPod's foreground SDK."""
from pathlib import Path
import sys


def replace(text, before, after):
    if text.count(before) != 1:
        raise SystemExit(f"review Clock adapter for {before!r}")
    return text.replace(before, after)


source, target = map(Path, sys.argv[1:3])
text = source.read_text()
if source.name == "clock.c":
    text = replace(text, "    if(clock_settings.general.save_settings == 1)",
                   "    if (!rb->plugin_usb_taken() &&\n"
                   "        clock_settings.general.save_settings)")
    text = replace(text, "                exit_clock=main_menu();",
                   "                exit_clock=main_menu();\n"
                   "                if (rb->plugin_usb_taken())\n"
                   "                    return PLUGIN_USB_CONNECTED;")
elif source.name == "clock_menu.c":
    text = replace(text, "            case 7:\n", "            case 7:\n            {\n")
    text = replace(text, "                    return 1;\n                }\n                break;",
                   "                    return 1;\n                }\n                break;\n            }")
    text = replace(text, "                rb->splash(HZ, ID2P(LANG_SETTINGS_SAVED));\n", "")
    text = replace(text, "                if (rb->yesno_pop_confirm(ID2P(LANG_RESET)))",
                   """                static const char *lines[] = {
                    ID2P(LANG_RESET)
                };
                static const struct text_message prompt = {
                    .message_lines = lines, .nb_lines = ARRAYLEN(lines)
                };
                if (rb->gui_syncyesno_run(&prompt, NULL, NULL) == YESNO_YES)""")
elif source.name == "clock_settings.c":
    text = replace(text, "    rb->storage_sleep();\n", "")
    text = replace(text, "static enum settings_file_status clock_settings_load(",
                   """static bool valid_settings(const unsigned char *bytes)
{
    static const size_t booleans[] = {
        offsetof(struct clock_settings, general.save_settings),
        offsetof(struct clock_settings, general.idle_poweroff),
        offsetof(struct clock_settings, analog.show_date),
        offsetof(struct clock_settings, analog.show_seconds),
        offsetof(struct clock_settings, analog.show_border),
    };

    for (unsigned i = 0; i < ARRAYLEN(booleans); ++i)
        if (bytes[booleans[i]] > 1)
            return false;

    struct clock_settings candidate;
    rb->memcpy(&candidate, bytes, sizeof(candidate));
    if ((unsigned)candidate.mode >= NB_CLOCK_MODES ||
        (unsigned)candidate.general.date_format > JAPANESE ||
        (unsigned)candidate.general.backlight > ALWAYS_ON ||
        (unsigned)candidate.digital.show_seconds > 1 ||
        (unsigned)candidate.digital.blinkcolon > 1)
        return false;

    for (unsigned i = 0; i < NB_CLOCK_MODES; ++i)
        if ((unsigned)candidate.skin[i] >= (unsigned)max_skin[i])
            return false;
    return true;
}

static enum settings_file_status clock_settings_load(""")
    begin = text.index("    int fd = rb->open(filename, O_RDONLY);")
    end = text.index("    /* Initializes the settings", begin)
    text = text[:begin] + """    unsigned char bytes[sizeof(*settings)];
    int fd = rb->open(filename, O_RDONLY);
    if (fd >= 0)
    {
        bool read_ok = rb->ffilesize(fd) == sizeof(bytes) &&
                       rb->read(fd, bytes, sizeof(bytes)) == sizeof(bytes);
        if (rb->close(fd) == 0 && read_ok && valid_settings(bytes))
        {
            rb->memcpy(settings, bytes, sizeof(bytes));
            apply_backlight_setting(settings->general.backlight);
            rb->memcpy(&hdd_clock_settings, settings, sizeof(*settings));
            return LOADED;
        }
    }
""" + text[end:]
    begin = text.index("    int fd = rb->creat(filename, 0666);")
    end = text.index("\n}\n", begin)
    text = text[:begin] + """    char temp[MAX_PATH];
    int len = rb->snprintf(temp, sizeof(temp), "%s.tmp", filename);
    if (len < 0 || (size_t)len >= sizeof(temp))
        return ERRSAVE;

    int fd = rb->open(temp, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (fd < 0)
        return ERRSAVE;

    bool written = rb->write(fd, settings, sizeof(*settings)) ==
                   sizeof(*settings) && rb->file_sync(fd) == 0;
    if (rb->close(fd) < 0)
        written = false;
    if (!written || rb->rename(temp, filename) < 0)
    {
        rb->remove(temp);
        return ERRSAVE;
    }

    rb->memcpy(&hdd_clock_settings, settings, sizeof(*settings));
    return SAVED;""" + text[end:]
    text = replace(text, "    clock_settings_save(&clock_settings, settings_filename);\n}",
                   "    save_settings();\n}")
else:
    raise SystemExit(f"unexpected Clock source: {source}")
target.write_text(text)
