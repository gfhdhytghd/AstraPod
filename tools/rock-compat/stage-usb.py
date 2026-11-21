#!/usr/bin/env python3
"""Stage the source adjustments for the enabled foreground modules."""
import sys
from pathlib import Path

src, out = map(Path, sys.argv[1:])
text = src.read_text()

def replace(old, new):
    global text
    if text.count(old) != 1:
        raise SystemExit(f"{src}: refresh USB conversion for {old!r}")
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
else:
    raise SystemExit(f"Unknown conversion: {src}")

out.write_text(text)
