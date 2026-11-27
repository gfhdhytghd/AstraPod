#!/usr/bin/env python3
"""The Control Centre (MENU held) in each theme, headless, frame by frame.

Arguments: ROCKBOXUI SIMDISK OUTPUT_DIRECTORY [--themes classic,...]
SIMDISK is a simulator root with .rockbox in it, as build-sim.sh leaves.
Runs headless under SDL dummy video.

For Classic, Fruit Company, and Fruit Company Ultra, verifies the full-screen
Control Centre page (20 px stock status bar + Volume hero + configured setting
rows) and its horizontal 320 px page push/pop slide.

On open in all three page themes, the wheel immediately adjusts Volume.
Under Fruit Company and Fruit Company Ultra, SELECT cycles wheel focus across
the rows (returning to Volume on wrap or after 4 s idle), wheel adjusts the
focused item, MENU closes, and LEFT/RIGHT/PLAY leave the page untouched.
Classic keeps its directional keys (MENU/LEFT/RIGHT/PLAY) + wheel=Volume +
SELECT close. Scrim keeps its tiles.

Writes PNGs, GIFs, per-theme ui-perf.txt and result.json to OUTPUT_DIRECTORY.
"""

import argparse
import json
import tempfile
import time
from pathlib import Path

from PIL import Image, ImageChops

from ui_stock_walk_sim import Sim

WHEEL_THEMES = ("fruit-company", "fruit-company-ultra")
PAGE_THEMES = ("classic", *WHEEL_THEMES)
DEFAULT_THEMES = ("classic", "fruit-company", "fruit-company-ultra", "scrim")
STATUS = 20

