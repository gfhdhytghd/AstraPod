#!/usr/bin/env python3
"""Check side-by-side keyboard and prose buttons in four headless themes."""

import argparse
from contextlib import contextmanager
import json
from pathlib import Path
import re
import subprocess
import tempfile
import time

from ui_era_dialogs_sim import (
    ULTRA_FALLBACK_THEME,
    check_pair_pixels,
    flat_button_readings,
    private_display,
    settle,
)
from ui_menu_sim import Screen, process
from ui_music_fixture import build_tagcache

THEMES = ("classic", "fruit-company", "fruit-company-ultra", "scrim")
NAVY_COLUMNS = [(31, 156), (164, 289)]
FLAT_KEYBOARD_BOXES = [(22, 119, 151, 148), (169, 119, 298, 148)]
FLAT_PROSE_BOXES = [(23, 172, 157, 199), (163, 172, 297, 199)]


def move(screen, key, count=1):
    for _ in range(count):
        screen.tap(key)
        time.sleep(0.12)
    return settle(screen)


def long_label_language(binary, rb, out):
    repo = Path(__file__).resolve().parents[2]
    english = repo / "apps-ipod/lang/english.lang"
    source = english.read_text()
    for default, key in (("OK", "LANG_DIAG_OK"), ("Cancel", "LANG_DIAG_CANCEL")):
        pattern = rf'(id: {key}\b.*?<dest>\s*\*: )"{default}"'
        source, count = re.subn(pattern, r'\1"WWWWWWWWWWWWWWWWWWWW"', source,
                                count=1, flags=re.DOTALL)
        assert count == 1, key
    translation = rb.parent / "wide.lang"
    translation.write_text(source)
    features = (binary.parent / "apps-ipod/genlang-features").read_text().strip()
    with (out / "genlang.log").open("w") as log:
        subprocess.run([
            str(repo / "tools/genlang"), f"-e={english}", f"-t=ipod6g:{features}",
            "-i=71", f"-b={rb / 'langs/wide-labels.lng'}", str(translation),
        ], stdout=log, stderr=subprocess.STDOUT, check=True)


def ellipsis_visible(frame, action):
    # The rightmost three runs are the three full stops, separated by blank
    # columns. The W strokes rise above the dots at the bottom of the line.
    x0, y0, x1, y1 = action
    runs = []
    for x in range(x0, x1):
        ink = [y for y in range(y0, y1) if frame.getpixel((x, y))[0] >= 112]
        if ink:
            if not runs or runs[-1][0] != x - 1:
                runs.append([x, min(ink)])
            else:
                runs[-1][0] = x
                runs[-1][1] = min(runs[-1][1], min(ink))
    return len(runs) >= 4 and all(y >= y1 - 3 for _, y in runs[-3:])


def measure(frame, theme, kind):
    if theme == "scrim":
        boxes = FLAT_KEYBOARD_BOXES if kind == "keyboard" else FLAT_PROSE_BOXES
        readings = flat_button_readings(frame, boxes)
        return readings, {"button_ink_padding": all(item["padding_ok"] for item in readings)}
    arrows = [0, 0] if kind.startswith("keyboard") else [-1, 1]
    readings, checks = check_pair_pixels(frame, NAVY_COLUMNS, arrows)
    if kind == "keyboard-long":
        checks["ellipsis_visible"] = len(readings) == 2 and all(
            item["action"] and ellipsis_visible(frame, item["action"]) for item in readings
        )
    return readings, checks


@contextmanager
def sim_screen(binary, root, env, log_path, config):
    rb = root / ".rockbox"
    for name in ("config.cfg.new", ".resume.cfg", ".resume.cfg.new"):
        (rb / name).unlink(missing_ok=True)
    (rb / "config.cfg").write_text(config)
    with log_path.open("w") as log, process(
        [str(binary), "--root", str(root), "--nobackground", "--zoom", "1", "--screenshot"],
        log, env=env, cwd=root,
    ) as fw:
        time.sleep(2.5)
        yield Screen(env, fw)


def run_case(binary, root, env, out, config, theme, long_labels=False):
    active = config + f"native theme: {theme}\n"
    if long_labels:
        active += "lang: wide-labels\n"
    kind = "keyboard-long" if long_labels else "keyboard"
    frames = {}
    with sim_screen(binary, root, env, out / f"{theme}-{kind}.log", active) as screen:
        move(screen, "Down", 7)  # Settings -> System
        move(screen, "Return")
        move(screen, "Down", 6)  # System -> Manage Settings
        move(screen, "Return")
        move(screen, "Down", 3)  # Manage Settings -> Save .cfg File
        frames[kind] = move(screen, "Return")
    if not long_labels:
        with sim_screen(binary, root, env, out / f"{theme}-prose.log", active) as screen:
            move(screen, "Down", 3)  # Settings -> Library
            move(screen, "Return")
            if theme == "fruit-company":
                move(screen, "Down", 8)  # Library -> Maintenance without wraparound
            else:
                move(screen, "Up")
            move(screen, "Return")
            move(screen, "Down", 4)  # Maintenance -> Update Sound Analysis
            frames["prose-confirm"] = move(screen, "Return")
            if theme == "scrim":
                move(screen, "space")  # transfer focus to the buttons
            frames["prose-depth"] = move(screen, "Return")

    results = {}
    for kind, frame in frames.items():
        frame.save(out / f"{theme}-{kind}.png")
        readings, checks = measure(frame, theme, kind)
        results[kind] = {"measurements": readings, "checks": checks}
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("binary", "simdisk", "out"):
        parser.add_argument(name, type=lambda p: Path(p).resolve())
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    results = {}
    with tempfile.TemporaryDirectory(prefix="era-pairs-") as temp:
        root = Path(temp)
        rb = root / ".rockbox"
        rb.mkdir()
        resources = args.simdisk / ".rockbox"
        for entry in resources.iterdir():
            if entry.name not in {"config.cfg", "library", "logs", "langs", "native-themes"}:
                (rb / entry.name).symlink_to(entry)
        (rb / "langs").mkdir()
        (rb / "native-themes").mkdir()
        (rb / "native-themes/fruit-company-ultra.podtheme").write_text(
            ULTRA_FALLBACK_THEME + "style.dialog=classic\n")
        config = (resources / "default-config.cfg").read_text() + (
            "\nstart in screen: menu\nbacklight timeout: off\nvoice menus: off\n"
            "playlist engine: on\nshow background tasks: off\nshow disk activity: off\n"
            "animations: on\n")
        (rb / "config.cfg").write_text(config)
        subprocess.run([
            "ffmpeg", "-v", "error", "-f", "lavfi", "-i", "sine=frequency=440:duration=3",
            "-metadata", "title=Dialog test tone", str(root / "test-tone.flac"),
        ], check=True)
        build_tagcache(args.binary, root, args.out / "tagcache.log", min_tracks=1)
        long_label_language(args.binary, rb, args.out)
        with private_display(args.out / "xvfb.log") as env:
            for theme in THEMES:
                print(f"Testing {theme}...", flush=True)
                results[theme] = run_case(args.binary, root, env, args.out, config, theme)
            results["classic"].update(
                run_case(args.binary, root, env, args.out, config, "classic", long_labels=True))

    (args.out / "result.json").write_text(json.dumps(results, indent=2) + "\n")
    failed = [f"{theme}/{dialog}/{check}"
              for theme, dialogs in results.items() for dialog, result in dialogs.items()
              for check, passed in result["checks"].items() if not passed]
    if failed:
        print("FAIL:", ", ".join(failed))
        return 1
    print("PASS side-by-side keyboard and prose dialogs in all four themes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
