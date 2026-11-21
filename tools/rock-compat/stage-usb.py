#!/usr/bin/env python3
"""Stage the source adjustments for the enabled foreground modules."""
import sys
from pathlib import Path

src, out = map(Path, sys.argv[1:])
text = src.read_text()

def replace(old, new, count=1):
    global text
    if text.count(old) != count:
        raise SystemExit(f"{src}: refresh source conversion for {old!r}")
    text = text.replace(old, new)

if src.stem == "stopwatch":
    replace(
        "if (rb->default_event_handler(button) == SYS_USB_CONNECTED)\n"
        "                    ret = PLUGIN_USB_CONNECTED;",
        "if (rb->default_event_handler(button) == SYS_USB_CONNECTED)\n"
        "                {\n"
        "                    ret = PLUGIN_USB_CONNECTED;\n"
        "                    done = true;\n"
        "                }",
    )
elif src.stem == "dice":
    replace("int selection;", "int selection = 0;")
    replace("#define PRINT_BUFFER_LENGTH MAX_DICES*4",
            "#define PRINT_BUFFER_LENGTH (MAX_DICES*4 + 1)")
    replace(
        "    if(!dice_menu(&dice))\n",
        "    bool roll = dice_menu(&dice);\n"
        "    if (rb->plugin_usb_taken())\n"
        "        return PLUGIN_USB_CONNECTED;\n"
        "    if (!roll)\n",
    )
    replace(
        "            case DICE_QUIT:\n                return PLUGIN_OK;\n",
        "            case DICE_QUIT:\n                return PLUGIN_OK;\n"
        "            default:\n"
        "                if (rb->default_event_handler(action) == SYS_USB_CONNECTED)\n"
        "                    return PLUGIN_USB_CONNECTED;\n"
        "                break;\n",
    )
    replace("while (!menu_quit)", "while (!menu_quit && !rb->plugin_usb_taken())")
elif src.stem == "flipit":
    replace(
        "        rb->button_get_w_tmo(HZ*3);",
        "        int button = rb->button_get_w_tmo(HZ*3);\n"
        "        if (rb->default_event_handler(button) == SYS_USB_CONNECTED)\n"
        "            return PLUGIN_USB_CONNECTED;",
    )
elif src.stem == "jackpot":
    # A spin can run for several seconds. Service system events and quit
    # while the reels are moving, without starting another turn.
    replace(
        "        rb->sleep(SLEEP_TIME);",
        "        int action = pluginlib_getaction(TIMEOUT_NOBLOCK,\n"
        "                                        plugin_contexts, ARRAYLEN(plugin_contexts));\n"
        "        if (action == JACKPOT_QUIT)\n"
        "            exit(PLUGIN_OK);\n"
        "        if (IS_SYSEVENT(action))\n"
        "            exit_on_usb(action);\n"
        "        rb->sleep(SLEEP_TIME);",
    )
elif src.stem == "periodic_table":
    # The table assigns fixed pixel regions to its labels and cells.
    replace(
        "    struct font *pf = rb->font_get(rb->screens[SCREEN_MAIN]->getuifont());",
        "    rb->lcd_setfont(FONT_SYSFIXED);\n"
        "    struct font *pf = rb->font_get(FONT_SYSFIXED);",
    )
elif src.stem == "robotfindskitten":
    replace('#include "lib/display_text.h"',
            '#include "lib/display_text.h"\n#include "lib/pluginlib_exit.h"')
    replace(
        "                ARRAYLEN(plugin_contexts));",
        "                ARRAYLEN(plugin_contexts));\n"
        "        exit_on_usb(input);",
    )
    replace(
        "    button = rb->button_get(true);",
        "    button = rb->button_get(true);\n"
        "    exit_on_usb(button);",
    )
    replace(
        "  while( ( button == BUTTON_NONE )",
        "  while( IS_SYSEVENT(button) || ( button == BUTTON_NONE )",
    )
    replace(
        "  display_text(ARRAYLEN(help_text), help_text, NULL, NULL, false);",
        "  if (display_text(ARRAYLEN(help_text), help_text, NULL, NULL, false))\n"
        "    exit(PLUGIN_USB_CONNECTED);",
    )
elif src.stem == "2048":
    # The two menu loops must propagate USB even from nested native widgets.
    replace(
        "        switch(rb->do_menu(&menu, &sel, NULL, false))\n        {",
        "        switch(rb->do_menu(&menu, &sel, NULL, false))\n        {\n"
        "        case MENU_ATTACHED_USB:\n"
        "            exit(PLUGIN_USB_CONNECTED);",
        count=2,
    )
