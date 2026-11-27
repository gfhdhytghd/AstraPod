#!/usr/bin/env python3
"""Fruit Company's in-place Settings rows, in SDL.

Arguments: ROCKBOXUI SIMDISK OUTPUT_DIRECTORY
SIMDISK is a simulator root with .rockbox in it, as build-sim.sh leaves.
Xvfb is private to this process. Select on Shuffle, Repeat, Sound Check,
Clicker and Date & Time > 24 Hour Clock must change the row's value and
nothing else in the list, show a different value at every step, and come
back to the first value after as many steps as stock has values. Each row
is then left one step on and the simulator restarted: the rows must show
the same values, and config.cfg must hold them. config.cfg is written when
storage goes idle, at most every 30 seconds, so it is read only at the end.
Writes each row's frames and result.json to the output directory.
"""

import argparse
import json
import os
import re
import tempfile
import time
from pathlib import Path

from PIL import ImageChops

from ui_menu_sim import FPS, Screen, process
from fruit_tree_sim import press, settle

# (row in Settings, or (Date & Time, row in it); cfg name; its default; the
# values Select steps through from the default, in stock's order)
ROWS = [
    ("shuffle", 1, "shuffle", "off", ["on", "off"]),
    ("repeat", 2, "repeat", "off", ["one", "all", "off"]),
    ("sound_check", 10, "replaygain type", "track shuffle",
     ["off", "track shuffle"]),
    ("clicker", 11, "hardware keyclick", "on", ["off", "on"]),
    ("24_hour_clock", (12, 3), "time format", "12hour",
     ["24hour", "12hour"]),
]

LIST = (0, 22, 160, 240)
ROW_H = 24


def cfg_value(root, name, default):
    path = root / ".rockbox/config.cfg"
    text = path.read_text(errors="replace") if path.exists() else ""
    m = re.search(r"^%s: (.*)$" % re.escape(name), text, re.M)
    return m.group(1).strip() if m else default


def selected_row(frame):
    """The y of the highlighted row's top in the list half."""
    list_half = frame.crop(LIST).convert("L")
    w, h = list_half.size
    for y in range(h):
        if list_half.getpixel((2, y)) < 200:
            return y
    return None


def without_row(frame, top):
    """The list half with the highlighted row blanked out."""
    img = frame.crop(LIST).copy()
    img.paste((0, 0, 0), (0, max(0, top - 2), img.size[0], top + ROW_H + 2))
    return img


def row_of(frame, top):
    """The highlighted row alone."""
    return frame.crop(LIST).crop((0, top, LIST[2] - LIST[0], top + ROW_H))


def differ(a, b):
    return bool(ImageChops.difference(a, b).getbbox())


def go(s, where):
    """From the main menu, into Settings and onto row `where`."""
    for _ in range(5):
        press(s, "Down")
    press(s, "Return")
    path = where if isinstance(where, tuple) else (where,)
    for depth, row in enumerate(path):
        for _ in range(row):
            press(s, "Down")
        if depth < len(path) - 1:
            press(s, "Return")
    return settle(s)


def leave(s, where):
    path = where if isinstance(where, tuple) else (where,)
    for row in reversed(path):
        for _ in range(row):
            press(s, "Up")
        press(s, "Escape")
    for _ in range(5):
        press(s, "Up")


def check_row(s, out, name, where, steps):
    menu = go(s, where)
    menu.save(out / f"{name}-0.png")
    top = selected_row(menu)
    if top is None:
        leave(s, where)
        return {"ok": False, "error": "no highlighted row"}, None
    rows = [row_of(menu, top)]
    rest_unchanged = True
    for i in range(1, len(steps) + 1):
        frame = press(s, "Return")
        frame.save(out / f"{name}-{i}.png")
        rest_unchanged &= not differ(without_row(menu, top),
                                     without_row(frame, top))
        rows.append(row_of(frame, top))
    distinct = all(differ(rows[i], rows[j]) for i in range(len(steps))
                   for j in range(i))
    back = not differ(rows[0], rows[-1])
    # Leave the row one step on, to see it survive a restart
    frame = press(s, "Return")
    left = row_of(frame, top)
    leave(s, where)
    ok = rest_unchanged and distinct and back and not differ(left, rows[1])
    return {"values": len(steps), "rest_unchanged": rest_unchanged,
            "distinct_values": distinct, "back_to_first": back,
            "ok": ok}, (top, left)


