#!/usr/bin/env python3
"""Stage AstraPod SDK adapters for upstream games. GPL-2.0-or-later."""
import sys
from pathlib import Path

src, out = map(Path, sys.argv[1:])
text = src.read_text()

def replace(old, new, count=1):
    global text
    if text.count(old) != count:
        raise SystemExit(f"{src}: refresh source conversion for {old!r}")
    text = text.replace(old, new)

if src.stem == "chip8":
    # The iPod wheel posts motion events without a corresponding release.
    replace(
        "    int button = rb->button_get(false);\n    switch (button)",
        "    int button = rb->button_get(false);\n"
        "    chip8_virtual_keys[2] = 0;\n"
        "    chip8_virtual_keys[8] = 0;\n"
        "    switch (button)",
    )
    for key in ("2", "8"):
        replace(
            f"    case CHIP8_KEY{key}:                chip8_virtual_keys[{key}] = 1; break;\n"
            f"    case CHIP8_KEY{key} | BUTTON_REL:   chip8_virtual_keys[{key}] = 0; break;",
            f"    case CHIP8_KEY{key} | BUTTON_REPEAT:\n"
            f"    case CHIP8_KEY{key}: chip8_virtual_keys[{key}] = 1; break;",
        )

    # Keep the ROM read within the guest program region and reject short reads.
    replace("    int numread;\n    int fd;\n    int len;", "    int fd;")
    replace(
        "    numread = rb->read(fd, chip8_mem+0x200, 4096-0x200);\n"
        "    rb->close(fd);\n"
        "    if (numread==-1) {",
        "    off_t size = rb->ffilesize(fd);\n"
        "    size_t capacity = sizeof(chip8_mem) - 0x200;\n"
        "    bool ok = false;\n"
        "    if (size >= 2 && size <= (off_t)capacity) {\n"
        "        memset(chip8_mem + 0x200, 0, capacity);\n"
        "        ok = rb->read(fd, chip8_mem + 0x200, size) == size;\n"
        "    }\n"
        "    rb->close(fd);\n"
        "    if (!ok) {",
    )
    replace("    rb->strcpy(c8kname, file);",
            '    size_t len = rb->strlcpy(c8kname, file, sizeof(c8kname));')
    replace(
        "    len = rb->strlen(c8kname);\n"
        "    c8kname[len-2] = '8';\n"
        "    c8kname[len-1] = 'k';",
        '    memcpy(c8kname + len - 4, ".c8k", sizeof(".c8k"));',
    )
    replace(
        '        rb->lcd_puts(0, 6, "File&Keymap OK.");\n'
        "        numread = rb->read(fd, chip8_keymap, 16);\n"
        "        rb->close(fd);",
        "        byte map[16];\n"
        "        ssize_t count = rb->read(fd, map, sizeof(map));\n"
        "        bool complete = count == (ssize_t)sizeof(map);\n"
        "        rb->close(fd);\n"
        "        if (complete)\n"
        "            memcpy(chip8_keymap, map, sizeof(map));\n"
        '        rb->lcd_puts(0, 6, complete ? "File&Keymap OK." : "File OK.");',
    )

    # Service native events during the upstream credits and load-error screen.
    replace(
        "    int ok;\n\n    rb->lcd_clear_display();",
        "    rb->lcd_setfont(FONT_SYSFIXED);\n"
        "    rb->lcd_clear_display();",
    )
    replace(
        "    ok = chip8_init(file);\n"
        "    rb->lcd_update();\n"
        "    rb->sleep(HZ*1);\n"
        "    if (!ok)",
        "    bool ok = chip8_init(file);\n"
        "    rb->lcd_update();\n"
        "    long deadline = *rb->current_tick + HZ;\n"
        "    chip8_running = 1;\n"
        "    while (chip8_running == 1 && TIME_BEFORE(*rb->current_tick, deadline)) {\n"
        "        chip8_keyboard();\n"
        "        rb->sleep(1);\n"
        "    }\n"
        "    if (chip8_running != 1)\n"
        "        return true;\n"
        "    if (!ok)",
    )
    replace(
        "    const char* filename;\n\n"
        "    if (parameter == NULL)\n"
        "    {\n"
        '        rb->splash(HZ, "Play a .ch8 file!");\n'
        "        return PLUGIN_ERROR;\n"
        "    }\n"
        "    else\n"
        "    {\n"
        "        filename = (char*) parameter;\n"
        "    }",
        "    const char* filename = parameter;\n"
        "    size_t len = filename ? rb->strlen(filename) : 0;\n"
        "    if (len < 4 || len >= MAX_PATH ||\n"
        '        rb->strcasecmp(filename + len - 4, ".ch8"))\n'
        "    {\n"
        '        rb->splash(HZ, "Open a .ch8 file in Files.");\n'
        "        return rb->plugin_usb_taken() ? PLUGIN_USB_CONNECTED : PLUGIN_OK;\n"
        "    }",
    )