# Row boxes for the 4-row default layout (Volume 64 px + three 52 px rows)
# and the 5-row layout (Volume 52 px + four 42 px rows).
ROW_BOXES_4 = (
    (0, 20, 320, 84),
    (0, 84, 320, 136),
    (0, 136, 320, 188),
    (0, 188, 320, 240),
)
ROW_BOXES_5 = (
    (0, 20, 320, 72),
    (0, 72, 320, 114),
    (0, 114, 320, 156),
    (0, 156, 320, 198),
    (0, 198, 320, 240),
)

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
style.quick_screen=page
"""


def differ(a, b, box=None):
    if box:
        a, b = a.crop(box), b.crop(box)
    return bool(ImageChops.difference(a, b).getbbox())


def frame(sim):
    return Image.frombytes("RGB", (320, 240), sim.seen[-1][1])


def settle(sim):
    sim.settle(quiet=0.25)
    return frame(sim)


def record(sim, out, name, key, hold):
    """Save all distinct frames captured while the key and animation run."""
    frames = [frame(sim)]
    first = len(sim.seen)
    start = time.monotonic()
    sim.key(key, hold=hold)
    time.sleep(0.55)
    settle(sim)
    stamps = [0]
    for stamp, pixels in sim.seen[first:]:
        frames.append(Image.frombytes("RGB", (320, 240), pixels))
        stamps.append(round((stamp - start) * 1000))
    holds = [max(16, b - a) for a, b in zip(stamps, stamps[1:])] + [400]
    frames[0].save(out / f"{name}.gif", save_all=True,
                   append_images=frames[1:], duration=holds, loop=0)
    return frames, stamps


def has_blue_row(img, box):
    """Return True if the row box carries the glossy blue selection bar."""
    y = box[1] + 6
    r, g, b = img.getpixel((6, y))
    return b > 170 and r < 60


def wheel_page_steps(sim, out, theme, tap):
    """Verify wheel=volume on open, SELECT cycles focus, wheel adjusts items,
    wrap and idle timeout both return focus to Volume."""
    def check(key, *rows):
        before = frame(sim)
        tap(key)
        after = frame(sim)
        changed = tuple(i for i, box in enumerate(ROW_BOXES_5)
                        if differ(before, after, box))
        return changed == rows

    steps = {}
    frame(sim).save(out / f"{theme}-focus-0-volume.png")
    # On open, wheel immediately adjusts Volume (row 0 only).
    steps["wheel_volume_up"] = check("down", 0)
    steps["wheel_volume_down"] = check("up", 0)

    # SELECT steps focus to row 1 (Brightness); wheel adjusts Brightness.
    steps["select_to_brightness"] = check("select", 0, 1)
    frame(sim).save(out / f"{theme}-focus-1-brightness.png")
    steps["wheel_brightness_down"] = check("up", 1)
    steps["wheel_brightness_up"] = check("down", 1)

    # SELECT steps focus to row 2 (Shuffle); wheel cycles Shuffle.
    steps["select_to_shuffle"] = check("select", 1, 2)
    frame(sim).save(out / f"{theme}-focus-2-shuffle.png")
    steps["wheel_shuffle_next"] = check("down", 2)

    # SELECT steps focus to row 3 (Repeat); wheel cycles Repeat.
    steps["select_to_repeat"] = check("select", 2, 3)
    steps["wheel_repeat_next"] = check("down", 3)

    # SELECT steps focus to row 4 (Hardware Keyclick switch); wheel toggles it.
    # Hardware Keyclick starts On in default-config.cfg, so step Off then On.
    steps["select_to_keyclick"] = check("select", 3, 4)
    steps["wheel_keyclick_off"] = check("up", 4)
    frame(sim).save(out / f"{theme}-focus-4-switch-off.png")
    steps["wheel_keyclick_on"] = check("down", 4)
    frame(sim).save(out / f"{theme}-focus-4-switch.png")

    # SELECT wraps focus back to Volume (row 0).
    steps["select_wraps_to_volume"] = check("select", 0, 4)

    # Idle timeout (4 s) returns focus from Brightness back to Volume.
    steps["select_for_idle_check"] = check("select", 0, 1)
    time.sleep(4.5)
    after_idle = settle(sim)
    steps["idle_returns_to_volume"] = (
        has_blue_row(after_idle, ROW_BOXES_5[0])
        and not has_blue_row(after_idle, ROW_BOXES_5[1]))
    return steps


def check_slide_motion(frames):
    """Verify the push/pop frames animate cleanly across multiple distinct steps."""
    return len(frames) >= 4


def run_theme(binary, simdisk, out, theme):
    result = {}
    is_page = theme in PAGE_THEMES
    is_wheel = theme in WHEEL_THEMES
    with tempfile.TemporaryDirectory(prefix="qs-sim-") as temp:
        root = Path(temp)
        rockbox = root / ".rockbox"
        rockbox.mkdir()
        for entry in (simdisk / ".rockbox").iterdir():
            if entry.name not in ("config.cfg", "ui-perf.txt",
                                  "ui-perf.enable", "native-themes"):
                (rockbox / entry.name).symlink_to(entry)
        themes_dir = rockbox / "native-themes"
        themes_dir.mkdir()
        src_themes = simdisk / ".rockbox/native-themes"
        if src_themes.is_dir():
            for entry in src_themes.iterdir():
                (themes_dir / entry.name).symlink_to(entry)
        if theme == "fruit-company-ultra":
            (themes_dir / "fruit-company-ultra.podtheme").write_text(
                ULTRA_FALLBACK_THEME)

        start = "root" if is_page else "menu"
        # Use Hardware Keyclick in the bottom slot for Fruit/Ultra so all five
        # rows (Volume hero, Brightness slider, Shuffle pill, Repeat pill,
        # Hardware Keyclick switch) are exercised. Classic uses the 4-row
        # default layout (Volume hero + merged Brightness, Shuffle, Repeat).
        slots = ("qs bottom: hardware keyclick\n" if is_wheel else "")
        (rockbox / "config.cfg").write_text(
            (simdisk / ".rockbox/default-config.cfg").read_text() +
            f"\nnative theme: {theme}\nstart in screen: {start}\n"
            "backlight timeout: off\nvoice menus: off\nanimations: on\n"
            "show background tasks: off\nshow disk activity: off\n" + slots)
        (rockbox / "ui-perf.enable").touch()

        close_key = "menu" if is_wheel else "select"
        sim = Sim(binary, root, out, theme, theme)
        try:
            time.sleep(1.5)
            settle(sim)
            # First open/close warms fonts and settles the underlying menu.
            sim.key("menu", hold=72)
            settle(sim)
            sim.key(close_key, hold=6)
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

            if is_wheel:
                result["initial_volume_selected"] = has_blue_row(
                    shown, ROW_BOXES_5[0])
                steps = wheel_page_steps(sim, out, theme, tap)
                before = frame(sim)
                for key in ("right", "back", "play"):
                    tap(key)
                    steps[f"{key}_leaves_page"] = not differ(
                        before, frame(sim), (0, STATUS, 320, 240))
                result["steps_change"] = steps
            else:
                steps = {}
                if is_page:
                    result["initial_volume_selected"] = has_blue_row(
                        shown, ROW_BOXES_4[0])
                    before = frame(sim)
                    tap("down")
                    after = frame(sim)
                    steps["wheel_volume"] = differ(before, after, ROW_BOXES_4[0])
                    tap("up")
                for key in ("right", "back", "play"):
                    before = frame(sim)
                    tap(key)
                    after = frame(sim)
                    after.save(out / f"{theme}-{key}.png")
                    steps[key] = differ(before, after)
                # Restore brightness stepped down by bottom slot (PLAY).
                before = frame(sim)
                tap("menu")
                after = frame(sim)
                steps["menu"] = differ(before, after)
                result["steps_change"] = steps

            frames_close, stamps_close = record(sim, out, f"{theme}-close",
                                                close_key, 6)
            closed = settle(sim)
            closed.save(out / f"{theme}-closed.png")
            result["close_frames"] = len(frames_close)
            result["close_ms"] = stamps_close
            result["closed_restores_menu"] = not differ(
                menu, closed, (0, STATUS, 160, 240))

            if is_page:
                corner = shown.getpixel((2, 236))
                result["full_screen_opaque"] = sum(corner) > 400
                result["slide_motion"] = (check_slide_motion(frames)
                                          and check_slide_motion(frames_close))
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
    parser.add_argument("--hud", action="store_true",
                        help="accepted for compatibility")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    themes = (args.themes.split(",") if args.themes else DEFAULT_THEMES)
    result = {}
    for theme in themes:
        result[theme] = run_theme(args.binary, args.simdisk, args.out, theme)
        (args.out / "result.json").write_text(json.dumps(result, indent=1))
    failed = []
    for theme, r in result.items():
        checks = [
            r["opened"],
            r["closed_restores_menu"],
            *r["steps_change"].values(),
            r.get("initial_volume_selected", True),
            r.get("full_screen_opaque", True),
            r.get("slide_motion", True),
        ]
        if not all(checks):
            failed.append(theme)
            print("FAIL", theme, json.dumps(r))
    print(f"{len(result) - len(failed)} of {len(result)} themes pass")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
