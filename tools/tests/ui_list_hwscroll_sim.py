#!/usr/bin/env python3
"""Check full-width lists sent as panel hardware-scroll frames in SDL.

The simulator emulates the iPod Classic panel: each frame is the last one
moved by its shift with its bands laid over, compared with the framebuffer.
With PODBOX_HWSCROLL_LOG set it prints one line per frame. Under Scrim (no
album art), Light and Classic this drives a slow scroll, a fast spin and
both ends of the list, and checks that frames went this way and that no
pixel of any of them differed. Classic's rows fill the list unevenly, so
there only the frames that leave the rows standing may go this way. Last
it pushes into a folder whose names load glyphs, waits, and scrolls: with
PODBOX_PANEL_RECORD, every submission from the first step on must be a
hardware-scroll frame, as later ones are.

Arguments: ROCKBOXUI ZIP OUTPUT_DIRECTORY [STRIP_DIRECTORY]
Xvfb is private to this process. Writes result.json, each run's log and
ui-perf.txt to the output directory, and Scrim's frame strips to the strip
directory (the output directory when omitted).
"""

import argparse
import json
import os
import re
import struct
import subprocess
import tempfile
import time
from pathlib import Path
from zipfile import ZipFile

from ui_list_snap_sim import Recorder, click, strip
from ui_menu_sim import FPS, Screen, process


TRACK_COUNT = 40
RECORD = struct.Struct("<4sIcBhhhI32h")
RECORD_SIZE = RECORD.size + 320 * 240 * 2
LINE = re.compile(r"^hwscroll: frame (\d+) top (-?\d+) shift (-?\d+) "
                  r"bands (\d+) wrong (\d+)$", re.M)

SCENARIOS = [
    ("slow-scroll", 9, 5, 250),
    ("fast-spin", 24, 5, 17),
    ("bottom-elastic", 30, 5, 60),
    ("top-elastic", 60, 4, 60),
]


def frames_in(log, start):
    text = log.read_text(errors="replace")[start:]
    return [tuple(int(v) for v in match.groups()) for match in LINE.finditer(text)]


def kinds_in(record, start):
    """The kind of each panel record from record number `start` on."""
    data = record.read_bytes() if record.is_file() else b""
    count = len(data) // RECORD_SIZE
    return "".join(RECORD.unpack_from(data, i * RECORD_SIZE)[2].decode()
                   for i in range(start, count)), count


def tap(env, key):
    subprocess.run(["xdotool", "keydown", key], env=env, check=True)
    time.sleep(3 / FPS)
    subprocess.run(["xdotool", "keyup", key], env=env, check=True)


def perf_rows(text):
    rows, header = [], None
    for line in text.splitlines():
        if line.startswith("frame,"):
            header = line.split(",")
        elif header and line[:1].isdigit():
            rows.append(dict(zip(header, line.split(","))))
    return rows