elif src.stem == "blackjack":
    replace('#include "lib/playback_control.h"',
            '#include "lib/playback_control.h"\n#include "lib/pluginlib_exit.h"')
    replace("\n        button = rb->button_get(true);",
            "\n        button = rb->button_get(true);\n"
            "        exit_on_usb(button);", count=2)  # dialog loops
    replace("            button = rb->button_get(true);",
            "            button = rb->button_get(true);\n"
            "            exit_on_usb(button);")
    # The wager box updates only its own rectangle. Present the table first.
    replace("        bj->current_bet = 10;\n        blackjack_get_bet(bj);",
            "        bj->current_bet = 10;\n"
            "        rb->lcd_clear_display();\n"
            "        rb->lcd_update();\n"
            "        blackjack_get_bet(bj);")
elif src.stem == "bubbles":
    replace("static unsigned int startlevel = 0;", "static int startlevel = 0;")
    replace("    if (rb->button_hold())",
            "    if (!IS_SYSEVENT(button) && rb->button_hold())")
    replace(
        "            while(pluginlib_getaction(TIMEOUT_BLOCK,plugin_contexts,\n"
        "                    ARRAYLEN(plugin_contexts))\n"
        "                 != BUBBLES_PAUSE);",
        "            do {\n"
        "                button = pluginlib_getaction(TIMEOUT_BLOCK, plugin_contexts,\n"
        "                                            ARRAYLEN(plugin_contexts));\n"
        "                if (rb->default_event_handler_ex(button, bubbles_callback, bb)\n"
        "                    == SYS_USB_CONNECTED)\n"
        "                    return BB_USB;\n"
        "            } while (button != BUBBLES_PAUSE);",
    )
    replace(
        "                rb->set_int(\"Choose start level\", \"\", UNIT_INT, &startlevel,\n"
        "                            NULL, 1, 1, highlevel+1, NULL);",
        "                if (rb->set_int(\"Choose start level\", \"\", UNIT_INT, &startlevel,\n"
        "                                NULL, 1, 1, highlevel+1, NULL)) {\n"
        "                    startlevel--;\n"
        "                    return BB_USB;\n"
        "                }",
    )
    replace("                playback_control(NULL);",
            "                if (playback_control(NULL))\n"
            "                    return BB_USB;")
elif src.stem == "clix":
    replace("                playback_control(NULL);",
            "                if (playback_control(NULL))\n"
            "                    return 1;")
    replace(
        "            button = rb->button_get_w_tmo(end - *rb->current_tick);",
        "            button = rb->button_get_w_tmo(end - *rb->current_tick);\n"
        "            if (IS_SYSEVENT(button) &&\n"
        "                rb->default_event_handler(button) == SYS_USB_CONNECTED)\n"
        "                return 1;",
    )
    replace("    clix_handle_game( &state);",
            "    clix_handle_game( &state);\n"
            "    if (rb->plugin_usb_taken())\n"
            "        return PLUGIN_USB_CONNECTED;")
elif src.stem == "invadrox":
    replace("    unsigned int released, pressed, newbuttonstate;",
            "    unsigned int released, pressed, newbuttonstate;\n"
            "    int event;")
    replace(
        "    if (rb->button_get(false) == SYS_USB_CONNECTED) {",
        "    event = rb->button_get(false);\n"
        "    if (IS_SYSEVENT(event) &&\n"
        "        rb->default_event_handler(event) == SYS_USB_CONNECTED) {",
    )
    replace("    game_loop();",
            "    game_loop();\n"
            "    if (rb->plugin_usb_taken())\n"
            "        return PLUGIN_USB_CONNECTED;")
