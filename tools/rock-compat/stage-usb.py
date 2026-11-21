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
else:
    raise SystemExit(f"Unknown conversion: {src}")

out.write_text(text)
