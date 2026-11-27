#!/usr/bin/env python3
"""The quick screen (MENU held) in each theme, headless, frame by frame.

Arguments: ROCKBOXUI SIMDISK OUTPUT_DIRECTORY [--themes classic,...] [--hud]
SIMDISK is a simulator root with .rockbox in it, as build-sim.sh leaves.
The simulator uses SDL dummy video. Classic and Scrim use their original
keys. With --hud, exercise Fruit's wheel focus, SELECT and MENU, and verify
that the HUD dims and restores the caller. Writes PNGs, GIFs, per-theme
ui-perf.txt and result.json to the output directory.
"""

import argparse
import json
import tempfile
import time
from pathlib import Path

from PIL import Image, ImageChops

from ui_stock_walk_sim import Sim

WHEEL_HUD_THEMES = ("fruit-company", "stock-hud")
HUD_THEMES = ("classic", *WHEEL_HUD_THEMES)
STATUS = 20
HUD_SLOTS = ((0, 56, 160, 119), (160, 56, 320, 119),
             (0, 150, 160, 209), (160, 150, 320, 209))


def wheel_hud_steps(snapshot, tap):
    """Verify that only the old/new focus or the activated tile changes."""
    def check(key, *tiles):
        before = snapshot()
        tap(key)
        after = snapshot()
        return tuple(i for i, box in enumerate(HUD_SLOTS)
                     if differ(before, after, box)) == tiles

    return {
        "select_initial_left": check("select", 0),
        "wheel_to_top": check("down", 0, 1),
        "select_top": check("select", 1),
        "wheel_to_right": check("down", 1, 2),
        "select_right": check("select", 2),
        "wheel_to_bottom": check("down", 2, 3),
        "select_bottom": check("select", 3),
        "wheel_wraps": check("down", 0, 3),
        "reverse_wheel_wraps": check("up", 0, 3),
    }


def differ(a, b, box=None):
    if box:
        a, b = a.crop(box), b.crop(box)
    return bool(ImageChops.difference(a, b).getbbox())


def frame(sim):
    return Image.frombytes("RGB", (320, 240), sim.seen[-1][1])


def settle(sim):
    sim.settle(quiet=.2)
    return frame(sim)


def record(sim, out, name, key, hold):
    """Save all distinct frames captured while the key and animation run."""
    frames = [frame(sim)]
    first = len(sim.seen)
    start = time.monotonic()
    sim.key(key, hold=hold)
    time.sleep(.65)
    settle(sim)
    stamps = [0]
    for stamp, pixels in sim.seen[first:]:
        frames.append(Image.frombytes("RGB", (320, 240), pixels))
        stamps.append(round((stamp - start) * 1000))
    holds = [b - a for a, b in zip(stamps, stamps[1:])] + [400]
    frames[0].save(out / f"{name}.gif", save_all=True,
                   append_images=frames[1:], duration=holds, loop=0)
    return frames, stamps


def run_theme(binary, simdisk, out, theme, hud):
    result = {}
    wheel_hud = theme in WHEEL_HUD_THEMES
    close = "menu" if wheel_hud else "select"
    with tempfile.TemporaryDirectory(prefix="qs-sim-") as temp:
        root = Path(temp)
        rockbox = root / ".rockbox"
        rockbox.mkdir()
        for entry in (simdisk / ".rockbox").iterdir():
            if entry.name not in ("config.cfg", "ui-perf.txt", "ui-perf.enable",
                                  "native-themes"):
                (rockbox / entry.name).symlink_to(entry)
        if theme == "stock-hud":
            # Exercise the same style combination used by the Ultra HUD.
            # Ultra itself is not a built-in theme on this integration branch.
            themes = rockbox / "native-themes"
            themes.mkdir()
            (themes / "stock-hud.podtheme").write_text(
                "podbox-theme=1\nstyle.menu_tree=stock\nstyle.quick_screen=hud\n"
                "background.list=solid\ncolor.background=303030\n"
                "palette.background=none\n")
        start = "root" if wheel_hud else "menu"
        slots = ("qs top: keyclick repeats\nqs bottom: hardware keyclick\n"
                 if wheel_hud else "")
        (rockbox / "config.cfg").write_text(
            (simdisk / ".rockbox/default-config.cfg").read_text() +
            f"\nnative theme: {theme}\nstart in screen: {start}\n"
            "backlight timeout: off\nvoice menus: off\nanimations: on\n"
            "show background tasks: off\nshow disk activity: off\n" + slots)
        (rockbox / "ui-perf.enable").touch()
        sim = Sim(binary, root, out, theme, theme)
        try:
            time.sleep(2)
            settle(sim)
            # Warm up the caller so its list has settled before capture.
            sim.key("menu", hold=72)
            settle(sim)
            if not wheel_hud:
                # A value change lets the caller redraw its list. Compare the
                # next close against that settled layout.
                sim.key("right", hold=6)
                settle(sim)
            sim.key(close, hold=6)
            menu = settle(sim)
            menu.save(out / f"{theme}-menu.png")

            frames, stamps = record(sim, out, f"{theme}-open", "menu", 72)
            shown = settle(sim)
            shown.save(out / f"{theme}-open.png")
            result["open_frames"] = len(frames)
            result["open_ms"] = stamps
            result["opened"] = differ(menu, shown)

            def tap(key):
                sim.key(key, hold=6)
                settle(sim)

            if wheel_hud:
                result["steps_change"] = wheel_hud_steps(lambda: frame(sim), tap)
                before = frame(sim)
                for key in ("right", "back", "play"):
                    tap(key)
                    result["steps_change"][f"{key}_leaves_hud"] = not differ(
                        before, frame(sim), (0, STATUS, 320, 240))
            else:
                steps = {}
                for key in ("right", "back", "play"):
                    before = frame(sim)
                    tap(key)
                    after = frame(sim)
                    after.save(out / f"{theme}-{key}.png")
                    steps[key] = differ(before, after)
                result["steps_change"] = steps
                # Restore the brightness stepped down via the bottom tile.
                tap("menu")

            frames, stamps = record(sim, out, f"{theme}-close", close, 6)
            closed = settle(sim)
            closed.save(out / f"{theme}-closed.png")
            result["close_frames"] = len(frames)
            result["close_ms"] = stamps
            result["closed_restores_menu"] = not differ(
                menu, closed, (0, STATUS, 160, 240))
            if hud and theme in HUD_THEMES:
                corner = (2, 236)
                result["dimmed_backdrop"] = (
                    0 < sum(shown.getpixel(corner)) < sum(menu.getpixel(corner)))
        finally:
            sim.close()
        perf = rockbox / "ui-perf.txt"
        if perf.exists():
            (out / f"{theme}-ui-perf.txt").write_text(perf.read_text())
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("binary", "simdisk", "out"):
        parser.add_argument(name, type=lambda p: Path(p).resolve())
    parser.add_argument("--themes", help="comma-separated list of native themes")
    parser.add_argument("--hud", action="store_true")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    themes = (args.themes.split(",") if args.themes else
              ("classic", "fruit-company", "scrim"))
    if args.hud and args.themes is None:
        themes += ("stock-hud",)
    result = {}
    for theme in themes:
        result[theme] = run_theme(args.binary, args.simdisk, args.out, theme, args.hud)
        (args.out / "result.json").write_text(json.dumps(result, indent=1))
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