def run(binary, resource, root, theme, output, strips, env):
    root.mkdir()
    with ZipFile(resource) as archive:
        for path in archive.namelist():
            if path.startswith(".rockbox/fonts/"):
                archive.extract(path, root)
    rockbox = root / ".rockbox"
    (rockbox / "config.cfg").write_text(
        "start in screen: files\nshow files: supported\nlist wraparound: off\n"
        "backlight timeout: off\nvoice menus: off\nkeyclick: off\n"
        "animations: on\nsettings mode: everything\nbeep: off\n"
        "scrollbar: right\nscrollbar width: 8\n"
        f"native theme: {theme}\n")
    (rockbox / "ui-perf.enable").touch()
    for i in range(TRACK_COUNT):
        (root / f"Track {i + 1:02d}").mkdir()
    # Names in characters no earlier page drew, so their glyphs load.
    for i in range(TRACK_COUNT):
        name = "".join(chr(0x4E00 + 5 * i + k) for k in range(5))
        (root / "Track 01" / f"Item {i + 1:02d} {name}").mkdir()

    log_path = output / f"{theme}-sim.log"
    result = {}
    with log_path.open("w") as log, process(
        [str(binary), "--root", str(root), "--nobackground", "--zoom", "1",
         "--screenshot"], log, cwd=root,
        env=dict(env, PODBOX_HWSCROLL_LOG="1",
                 PODBOX_PANEL_RECORD=str(root / "panel.rec")),
    ) as firmware:
        screen = Screen(env, firmware)
        time.sleep(240 / FPS)
        x, y, _, _ = screen.bbox
        subprocess.run(["xdotool", "mousemove", str(x + 120), str(y + 120)],
                       env=env, check=True)
        time.sleep(30 / FPS)
        for name, clicks, button, delay in SCENARIOS:
            start = len(log_path.read_text(errors="replace"))
            with Recorder(screen) as rec:
                click(screen, clicks, button, delay)
                time.sleep(40 / FPS)
            frames = frames_in(log_path, start)
            if theme == "scrim":
                strip(rec.frames, strips / f"scrim-{name}.png")
            result[name] = {
                "captured_screens": len(rec.frames),
                "batches": len(frames),
                "moved": sum(1 for f in frames if f[2] != 0),
                "wrong_frames": sum(1 for f in frames if f[4]),
                "wrong_pixels": sum(f[4] for f in frames),
                "shifts": sorted({f[2] for f in frames}),
            }
        # The top-end scenario leaves Search selected; Track 01 is next.
        click(screen, 1, 5, 250)
        time.sleep(40 / FPS)
        tap(env, "Return")
        time.sleep(40 / FPS)
        _, start = kinds_in(root / "panel.rec", 0)
        click(screen, 5, 5, 250)
        time.sleep(40 / FPS)
        result["after-push"] = kinds_in(root / "panel.rec", start)[0]
        assert firmware.poll() is None
    perf = rockbox / "ui-perf.txt"
    text = perf.read_text() if perf.is_file() else ""
    (output / f"{theme}-ui-perf.txt").write_text(text)
    rows = perf_rows(text)
    result["perf"] = {
        "rows": len(rows),
        "hw_rows": sum(1 for r in rows if r.get("hw_offset", "-1") != "-1"),
    }
    all_frames = frames_in(log_path, 0)
    result["wrong_lines"] = sum(1 for f in all_frames if f[4])
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("binary", "resource", "output"):
        parser.add_argument(name, type=lambda arg: Path(arg).resolve())
    parser.add_argument("strips", nargs="?", type=lambda arg: Path(arg).resolve())
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    strips = args.strips or args.output
    strips.mkdir(parents=True, exist_ok=True)

    result = {}
    with tempfile.TemporaryDirectory(prefix="podbox-list-hwscroll-") as folder, \
            tempfile.TemporaryFile() as display, \
            (args.output / "xvfb.log").open("w") as log:
        root = Path(folder)
        runtime = root / "runtime"
        runtime.mkdir(mode=0o700)
        with process(["Xvfb", "-displayfd", str(display.fileno()), "-screen", "0",
                      "800x600x24", "-nolisten", "tcp"], log,
                     pass_fds=(display.fileno(),)):
            for _ in range(120):
                display.seek(0)
                number = display.read().decode().strip()
                if number:
                    break
                time.sleep(1 / FPS)
            else:
                raise RuntimeError("Xvfb did not start")
            env = dict(os.environ, DISPLAY=":" + number, XDG_RUNTIME_DIR=str(runtime),
                       SDL_VIDEODRIVER="x11", SDL_AUDIODRIVER="dummy",
                       SDL_RENDER_DRIVER="software", LIBGL_ALWAYS_SOFTWARE="1")
            for theme in ("scrim", "light", "classic"):
                result[theme] = run(args.binary, args.resource, root / theme,
                                    theme, args.output, strips, env)
    (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n")

    for theme, scenarios in result.items():
        assert scenarios["wrong_lines"] == 0, (theme, scenarios)
        # Classic jumps without motion, so it records no list-glide trace.
        if theme != "classic":
            assert scenarios["perf"]["hw_rows"] > 0, (theme, scenarios["perf"])
        for name, *_ in SCENARIOS:
            outcome = scenarios[name]
            # Classic's fast spin already reaches the end of the list.
            # Classic's rows fill the list unevenly, so only its frames
            # that leave the rows standing go by hardware scroll.
            if outcome["captured_screens"] > 1:
                assert outcome["batches"] > 0, (theme, name, outcome)
                assert (outcome["moved"] == 0) == (theme == "classic"), \
                    (theme, name, outcome)
            assert outcome["wrong_pixels"] == 0, (theme, name, outcome)
        kinds = scenarios["after-push"]
        assert kinds and set(kinds) == {"h"}, (theme, kinds)
    print("PASS full-width lists by hardware scroll: "
          + ", ".join(f"{theme} {sum(s[n]['batches'] for n, *_ in SCENARIOS)} frames"
                      for theme, s in result.items()) + ", no pixel wrong")


if __name__ == "__main__":
    main()
