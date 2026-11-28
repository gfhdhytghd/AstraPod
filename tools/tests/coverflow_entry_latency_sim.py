#!/usr/bin/env python3
"""Measure cold entry, exit and warm entry under Fruit Company and Ultra.

Use the same fixture with both builds. "Cold" is the first entry of each
simulator process, with the fixture's thumbnails on disk. UI perf measures
from transition dispatch, while the framebuffer recording measures from
the synthetic SELECT press (including its hold before release).
"""

import argparse
import json
import sys
from pathlib import Path

sys.dont_write_bytecode = True

from ultra_coverflow_sim import open_music, record
from ultra_sim import FRUIT, ULTRA, compare, make_fixture, perf_routes


# Exclude the always-changing clock and preview pane from the first-motion
# test. This area has the selected row and remains fixed before the split slide.
MENU = (0, 32, 126, 207)
COVERS = (0, 25, 320, 195)
ULTRA_FLIGHT_MS = 460


def first_change(shots, area):
    before = shots[0][1].crop(area).tobytes()
    return next(i for i, (_, frame) in enumerate(shots[1:], 1)
                if frame.crop(area).tobytes() != before)


def script(sim, theme):
    open_music(sim)
    recordings = {}
    for phase in ("cold", "warm"):
        recordings[phase] = record(sim, 170, "select")
        recordings[phase + "-exit"] = record(sim, 45, "menu")
    return recordings


def measure(folder, shots, theme):
    entries = perf_routes(folder, "cover-flow-entry")
    exits = perf_routes(folder, "cover-flow-exit")
    assert len(entries) == len(exits) == 2, (theme, entries, exits)
    paths = flow_paths(folder)
    assert len(paths) == 2 and all(row["enter"] == row["leave"] == "slide"
                                   for row in paths), (theme, paths)
    results = {}
    for i, phase in enumerate(("cold", "warm")):
        entry, leave = shots[phase], shots[phase + "-exit"]
        perf = entries[i]
        assert perf["first_run"] == str(1 - i)
        period = int(perf["period_us"]) / 1000
        start = int(perf["ready_wait_us"]) / 1000
        submit = int(perf["first_submit_wait_us"]) / 1000
        first = first_change(entry, MENU)
        results[phase] = {
            "start_ms": start,
            "submit_ms": submit,
            "host_motion_ms": round(entry[first][0], 2),
            "work_p99_ms": round(float(perf["work_p99_f"]) * period, 2),
            "interval_p99_ms": round(float(perf["interval_p99_f"]) * period, 2),
            "skipped_slots": int(perf["skipped_slots"]),
            "exit_work_p99_ms": round(float(exits[i]["work_p99_f"]) * period, 2),
            "exit_skipped_slots": int(exits[i]["skipped_slots"]),
        }
        # The split's zero-offset submission still shows the source screen.
        # Align the recording to that frame immediately before visible motion.
        zero = entry[first - 1][0]
        if theme == ULTRA:
            captures = []
            for percent in (25, 50, 75):
                target = zero + ULTRA_FLIGHT_MS * percent / 100
                ms, image = min(entry[first:], key=lambda shot: abs(shot[0] - target))
                image.save(folder / f"{phase}-{percent}.png")
                captures.append(round(ms - zero, 1))
            results[phase]["capture_elapsed_ms"] = captures

        assert leave[-1][1].crop(MENU).tobytes() == entry[0][1].crop(MENU).tobytes()
    assert shots["cold"][-1][1].crop(COVERS).tobytes() == \
           shots["warm"][-1][1].crop(COVERS).tobytes(), theme
    return results


def flow_paths(folder):
    lines = (folder / "ui-perf.txt").read_text().splitlines()
    return [dict(field.split("=", 1) for field in line.split()[1:])
            for line in lines if line.startswith("cover-flow-path ")]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("binary", "resource", "output"):
        parser.add_argument(name, type=lambda path: Path(path).resolve())
    parser.add_argument("--fixture", type=lambda path: Path(path).resolve())
    parser.add_argument("--runs", type=int, default=1)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    fixture = args.fixture or args.output / "fixture"
    if not args.fixture:
        make_fixture(args.binary, args.resource, fixture)

    results = []
    for run in range(args.runs):
        out = args.output / f"run-{run + 1}"
        shots = compare(args.binary, fixture, out, "", script)
        results.append({theme: measure(out / theme, shots[theme], theme)
                        for theme in (FRUIT, ULTRA)})
        (args.output / "result.json").write_text(json.dumps(results, indent=2) + "\n")
    print(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