elif src.stem == "jewels":
    replace("    unsigned int tmp_type;", "    int tmp_type;")
    replace('                rb->set_option("Mode", &bj->tmp_type, RB_INT, mode, 2, NULL);',
            '                if (rb->set_option("Mode", &bj->tmp_type, RB_INT, mode, 2, NULL))\n'
            '                    return 1;')
    replace("                playback_control(NULL);",
            "                if (playback_control(NULL))\n"
            "                    return 1;")
elif src.stem == "pegbox":
    replace('#include "lib/playback_control.h"',
            '#include "lib/playback_control.h"\n#include "lib/pluginlib_exit.h"')
    replace("    unsigned int level;\n    unsigned int highlevel;",
            "    int level;\n    int highlevel;")
    replace("        button = rb->button_get(true);",
            "        button = rb->button_get(true);\n"
            "        exit_on_usb(button);", count=2)
    replace("    pegbox_main(&pb);",
            "    pegbox_main(&pb);\n"
            "    if (rb->plugin_usb_taken())\n"
            "        return PLUGIN_USB_CONNECTED;")
elif src.stem == "rockblox":
    replace("            while (rb->button_hold ())\n"
            "                rb->sleep(HZ/10);",
            "            while (rb->button_hold ()) {\n"
            "                int event = rb->button_get_w_tmo(HZ/10);\n"
            "                if (IS_SYSEVENT(event) &&\n"
            "                    rb->default_event_handler(event) == SYS_USB_CONNECTED)\n"
            "                    return PLUGIN_USB_CONNECTED;\n"
            "            }")
elif src.stem == "solitaire":
    replace("                 playback_control(NULL);",
            "                if (playback_control(NULL))\n"
            "                    result = MENU_USB;")
elif src.stem == "spacerocks":
    replace("                playback_control(NULL);",
            "                if (playback_control(NULL))\n"
            "                    return PLUGIN_USB_CONNECTED;")
elif src.stem == "wormlet":
    replace('#include "lib/playback_control.h"',
            '#include "lib/playback_control.h"\n#include "lib/pluginlib_exit.h"')
    replace("        button = rb->button_get_w_tmo(speed - cycle_duration);",
            "        button = rb->button_get_w_tmo(speed - cycle_duration);\n"
            "        exit_on_usb(button);")
    replace("    int result;\n    int menu_quit", "    int result = 0;\n    int menu_quit")
    # The native RB_INT option reads and writes an int, including on hosted builds.
    replace("case 3:\n                switch(players)",
            "case 3:\n                new_setting = 0;\n                switch(players)")
    replace("&use_remote, RB_INT,", "&new_setting, RB_INT,", count=5)
    replace("                    case 1:\n                        rb->set_option",
            "                    case 1:\n"
            "                        new_setting = use_remote;\n"
            "                        rb->set_option")
    replace('                        rb->set_option(rb->str(LANG_CONTROL_STYLE),&new_setting, RB_INT,\n'
            '                                       remote_option, 2, NULL);',
            '                        new_setting = use_remote;\n'
            '                        rb->set_option(rb->str(LANG_CONTROL_STYLE),&new_setting, RB_INT,\n'
            '                                       remote_option, 2, NULL);')
    replace("                }\n                break;\n            case 4:",
            "                }\n"
            "                use_remote = players > 2 || new_setting != 0;\n"
            "                break;\n            case 4:")
    replace("                playback_control(NULL);",
            "                if (playback_control(NULL))\n"
            "                    return PLUGIN_USB_CONNECTED;")
    replace("    configfile_save(SETTINGS_FILENAME, config,\n"
            "                        sizeof(config)/sizeof(*config),\n"
            "                        SETTINGS_VERSION);\n\n    return PLUGIN_OK;",
            "    if (rb->plugin_usb_taken())\n"
            "        return PLUGIN_USB_CONNECTED;\n"
            "    configfile_save(SETTINGS_FILENAME, config,\n"
            "                        sizeof(config)/sizeof(*config),\n"
            "                        SETTINGS_VERSION);\n\n    return PLUGIN_OK;")
