#!/usr/bin/env python3
"""The Control Centre (MENU held) in each theme, headless, frame by frame.

Arguments: ROCKBOXUI SIMDISK OUTPUT_DIRECTORY [--themes classic,...]
SIMDISK is a simulator root with .rockbox in it, as build-sim.sh leaves.
Runs headless under SDL dummy video.

Tests all four themes by default: classic, fruit-company, fruit-company-ultra,
and scrim.

For Classic, Fruit Company, and Fruit Company Ultra:
- Full-screen page under the 20 px white status bar: Shuffle and Repeat in
  the left column, one tall Brightness item with a vertical bar on the right
- Horizontal 320 px push/pop slide matching stock timing (~270 ms, quadratic)
- Click wheel adjusts Volume and shows only the status-bar volume overlay
  without altering the page body; the normal status bar is back after the
  overlay's hold
- LEFT steps Shuffle and RIGHT Repeat, each changing only its own row; MENU
  raises Brightness and PLAY lowers it, each stopping at its end
- SELECT closes with pop slide and restores the caller menu with no flash

For Scrim:
- Keeps its existing tiles and vertical sheet slide unchanged
- Wheel shows status-bar volume overlay; MENU/LEFT/RIGHT/PLAY step tiles;
  SELECT closes and restores the caller menu.

Writes PNGs, GIFs, per-theme ui-perf.txt and result.json to OUTPUT_DIRECTORY.
"""

import argparse
import json
import tempfile
import time
from pathlib import Path

from PIL import Image, ImageChops

from ui_stock_walk_sim import Sim

PAGE_THEMES = ("classic", "fruit-company", "fruit-company-ultra")
DEFAULT_THEMES = (*PAGE_THEMES, "scrim")
STOCK_STATUS = 20
SCRIM_STATUS = 35

# The page's three items, as screen boxes: the Shuffle row (LEFT), the
# Repeat row (RIGHT) and the tall Brightness column (MENU up, PLAY down).
ITEM_BOXES = {
    "shuffle": (0, 20, 192, 130),
    "repeat": (0, 130, 192, 240),
    "brightness": (192, 20, 320, 240),
}
# A column of the Brightness knob outside its bar.
KNOB_COLUMN = 247


def knob_y(image):
    """The Brightness knob's centre row: the bright rows of its rim."""
    rows = [y for y in range(60, 220)
            if min(image.getpixel((KNOB_COLUMN, y))) > 170]
    return sum(rows) / len(rows) if rows else None


# Fallback theme file used when running against a branch that does not yet
# carry built-in Fruit Company Ultra; built-in names take precedence once
# merged.
ULTRA_FALLBACK_THEME = """\
podbox-theme=1
color.background=ffffff
color.foreground=000000
color.muted=000000
color.accent=3185d0
color.selection=0089de
color.selected_text=ffffff
color.status_bg=cecfce
color.status_fg=000000
color.wps_muted=3d3d3d
gradient.selection=96c5da,09a4e1,0065d3,004386
gradient.status=fcfcfc,f8f8f8,9f9e9e,5b5b5b
gradient.progress=6483f0,6789f2,56aeff,4e8fce
gradient.progress_track=dbdbdb,fdfdfd,e2e2e2,bababa
font.list=16-helvetica-bold
font.title=17-helvetica-bold
font.body=13-helvetica
font.status=13-helvetica-bold
font.status_title=13-helvetica-bold
font.wps_title=17-helvetica-bold
font.wps_meta=13-helvetica-bold
font.wps_time=14-helvetica-bold
font.pane_title=17-helvetica-bold-pane
font.pane_text=13-helvetica-bold
font.list_value=13-helvetica-bold
metric.row_height=24
metric.list_x=0
metric.list_y=20
metric.list_bottom=0
metric.status_height=20
metric.status_pad_left=6
metric.status_pad_right=5
metric.status_text_y=3
metric.status_title_height=17
metric.status_icon_y=0
metric.status_icon_size=20
metric.status_icon_gap=5
metric.wps_volume_y=14
background.list=solid
background.wps=solid
background.lyrics=solid
background.status=solid
motion.push=18
motion.pop=18
motion.list=0
motion_easing=quad
style.selection=bar
style.list_icons=off
style.accessory=selected
style.status_clock=off
style.page_motion=slide
style.wps_layout=classic
style.scrollbar=classic
style.art=plastic
style.menu_pane=split
style.list_rows=fill
style.menu_tree=stock
style.quick_screen=hud
"""


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


