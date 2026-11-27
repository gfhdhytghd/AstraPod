#!/usr/bin/env python3
"""The quick screen (MENU held) in each theme, headless, frame by frame.

Arguments: ROCKBOXUI SIMDISK OUTPUT_DIRECTORY [--themes classic,...] [--hud]
SIMDISK is a simulator root with .rockbox in it, as build-sim.sh leaves.
Xvfb is private to this process. For each theme the main menu is shown,
MENU is held until the quick screen opens, Right, Left and Play each step
their slot, and Select closes it. A first
open and close goes before, so the menu it is compared with has settled. Every frame
that differs from the one before it is kept while the screen opens and
closes, as a GIF with its capture times. Writes PNGs, GIFs, each theme's
ui-perf.txt and result.json to the output directory.

The screen must open, every step must change it, and closing must give back
the menu's list exactly. The pane beside it is left out: it draws its
reflection again on its own time. --hud also asserts, for Classic and Fruit Company, that the
menu shows dimmed around the panel rather than covered.
"""

import argparse
import json
import os
import tempfile
import time
from pathlib import Path

from PIL import ImageChops

from ui_menu_sim import FPS, Screen, process

HUD_THEMES = ("classic", "fruit-company")
STATUS = 20


def differ(a, b, box=None):
    if box:
        a, b = a.crop(box), b.crop(box)
    return bool(ImageChops.difference(a, b).getbbox())


def settle(s, frames=60):
    last = s.frame()
    for _ in range(frames):
        time.sleep(6 / FPS)
        frame = s.frame()
        if not differ(last, frame):
            return frame
        last = frame
    return last


def record(s, out, name, edges, seconds):
    """Frames that change while `edges` ((time, kind, key)) are sent."""
    frames, stamps = [s.frame()], [0.0]
    start = time.monotonic()
    pending = list(edges)
    while time.monotonic() - start < seconds:
        now = time.monotonic() - start
        while pending and pending[0][0] <= now:
            s.key(pending[0][1], pending[0][2])
            pending.pop(0)
        frame = s.frame()
        if differ(frames[-1], frame):
            frames.append(frame)
            stamps.append(round(now * 1000))
    holds = [b - a for a, b in zip(stamps, stamps[1:])] + [400]
    frames[0].save(out / f"{name}.gif", save_all=True,
                   append_images=frames[1:], duration=holds, loop=0)
    return frames, stamps


def run_theme(binary, simdisk, env, out, theme, hud):
    result = {}
    with tempfile.TemporaryDirectory(prefix="qs-sim-") as temp:
        root = Path(temp)
        rockbox = root / ".rockbox"
        rockbox.mkdir()
        for entry in (simdisk / ".rockbox").iterdir():
            if entry.name not in ("config.cfg", "ui-perf.txt",
                                  "ui-perf.enable"):
                (rockbox / entry.name).symlink_to(entry)
        start = "root" if theme == "fruit-company" else "menu"
        # Fruit Company's bottom slot holds an on/off setting instead of
        # Brightness, so a switch is drawn too.
        slots = ("qs bottom: hardware keyclick\n"
                 if theme == "fruit-company" else "")
        (rockbox / "config.cfg").write_text(
            (simdisk / ".rockbox/default-config.cfg").read_text() +
            f"\nnative theme: {theme}\nstart in screen: {start}\n"
            "backlight timeout: off\nvoice menus: off\nanimations: on\n"
            "show background tasks: off\nshow disk activity: off\n" + slots)
        (rockbox / "ui-perf.enable").touch()
        with (out / f"{theme}.log").open("w") as slog, process(
            [str(binary), "--root", str(root), "--nobackground", "--zoom",
             "1", "--screenshot"], slog, env=env, cwd=root) as firmware:
            time.sleep(120 / FPS)
            s = Screen(env, firmware)
            time.sleep(60 / FPS)
            # A first round settles the list's scroll, which a menu sets
            # afresh when it is drawn again.
            s.key("keydown", "Escape")
            time.sleep(1.2)
            s.key("keyup", "Escape")
            settle(s)
            s.tap("Return")
            menu = settle(s)
            menu.save(out / f"{theme}-menu.png")

            frames, stamps = record(s, out, f"{theme}-open", [
                (0.0, "keydown", "Escape"), (1.2, "keyup", "Escape")], 2.4)
            shown = settle(s)
            shown.save(out / f"{theme}-open.png")
            result["open_frames"] = len(frames)
            result["open_ms"] = stamps
            result["opened"] = differ(menu, shown)

            steps = {}
            for key in ("Right", "Left", "space"):
                before = settle(s)
                s.tap(key)
                after = settle(s)
                after.save(out / f"{theme}-{key.lower()}.png")
                steps[key] = differ(before, after)
            result["steps_change"] = steps
            if not slots:
                # Play stepped the bottom Brightness down, and the simulator
                # shows it; Menu steps the top one back up.
                s.tap("Escape")
                settle(s)

            frames, stamps = record(s, out, f"{theme}-close", [
                (0.1, "keydown", "Return"), (0.17, "keyup", "Return")], 1.4)
            closed = settle(s)
            closed.save(out / f"{theme}-closed.png")
            result["close_frames"] = len(frames)
            result["close_ms"] = stamps
            result["closed_restores_menu"] = not differ(
                menu, closed, (0, STATUS, 160, 240))
            if hud and theme in HUD_THEMES:
                # A corner of the content area: the menu shows through,
                # darker than it was.
                corner = (2, 236)
                result["dimmed_backdrop"] = (
                    sum(shown.getpixel(corner)) < sum(menu.getpixel(corner))
                    and sum(shown.getpixel(corner)) > 0)
            assert firmware.poll() is None, "simulator exited"
        perf = rockbox / "ui-perf.txt"
        if perf.exists():
            (out / f"{theme}-ui-perf.txt").write_text(perf.read_text())
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("binary", "simdisk", "out"):
        parser.add_argument(name, type=lambda p: Path(p).resolve())
    parser.add_argument("--themes", default="classic,fruit-company,scrim")
    parser.add_argument("--hud", action="store_true")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    result = {}
    with tempfile.TemporaryDirectory(prefix="qs-x-") as temp, \
            tempfile.TemporaryFile() as display, \
            (args.out / "xvfb.log").open("w") as log:
        runtime = Path(temp) / "runtime"
        runtime.mkdir(mode=0o700)
        with process(["Xvfb", "-displayfd", str(display.fileno()), "-screen",
                      "0", "800x600x24", "-nolisten", "tcp"], log,
                     pass_fds=(display.fileno(),)):
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
            for theme in args.themes.split(","):
                result[theme] = run_theme(args.binary, args.simdisk, env,
                                          args.out, theme, args.hud)
                (args.out / "result.json").write_text(
                    json.dumps(result, indent=1))
    failed = []
    for theme, r in result.items():
        checks = [r["opened"], r["closed_restores_menu"],
                  *r["steps_change"].values(), r.get("dimmed_backdrop", True)]
        if not all(checks):
            failed.append(theme)
            print("FAIL", theme, json.dumps(r))
    print(f"{len(result) - len(failed)} of {len(result)} themes pass")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
