#!/usr/bin/env python3
"""Replay shell WAV start/stop through the production EP0 and UI handlers."""
import argparse
import os
from pathlib import Path
import subprocess


fixture = Path(__file__).resolve().with_suffix(".c")
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("out", type=Path)
parser.add_argument("--source", type=Path, default=fixture.parents[2])
args = parser.parse_args()
args.out.mkdir(parents=True, exist_ok=True)

recording = (args.source / "apps-ipod/screens/recording.c").read_text()
rec_start = recording.index("static volatile enum sm1_test_record_state")
rec_end = recording.index("\n#endif\n\nstatic int open_recording", rec_start)

usb = (args.source / "firmware/usbstack/usb_core.c").read_text()
usb_start = usb.index("static void usb_core_control_request_handler",
                      usb.index("/* Handling USB requests starts here */"))
usb_end = usb.index("    if (req->bRequest == 0x5c)", usb_start)

shutdown = (args.source / "apps-ipod/system/shutdown.c").read_text()
dispatch_start = shutdown.index("        case SYS_SM1_TEST_CONTROL:")
dispatch_end = shutdown.index("\n#endif", dispatch_start)

for name, fragment in {
    "recording.inc": recording[rec_start:rec_end],
    "ep0.inc": usb[usb_start:usb_end] + "#endif\n    assert(0);\n}\n",
    "dispatch.inc": shutdown[dispatch_start:dispatch_end],
}.items():
    (args.out / name).write_text(fragment)

binary = (args.out / "podlink_recording").resolve()
subprocess.run([
    os.environ.get("CC", "cc"), "-std=gnu11", "-Wall", "-Wextra", "-Werror",
    "-g", "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
    "-I", str(args.source / "firmware/export"), "-I", str(args.out),
    str(fixture), "-o", str(binary)
], check=True)
for case in ("normal", "nested", "busy", "late", "unplug", "save-error"):
    subprocess.run([str(binary), case], check=True, timeout=10)