def run_theme(binary, simdisk, out, theme):
    result = {}
    is_page = theme in PAGE_THEMES
    status_h = STOCK_STATUS if is_page else SCRIM_STATUS
    with tempfile.TemporaryDirectory(prefix="qs-sim-") as temp:
        root = Path(temp)
        rockbox = root / ".rockbox"
        rockbox.mkdir()
        for entry in (simdisk / ".rockbox").iterdir():
            if entry.name not in ("config.cfg", "ui-perf.txt", "ui-perf.enable",
                                  "native-themes"):
                (rockbox / entry.name).symlink_to(entry)
        if theme == "fruit-company-ultra":
            themes = rockbox / "native-themes"
            themes.mkdir()
            (themes / "fruit-company-ultra.podtheme").write_text(
                ULTRA_FALLBACK_THEME)
        start = "root" if theme in ("fruit-company", "fruit-company-ultra") else "menu"
        (rockbox / "config.cfg").write_text(
            (simdisk / ".rockbox/default-config.cfg").read_text() +
            f"\nnative theme: {theme}\nstart in screen: {start}\n"
            "brightness: 32\n"
            "backlight timeout: off\nvoice menus: off\nanimations: on\n"
            "show background tasks: off\nshow disk activity: off\n")
        (rockbox / "ui-perf.enable").touch()
        sim = Sim(binary, root, out, theme, theme)
        try:
            time.sleep(2)
            settle(sim)
            # Warm up the caller so its list has settled after a setting change
            # before capturing the baseline menu layout.
            sim.key("menu", hold=72)
            settle(sim)
            sim.key("right", hold=6)
            settle(sim)
            sim.key("select", hold=6)
            menu = settle(sim)
            menu.save(out / f"{theme}-menu.png")

            # Verify unchanged open -> close restores the caller with no flash.
            sim.key("menu", hold=72)
            settle(sim)
            u_frames, _ = record(sim, out, f"{theme}-close-unchanged", "select", 6)
            u_closed = settle(sim)
            compare_box = (0, status_h, 160, 240) if is_page else (0, status_h, 320, 240)
            result["unchanged_close_no_flash"] = (
                len(u_frames) >= 2 and
                not differ(menu, u_closed, compare_box) and
                not differ(u_frames[-1], u_closed, compare_box)
            )

            frames, stamps = record(sim, out, f"{theme}-open", "menu", 72)
            shown = settle(sim)
            shown.save(out / f"{theme}-open.png")
            result["open_frames"] = len(frames)
            result["open_ms"] = stamps
            result["opened"] = differ(menu, shown)
            result["open_animated"] = len(frames) >= 3

            def tap(key):
                sim.key(key, hold=6)
                settle(sim)

            # 1. Wheel adjusts Volume and shows ONLY the status-bar volume
            # overlay without changing the page body.
            before_vol = frame(sim)
            tap("down")
            after_vol1 = frame(sim)
            for _ in range(5):
                tap("down")
            after_vol2 = frame(sim)
            after_vol2.save(out / f"{theme}-volume.png")
            status_box = (0, 0, 320, status_h)
            body_box = (0, status_h, 320, 240)
            result["wheel_shows_status_volume"] = (
                differ(before_vol, after_vol1, status_box) and
                differ(after_vol1, after_vol2, status_box)
            )
            result["wheel_leaves_body_unchanged"] = (
                not differ(before_vol, after_vol1, body_box) and
                not differ(before_vol, after_vol2, body_box)
            )

            # 2. Each key changes only its own item.
            if is_page:
                steps = {}
                for key, expected in (("back", "shuffle"), ("right", "repeat"),
                                      ("menu", "brightness"),
                                      ("play", "brightness")):
                    before = frame(sim)
                    tap(key)
                    after = frame(sim)
                    changed = tuple(name for name, box in ITEM_BOXES.items()
                                    if differ(before, after, box))
                    steps[key] = changed == (expected,)
                    if key == "menu":
                        steps["menu_raises"] = knob_y(after) < knob_y(before)
                    if key == "play":
                        steps["play_lowers"] = knob_y(after) > knob_y(before)
                frame(sim).save(out / f"{theme}-stepped.png")
                # Brightness stops at its ends instead of wrapping.
                for key, name in (("menu", "top"), ("play", "bottom")):
                    for _ in range(20):
                        tap(key)
                    end = frame(sim)
                    end.save(out / f"{theme}-brightness-{name}.png")
                    tap(key)
                    steps[f"stops_at_{name}"] = not differ(
                        end, frame(sim), ITEM_BOXES["brightness"])
                result["steps_change"] = steps
            else:
                steps = {}
                for key in ("back", "menu", "right", "play"):
                    before = frame(sim)
                    tap(key)
                    after = frame(sim)
                    steps[key] = differ(before, after, body_box)
                frame(sim).save(out / f"{theme}-stepped.png")
                result["steps_change"] = steps

            # Wait for the 3 s status-bar volume overlay to clear so the close
            # comparison against `menu` checks a settled status bar too.
            time.sleep(3.2)
            rested = settle(sim)
            rested.save(out / f"{theme}-volume-rested.png")
            result["volume_overlay_clears"] = not differ(
                before_vol, rested, status_box)

            # 3. SELECT closes with pop slide and restores caller with no flash.
            frames, stamps = record(sim, out, f"{theme}-close", "select", 6)
            closed = settle(sim)
            closed.save(out / f"{theme}-closed.png")
            result["close_frames"] = len(frames)
            result["close_ms"] = stamps
            result["close_animated"] = len(frames) >= 3
            compare_box = (0, status_h, 160, 240) if is_page else (0, status_h, 320, 240)
            result["closed_restores_menu"] = not differ(
                menu, closed, compare_box)
            # Check no extra redraw/flash right after the close slide finishes.
            result["close_no_flash"] = (
                len(frames) >= 2 and
                not differ(frames[-1], closed, compare_box)
            )
            if is_page:
                # Full-screen steel-blue backdrop at bottom-left corner, not
                # a dimmed menu background.
                r, g, b = shown.getpixel((2, 236))
                result["full_screen_backdrop"] = (b > r and g > r and b >= 60)
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
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    themes = tuple(args.themes.split(",")) if args.themes else DEFAULT_THEMES
    result = {}
    for theme in themes:
        result[theme] = run_theme(args.binary, args.simdisk, args.out, theme)
        (args.out / "result.json").write_text(json.dumps(result, indent=1))
    failed = []
    for theme, r in result.items():
        checks = [
            r["opened"],
            r["open_animated"],
            r["wheel_shows_status_volume"],
            r["wheel_leaves_body_unchanged"],
            r["volume_overlay_clears"],
            r["closed_restores_menu"],
            r["close_animated"],
            r["close_no_flash"],
            r["unchanged_close_no_flash"],
            *r["steps_change"].values(),
            r.get("full_screen_backdrop", True),
        ]
        if not all(checks):
            failed.append(theme)
            print("FAIL", theme, json.dumps(r))
    print(f"{len(result) - len(failed)} of {len(result)} themes pass")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