elif src.stem == "amaze":
    replace('#include "lib/pluginlib_actions.h"',
            '#include "lib/pluginlib_actions.h"\n#include "lib/pluginlib_exit.h"')
    replace(
        "                                        ARRAYLEN(plugin_contexts));",
        "                                        ARRAYLEN(plugin_contexts));\n"
        "        if (IS_SYSEVENT(input))\n"
        "            exit_on_usb(input);",
    )
    replace(
        "                                    ARRAYLEN(plugin_contexts));\n\n",
        "                                    ARRAYLEN(plugin_contexts));\n"
        "        if (IS_SYSEVENT(input))\n"
        "            exit_on_usb(input);\n\n",
        count=2,
    )
    replace(
        "        switch(result)\n        {",
        "        switch(result)\n        {\n"
        "        case MENU_ATTACHED_USB:\n"
        "            exit(PLUGIN_USB_CONNECTED);",
    )
elif src.stem == "brickmania":
    replace("            if (rb->button_hold())",
            "            if (!IS_SYSEVENT(button) && rb->button_hold())")
elif src.stem == "chopper":
    replace(
        "        if (rb->button_get(false) == QUIT) {",
        "        int event = rb->button_get(false);\n"
        "        if (IS_SYSEVENT(event) &&\n"
        "            rb->default_event_handler(event) == SYS_USB_CONNECTED)\n"
        "            return PLUGIN_USB_CONNECTED;\n"
        "        if (event == QUIT) {",
    )
    replace(
        "            ret = chopMenu(1);\n"
        "            if (ret != -1)\n"
        "                return PLUGIN_OK;",
        "            ret = chopMenu(1);\n"
        "            if (ret != -1)\n"
        "                return ret;",
    )
    replace(
        "                if (rb->default_event_handler(move_button) == SYS_USB_CONNECTED)\n"
        "                    return PLUGIN_USB_CONNECTED;\n",
        "",
    )
elif src.stem == "fireworks":
    replace('#include "lib/pluginlib_actions.h"',
            '#include "lib/pluginlib_actions.h"\n#include "lib/pluginlib_exit.h"')
    replace(
        "            case 8:\n                quit_plugin = true;",
        "            case MENU_ATTACHED_USB:\n"
        "                exit(PLUGIN_USB_CONNECTED);\n"
        "            case 8:\n                quit_plugin = true;",
    )
    replace(
        "        switch(button)\n",
        "        if (IS_SYSEVENT(button))\n"
        "            exit_on_usb(button);\n"
        "        switch(button)\n",
    )
elif src.stem == "highscore":
    replace('#include "highscore.h"',
            '#include "highscore.h"\n#include "pluginlib_exit.h"')
    replace(
        "    rb->button_get(true);",
        "    int button;\n"
        "    do\n"
        "    {\n"
        "        button = rb->button_get(true);\n"
        "        exit_on_usb(button);\n"
        "    } while (IS_SYSEVENT(button));",
    )
elif src.stem == "minesweeper":
    replace('#include "lib/playback_control.h"',
            '#include "lib/playback_control.h"\n#include "lib/pluginlib_exit.h"')
    replace(
        "    do\n        button = rb->button_get(true);\n"
        "    while( ( button == BUTTON_NONE )",
        "    do\n    {\n        button = rb->button_get(true);\n"
        "        exit_on_usb(button);\n    }\n"
        "    while( IS_SYSEVENT(button) || ( button == BUTTON_NONE )",
    )
elif src.stem == "pong":
    replace(
        "        if(rb->default_event_handler(key) == SYS_USB_CONNECTED)\n"
        "            return -1; /* exit game because of USB */\n",
        "",
    )
    replace(
        "        key = rb->button_get_w_tmo(end - *rb->current_tick);",
        "        key = rb->button_get_w_tmo(end - *rb->current_tick);\n"
        "        if(IS_SYSEVENT(key) &&\n"
        "           rb->default_event_handler(key) == SYS_USB_CONNECTED)\n"
        "            return -1; /* exit game because of USB */",
    )
elif src.stem == "snake":
    replace("        if (rb->button_hold() && !pause)",
            "        if (!IS_SYSEVENT(button) && rb->button_hold() && !pause)")
elif src.stem == "snake2":
    replace("        if (rb->button_hold())",
            "        if (!IS_SYSEVENT(button) && rb->button_hold())")
    replace(
        "        button=rb->button_get(true);",
        "        button=rb->button_get(true);\n"
        "        if (rb->default_event_handler(button) == SYS_USB_CONNECTED) {\n"
        "            dead = 1;\n"
        "            quit = 2;\n"
        "            return;\n"
        "        }",
    )
elif src.stem == "star":
    replace("    int selection, level=1;", "    int selection = 0, level=1;")
    # The level transition presents rectangles over the previous frame.
    replace(
        "                star_run_game(level-1);",
        "                rb->lcd_clear_display();\n"
        "                rb->lcd_update();\n"
        "                star_run_game(level-1);",
    )
    replace(
        "            case 4:\n                menu_quit = true;",
        "            case MENU_ATTACHED_USB:\n"
        "                return PLUGIN_USB_CONNECTED;\n"
        "            case 4:\n                menu_quit = true;",
    )
else:
    raise SystemExit(f"Unknown conversion: {src}")

out.write_text(text)