elif src.stem == "codebuster":
    # The board dimensions and random palette require positive counts.
    for field in ("pieces", "colors", "guesses"):
        replace(f"{{TYPE_INT, 0, MAX_{field.upper()}_COUNT",
                f"{{TYPE_INT, 1, MAX_{field.upper()}_COUNT")
        replace(f"NULL, -1, MAX_{field.upper()}_COUNT, 1, NULL);",
                f"NULL, 1, 1, MAX_{field.upper()}_COUNT, NULL);")
    replace("static void settings_menu(void)", "static bool settings_menu(void)")
    replace("        switch(rb->do_menu(&settings_menu, &cur_item, NULL, false)) {",
            "        if (rb->plugin_usb_taken())\n"
            "            return true;\n"
            "        switch(rb->do_menu(&settings_menu, &cur_item, NULL, false)) {")
    replace("            case GO_TO_PREVIOUS:\n                menu_quit = true;",
            "            case MENU_ATTACHED_USB:\n                return true;\n"
            "            case GO_TO_ROOT:\n"
            "            case GO_TO_PREVIOUS:\n                menu_quit = true;")
    replace("}\n\nstatic bool resume;", "    return false;\n}\n\nstatic bool resume;")
    replace("                settings_menu();",
            "                if (settings_menu())\n"
            "                    quit = usb = menu_quit = true;")
    replace("                playback_control(NULL);",
            "                if (playback_control(NULL))\n"
            "                    quit = usb = menu_quit = true;")
    replace("            case 4:\n                quit = menu_quit = true;",
            "            case GO_TO_ROOT:\n"
            "            case GO_TO_PREVIOUS:\n"
            "            case 4:\n                quit = menu_quit = true;")
    replace("                usb = menu_quit = true;",
            "                quit = usb = menu_quit = true;")
    replace("            } while( ( button == BUTTON_NONE )\n"
            "                  || ( button & (BUTTON_REL|BUTTON_REPEAT) ) );\n"
            "            main_menu();",
            "            } while (!quit && (button == BUTTON_NONE ||\n"
            "                               (button & (BUTTON_REL | BUTTON_REPEAT))));\n"
            "            if (!quit)\n"
            "                main_menu();")
    replace("    if (rb->memcmp(&old_settings,",
            "    if (!usb && rb->memcmp(&old_settings,")
elif src.stem == "rocklife":
    replace("                case ROCKLIFE_PLAY_PAUSE:\n                case ROCKLIFE_QUIT:",
            "                case ROCKLIFE_QUIT:\n"
            "                    quit = 1;\n"
            "                    /* fall through */\n"
            "                case ROCKLIFE_PLAY_PAUSE:")
elif src.stem == "superdom":
    replace('    rb->read(fd, buf, 5);\n    if(rb->strcmp(buf, "SSGv3"))\n'
            '    {\n        rb->splash(HZ, "Invalid/incompatible savegame");',
            '    /* SSGv3 stores the phase, twenty resource values, the board\n'
            '     * and seven settings. Existing saves can have trailing bytes. */\n'
            '    const off_t save_size = 5 + 28 * sizeof(int) + sizeof(board);\n'
            '    if (rb->ffilesize(fd) < save_size || rb->read(fd, buf, 5) != 5 ||\n'
            '        rb->memcmp(buf, "SSGv3", 5) != 0)\n'
            '    {\n        rb->close(fd);\n'
            '        rb->splash(HZ, "Invalid/incompatible savegame");')
    replace("            static int sel=1;", "            int sel = superdom_settings.compdiff - 1;")
    replace("        case 4:\n            return RET_VAL_QUIT_ERR;\n            break;",
            "        case MENU_ATTACHED_USB:\n            return RET_VAL_USB;\n"
            "        case GO_TO_ROOT:\n"
            "        case GO_TO_PREVIOUS:\n"
            "        case 4:\n            return RET_VAL_QUIT_ERR;")
    replace('        if(!save_game())\n            rb->splash(HZ, "Game saved");',
            '    {\n'
            '        int result = save_game();\n'
            '        if (rb->plugin_usb_taken())\n            return RET_VAL_USB;\n'
            '        if (!result)\n            rb->splash(HZ, "Game saved");')
    replace('            rb->splash(HZ, "Error in save");\n        break;',
            '            rb->splash(HZ, "Error in save");\n        break;\n    }')
    replace("        case 1:\n            return RET_VAL_OK;\n            break;\n"
            "        case 2:\n            if((temp = ingame_menu()) != RET_VAL_OK)",
            "        case MENU_ATTACHED_USB:\n            return RET_VAL_USB;\n"
            "        case 1:\n            return RET_VAL_OK;\n"
            "        case 2:\n            if((temp = ingame_menu()) != RET_VAL_OK)")
else:
    raise SystemExit(f"Unknown conversion: {src}")

out.write_text(text)
