#!/usr/bin/env python3
"""Measure iPod 6G UI loops, LCD submissions and CPU boost requests.

Configure a fresh ipod6g simulator build and run make PODBOX_POWER_PROBE=1.
Pass its rockboxui and a bundled Simulator ZIP. SDL uses the dummy drivers.
Counters count top-level action returns, split by timed wait or poll, and
SDL panel submissions. They do not measure SoC wakeups, current or DMA.
"""
import argparse
import json
import mmap
import runpy
import shutil
import struct
import sys
import tempfile
import time
from pathlib import Path

sys.dont_write_bytecode = True
from coverflow_np_sheet_sim import THEMES
from ui_stock_transitions_sim import CLASSIC, Sim, make_fixture

COUNTERS = struct.Struct("=9Q")
SCENARIOS = ("static-menu", "pane-cycle", "playing-lit", "playing-dark",
             "cover-flow", "scroll")
THEME_NAMES = ("fruit-company", "classic", "scrim")


def fixture_for(binary, resource, output):
    fixture = output / "fixture"
    make_fixture(binary, resource, fixture)
    capture = runpy.run_path(str(Path(__file__).resolve().parents[1] /
                                 "capture-readme-gifs.py"))
    config = fixture / ".rockbox/config.cfg"
    saved = config.read_bytes()
    try:
        capture["build_library_cache"](binary, fixture, output,
                                       tracks=24, albums=9)
    finally:
        config.write_bytes(saved)
    return fixture


def sample(probe):
    now = time.monotonic_ns()
    waits, polls, updates, pixels, boost, start, lit, audio, context = \
        COUNTERS.unpack_from(probe)
    if start:
        boost += max(0, now - start)
    return (now, waits, polls, updates, pixels, boost, lit, audio, context)


def run(binary, fixture, output, theme, scenario, seconds, pane_seconds):
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output) as work:
        root = Path(work) / "disk"
        shutil.copytree(fixture, root)
        rockbox = root / ".rockbox"
        for name in ("config.cfg.new", "config.cfg.old", "ui-perf.enable"):
            (rockbox / name).unlink(missing_ok=True)
        cfg = rockbox / "config.cfg"
        settings = CLASSIC
        if scenario == "static-menu":
            settings = CLASSIC.replace("pictureflow,database",
                                       "settings,pictureflow,database")
            settings = settings.replace(",settings,tagnavi10", ",tagnavi10")
        if scenario == "playing-dark":
            settings += ("backlight timeout: 5\n"
                         "backlight timeout plugged: 5\n"
                         "lcd sleep after backlight off: on\n")
        cfg.write_text(cfg.read_text() + settings +
                       f"\nnative theme: {theme}\n"
                       "idle poweroff: off\nsleep timer on startup: off\n"
                       "caption backlight: off\nscrolling enabled: off\n")
        path = output / "counters.bin"
        with path.open("w+b") as handle:
            handle.truncate(COUNTERS.size)
            with mmap.mmap(handle.fileno(), COUNTERS.size,
                           access=mmap.ACCESS_READ) as probe:
                sim = Sim(binary, root, output, {"PODBOX_POWER_COUNTERS": str(path)})
                try:
                    sim.wait(180)
                    if scenario == "static-menu" and theme == "fruit-company":
                        sim.tap("down", 100)  # Music -> Videos/Photos.
                    entry = None
                    if scenario in ("cover-flow", "playing-lit", "playing-dark"):
                        if scenario == "cover-flow":
                            entry = sample(probe)
                        THEMES[theme][0](sim)
                    if scenario.startswith("playing"):
                        sim.tap("play", 420 if scenario == "playing-dark" else 200)
                    before = sample(probe)
                    assert before[1] + before[2], "missing probe counters"
                    expected = (1 if scenario.startswith("playing") else
                                0 if scenario == "cover-flow" else 4)
                    assert before[-1] & 0xff == expected, (scenario, before)
                    assert bool(before[-2] & 1) == scenario.startswith("playing"), before
                    assert bool(before[-3]) == (scenario != "playing-dark"), before
                    duration = pane_seconds if scenario == "pane-cycle" else seconds
                    if scenario == "scroll":
                        index = 0
                        while time.monotonic_ns() - before[0] < duration * 1e9:
                            sim.tap("down" if index % 8 < 4 else "up", 7)
                            index += 1
                    else:
                        sim.wait(duration * 60)
                    after = sample(probe)
                    assert after[-3:] == before[-3:], (scenario, before, after)
                finally:
                    sim.close()
    delta = [b - a for a, b in zip(before, after)]
    elapsed = delta[0] / 1e9
    result = dict(seconds=round(elapsed, 3),
                  waits_s=round(delta[1] / elapsed, 2),
                  polls_s=round(delta[2] / elapsed, 2),
                  lcd_s=round(delta[3] / elapsed, 2),
                  equivalent_full_frames_s=round(delta[4] / (320 * 240 * elapsed), 2),
                  boost_pct=round(delta[5] / delta[0] * 100, 2))
    if entry:
        result["entry_boost_s"] = round((before[5] - entry[5]) / 1e9, 3)
        result["entry_polls"] = before[2] - entry[2]
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("binary", "resource", "output"):
        parser.add_argument(name, type=lambda arg: Path(arg).resolve())
    parser.add_argument("--fixture", type=lambda arg: Path(arg).resolve())
    parser.add_argument("--seconds", type=float, default=8)
    parser.add_argument("--pane-seconds", type=float, default=38)
    parser.add_argument("--themes", nargs="+", choices=THEME_NAMES,
                        default=THEME_NAMES)
    parser.add_argument("--scenarios", nargs="+", choices=SCENARIOS,
                        default=SCENARIOS)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    fixture = args.fixture or fixture_for(args.binary, args.resource, args.output)
    results = {}
    for theme in args.themes:
        results[theme] = {}
        for scenario in args.scenarios:
            result = run(args.binary, fixture, args.output / theme / scenario,
                         theme, scenario, args.seconds, args.pane_seconds)
            results[theme][scenario] = result
            print(theme, scenario, result, flush=True)
            (args.output / "results.json").write_text(
                json.dumps(results, indent=2) + "\n")


if __name__ == "__main__":
    main()
