#!/usr/bin/env python3
"""Stage the Pacbox foreground adapter. GNU GPL version 2 or later."""
from pathlib import Path
import sys


def replace(text, before, after):
    if text.count(before) != 1:
        raise ValueError(f"expected one Pacbox source match: {before!r}")
    return text.replace(before, after, 1)


def stage(text, preferences):
    text = replace(text, '#include "lib/playback_control.h"\n', "")
    text = replace(text, '#define SETTINGS_MIN_VERSION 2\n'
                         '#define SETTINGS_FILENAME "pacbox.cfg"\n', "")
    text = replace(text, 'static bool sound_playing = false;\n', "")
    text = replace(text, 'static bool loadFile(', preferences + '\nstatic bool loadFile(')
    text = replace(text, '''    int n = rb->read( fd, buf, len);

    rb->close( fd );

    if( n != len ) {
        return false;
    }

    return true;''', '''    bool complete = rb->ffilesize(fd) == len && rb->read(fd, buf, len) == len;
    return rb->close(fd) == 0 && complete;''')
    text = replace(text, '            rb->set_option("Sound",&settings.sound, RB_INT,\n'
                         '                               noyes, 2, NULL);',
                         '            rb->splash(HZ, "Sound unavailable in this build");')
    text = replace(text, '    return (result==PBMI_QUIT);',
                         '    return result == PBMI_QUIT || result == MENU_ATTACHED_USB;')
    start = text.index('/* Sound is emulated in ISR context')
    end = text.index('/* use buttons for joystick */', start)
    text = text[:start] + text[end:]
    text = replace(text, '    rb->button_clear_queue();\n    /*handle buttons if AI is off */',
                         '    /*handle buttons if AI is off */')
    text = replace(text, 'if((turn != 0) && (turn !=209))',
                         'if (turn > 0 && turn < ARRAYLEN(ai_location[0]))')
    text = replace(text, 'if( ram_[0x4D31] == (ai_location[level][turn] + 30))',
                         'if (turn < ARRAYLEN(ai_location[0]) &&\n'
                         '                    ram_[0x4D31] == ai_location[level][turn] + 30)')
    text = replace(text, 'static int gameProc( void )',
                         'static enum plugin_status gameProc(void)')
    text = replace(text, '    if (settings.sound)\n        start_sound();\n\n', '')
    text = replace(text, '''        status = rb->button_status();
        rb->button_clear_queue();''', '''        long event = rb->button_get(false);
        if (rb->default_event_handler(event) == SYS_USB_CONNECTED)
            return PLUGIN_USB_CONNECTED;
        status = rb->button_status();''')
    text = replace(text, '''            stop_sound();

            menu_res = pacbox_menu();''', '''            menu_res = pacbox_menu();
            if (rb->plugin_usb_taken())
                return PLUGIN_USB_CONNECTED;''')
    text = replace(text, '''            if (menu_res)
                return 1;

            if (settings.sound)
                start_sound();''', '''            if (menu_res)
                return PLUGIN_OK;''')
    text = replace(text, '''    }

    stop_sound();

    return 0;
}

enum plugin_status plugin_start''', '''    }
}

enum plugin_status plugin_start''')
    start = text.index('    if (configfile_load(')
    end = text.index('    /* Keep a copy', start)
    text = text[:start] + '    pacbox_load_settings();\n\n' + text[end:]
    text = replace(text, '    /* Load the romset */\n',
                         '    enum plugin_status result = PLUGIN_OK;\n\n'
                         '    /* Load the romset */\n')
    text = replace(text, '''        gameProc();

        /* Save the user settings if they have changed */
        if (rb->memcmp(&settings,&old_settings,sizeof(settings))!=0) {
            rb->splash(0, "Saving settings...");
            configfile_save(SETTINGS_FILENAME, config,
                            sizeof(config)/sizeof(*config),
                            SETTINGS_VERSION);
        }''', '''        result = gameProc();

        if (result == PLUGIN_OK &&
            rb->memcmp(&settings, &old_settings, sizeof(settings)) &&
            !pacbox_save_settings())
            rb->splash(HZ, "Could not save Pacbox settings");''')
    text = replace(text, '        rb->splashf(HZ*2, "No ROMs in %s/pacman/", ROCKBOX_DIR);',
                         '''        result = rb->view_text(
            "Pacbox ROM files",
            "Place six compatible 4 KiB ROM files in " ROCKBOX_DIR "/pacman/.");''')
    return replace(text, '    backlight_use_settings();\n\n    return PLUGIN_OK;',
                         '    backlight_use_settings();\n\n    return result;')


if __name__ == "__main__":
    source, preferences, output = map(Path, sys.argv[1:])
    output.write_text(stage(source.read_text(), preferences.read_text()))