def run_sim(binary, root, env, log, out, body):
    with process([str(binary), "--root", str(root), "--nobackground",
                  "--zoom", "1", "--screenshot"],
                 log, env=env, cwd=root) as firmware:
        time.sleep(150 / FPS)
        s = Screen(env, firmware)
        time.sleep(5)
        settle(s)
        result = body(s)
        assert firmware.poll() is None, "simulator exited"
        return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("binary", "simdisk", "out"):
        parser.add_argument(name, type=lambda p: Path(p).resolve())
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    result = {}
    with tempfile.TemporaryDirectory(prefix="fruit-cycle-") as temp, \
            tempfile.TemporaryFile() as display, \
            (args.out / "xvfb.log").open("w") as log:
        root = Path(temp) / "root"
        rockbox = root / ".rockbox"
        rockbox.mkdir(parents=True)
        for entry in (args.simdisk / ".rockbox").iterdir():
            if entry.name != "config.cfg":
                (rockbox / entry.name).symlink_to(entry)
        (rockbox / "config.cfg").write_text(
            (args.simdisk / ".rockbox/default-config.cfg").read_text() +
            "\nnative theme: fruit-company\nstart in screen: root\n"
            "backlight timeout: off\nvoice menus: off\nanimations: off\n")
        runtime = Path(temp) / "runtime"
        runtime.mkdir(mode=0o700)
        with process(["Xvfb", "-displayfd", str(display.fileno()), "-screen",
                      "0", "800x600x24"], log, pass_fds=(display.fileno(),)):
            for _ in range(FPS * 5):
                display.seek(0)
                number = display.read().decode().strip()
                if number:
                    break
                time.sleep(1 / FPS)
            else:
                raise RuntimeError("Xvfb did not start")
            env = dict(os.environ, DISPLAY=":" + number,
                       XDG_RUNTIME_DIR=str(runtime), SDL_VIDEODRIVER="x11",
                       SDL_AUDIODRIVER="dummy", SDL_RENDER_DRIVER="software")

            rows_after = {}

            def first(s):
                for name, where, _, _, steps in ROWS:
                    result[name], rows_after[name] = check_row(
                        s, args.out, name, where, steps)
                    (args.out / "result.json").write_text(
                        json.dumps(result, indent=1))

            def second(s):
                for name, where, _, _, _ in ROWS:
                    frame = go(s, where)
                    frame.save(args.out / f"{name}-restarted.png")
                    leave(s, where)
                    if rows_after[name] is None:
                        continue
                    top, left = rows_after[name]
                    kept = not differ(row_of(frame, top), left)
                    result[name]["shown_after_restart"] = kept
                    result[name]["ok"] &= kept

            with (args.out / "sim1.log").open("w") as slog:
                run_sim(args.binary, root, env, slog, args.out, first)
            with (args.out / "sim2.log").open("w") as slog:
                run_sim(args.binary, root, env, slog, args.out, second)
            (args.out / "config.cfg").write_text(
                (rockbox / "config.cfg").read_text())
            for name, _, cfg, default, steps in ROWS:
                saved = cfg_value(root, cfg, default)
                result[name]["config"] = saved
                result[name]["ok"] &= saved == steps[0]
    (args.out / "result.json").write_text(json.dumps(result, indent=1))
    failed = [k for k, v in result.items() if not v["ok"]]
    for key in failed:
        print("FAIL", key, result[key])
    print(f"{len(result) - len(failed)} of {len(result)} rows pass")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
