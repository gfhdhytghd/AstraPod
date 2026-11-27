#!/usr/bin/env python3
"""Check MENU after playing a track from Cover Flow on the SDL dummy driver.

Usage: coverflow_back_stack_sim.py ROCKBOXUI ZIP OUTPUT [--fixture DIRECTORY]
Requires Pillow, ffmpeg and Tesseract with English data. Use --themes to
select themes, including fruit-company-ultra on builds that provide it.
"""

import argparse
from io import BytesIO
import os
from pathlib import Path
import runpy
import subprocess
import sys

sys.dont_write_bytecode = True
from PIL import Image
from ui_music_fixture import TRACK_COUNT
from ui_stock_transitions_sim import CLASSIC, H, W, make_fixture, session


def prepare_fixture(binary, resource, out):
    fixture = out / "fixture"
    make_fixture(binary, resource, fixture)
    capture = runpy.run_path(Path(__file__).resolve().parents[1] /
                             "capture-readme-gifs.py")
    config = fixture / ".rockbox/config.cfg"
    saved = config.read_bytes()
    try:
        capture["build_library_cache"](binary, fixture, out,
                                       tracks=TRACK_COUNT, albums=9)
    finally:
        config.write_bytes(saved)
    return fixture


def walk(sim, has_music, visit_settings):
    frames = {}

    def tap(name, key, wait=100):
        sim.tap(key, wait)
        _, pixels = sim.frames_since(0)
        assert pixels, f"{name}: no frame reached the panel"
        frames[name] = [(0, Image.frombytes("RGB", (W, H), pixels[-1]))]

    if visit_settings:
        for _ in range(5):
            sim.tap("down", 12)
        tap("settings-in", "select")
        for _ in range(17):
            sim.tap("down", 12)
        sim.wait(45)
        tap("advanced", "select")
        tap("settings-out", "menu")
        tap("home-after-settings", "menu")
        for _ in range(5):
            sim.tap("up", 12)
        sim.wait(45)
    if has_music:
        tap("music-in", "select")
    tap("covers-in", "select", 240)
    tap("album-in", "select")
    tap("playing", "select")
    tap("back-1-album", "menu")
    tap("back-2-covers", "menu")
    if has_music:
        tap("back-3-music", "menu")
        tap("back-4-home", "menu")
    else:
        tap("back-3-home", "menu")
    return frames


def read_text(image, crop, mode):
    patch = image.crop(crop).convert("L")
    enlarged = patch.resize((4 * patch.width, 4 * patch.height),
                            Image.Resampling.NEAREST)
    data = BytesIO()
    enlarged.save(data, "PNG")
    result = subprocess.run(
        ["tesseract", "stdin", "stdout", "-l", "eng", "--psm", str(mode)],
        input=data.getvalue(), stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        env=dict(os.environ, OMP_THREAD_LIMIT="1"), check=True, timeout=30)
    return " ".join(result.stdout.decode().split())


def check(frames, theme, has_music, visit_settings):
    expected = {}
    if visit_settings:
        expected.update({"settings-in": "Settings", "advanced": "Settings",
                         "settings-out": "Settings",
                         "home-after-settings": "AstraPod"})
    if has_music:
        expected["music-in"] = "Music"
    expected.update({"covers-in": "Cover Flow", "album-in": "Cover Flow",
                     "playing": "Now Playing", "back-1-album": "Cover Flow",
                     "back-2-covers": "Cover Flow"})
    if has_music:
        expected["back-3-music"] = "Music"
    expected["back-4-home" if has_music else "back-3-home"] = "AstraPod"

    failures = []
    for name, title in expected.items():
        image = frames[name][0][1]
        actual = read_text(image, (0, 0, 104, 20), 7)
        print(f"{theme} {name}: {actual}", flush=True)
        if actual != title:
            failures.append(f"{name}: expected {title!r}, saw {actual!r}")
        if name == "advanced":
            row = read_text(image, (0, 20, 145, 44), 7)
            if "Sound Settings" not in row:
                failures.append(f"{name}: expected Sound Settings, saw {row!r}")
        if name in ("album-in", "back-1-album"):
            tracks = read_text(image, (35, 96, 285, 145), 6)
            if "Track 02" not in tracks or "Track 03" not in tracks:
                failures.append(f"{name}: expected album tracks, saw {tracks!r}")
        if name in ("covers-in", "back-2-covers"):
            caption = read_text(image, (55, 186, 265, 232), 6)
            if "Aurora Sessions" not in caption:
                failures.append(f"{name}: expected cover caption, saw {caption!r}")
    return [f"{theme} {failure}" for failure in failures]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("binary", "resource", "output"):
        parser.add_argument(name, type=lambda arg: Path(arg).resolve())
    parser.add_argument("--fixture", type=lambda arg: Path(arg).resolve())
    parser.add_argument("--themes", default="fruit-company,classic")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    fixture = args.fixture or prepare_fixture(args.binary, args.resource,
                                               args.output)
    failures = []
    for theme in args.themes.split(","):
        has_music = theme != "classic"
        for visit_settings in (False, True) if has_music else (False,):
            name = theme + ("-after-settings" if visit_settings else "")
            frames = session(args.binary, fixture, args.output / name, theme,
                             "" if has_music else CLASSIC,
                             lambda sim: walk(sim, has_music, visit_settings))
            failures.extend(check(frames, name, has_music, visit_settings))
    if failures:
        raise SystemExit("\n".join(failures))
    print("PASS: one MENU per level after playback from Cover Flow")


if __name__ == "__main__":
    main()
