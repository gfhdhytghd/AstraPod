#!/usr/bin/env python3
"""Stage the foreground HID lease for Remote Control. GPL-2.0-or-later."""
from pathlib import Path
import sys


source, output = map(Path, sys.argv[1:])
text = source.read_text()
anchor = "    (void)parameter;\n"
if text.count(anchor) != 1:
    raise SystemExit(f"{source}: review the AstraPod Remote Control adapter")
output.write_text(text.replace(anchor, anchor + """
    if (rb->usb_hid_open() == ROCK_HID_ARMED)
        rb->splash(HZ, "Plug in USB to use Remote Control");
"""))
