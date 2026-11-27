#!/usr/bin/env python3
"""Fruit Company Ultra's Now Playing, sheets and USB screen, in SDL.

Records each motion under Fruit Company and Fruit Company Ultra in a
simulator with no window:

  np-track     Now Playing, the next song from another album: the title
               block and cover dissolve and the fill drains back.
  np-seek      letting go of Next after a scrub: a gloss travels along
               the fill as it lands.
  np-vol-in    turning the wheel slides the volume bar in.
  np-vol-out   the volume bar slides out again 4 s later.
  sheet-in     the Disk Mode question rises over Now Playing.
  sheet-out    and sinks again when answered. Both are recorded three
               times and their intervals measured together.
  usb          the USB Connected screen comes in.

Ultra must hold its frame pacing (no hitches; p95 no worse than Fruit
Company's same motion + 3 ms, or within 36 ms full-screen / 20 ms band
for a motion Fruit Company does not have), must differ from Fruit
Company's motion, and with `animations: off` must end on Fruit Company's
frames. The host's load shows in the pacing, so a run with a pacing
failure records every motion once more; a motion passes if either
recording holds its pace, and result.json keeps both.

Arguments: ROCKBOXUI ZIP OUTPUT_DIRECTORY [--fixture DIRECTORY]
[--review DIRECTORY]. ZIP is the build's rockbox.zip after the bundle
steps. Writes result.json and the frames to OUTPUT_DIRECTORY, contact
sheets and side-by-side clips to the review directory, and prints PASS or
FAIL.
"""

import argparse
import json
import shutil
import sys
import time
from pathlib import Path

from PIL import Image

sys.dont_write_bytecode = True
from ultra_sim import (  # noqa: E402
    FPS, FRUIT, H, HITCH_MS, ULTRA, W, compare, difference, make_fixture,
    motion_span, pacing, perf_routes, write_review)

REVIEW = Path("/tmp/fruit-company-ultra-review/system")
# Regions each motion is measured in, leaving out the title marquee and
# the progress row's own ticking.
TEXT = (159, 68, 315, 161)
COVER = (9, 33, 145, 203)
ROW = (0, 203, W, 233)
BOXES = {"np-track": (9, 33, 315, 203), "np-seek": ROW,
         "np-seek-hold": ROW, "np-vol-in": ROW, "np-vol-out": ROW,
         "sheet-in": (0, 84, W, 203), "sheet-out": (0, 84, W, 203),
         "usb": (99, 42, 220, 164)}
# Where the end frames of the two themes must agree with motion off: away
# from the progress row, whose time depends on when the run paused.
END_BOXES = {"np-vol-in": ROW, "usb": None}
END_BOX = (0, 0, W, 203)
# Recorded for review only: holding Next steps the preview in jumps of the
# seek's own pace under both themes.
REVIEW_ONLY = ("np-seek-hold",)
# Where Ultra's motion must differ from Fruit Company's: the gloss
# brightens only part of the bar.
DIFFER_BOXES = {"np-seek": (54, 207, 266, 222)}
# Motions recorded more than once, their intervals measured together.
SHEET_CYCLES = 3
REPEATS = {name: [name] + ["%s-%d" % (name, n)
                           for n in range(2, SHEET_CYCLES + 1)]
           for name in ("sheet-in", "sheet-out")}
# Motions Fruit Company does not have, and whether they are full-screen.
NEW_MOTION = {"np-track": True, "usb": True}
FULL_P95_MS, BAND_P95_MS, SLACK_MS = 36, 20, 3
# The title would scroll through every recording.
CONFIG = "scrolling enabled: off\n"


def capture(sim, frames, events=()):
    """Every distinct frame for FRAMES at 60 fps, with EVENTS (seconds,
    key line) sent on the way; the first shot is the frame before them."""
    seen, got = sim.frames_since(0)
    shots = [(0.0, Image.frombytes("RGB", (W, H), got[-1]))] if got else []
    previous = got[-1] if got else None
    pending = sorted(events)
    start = time.monotonic()
    while time.monotonic() - start < frames / FPS:
        now = time.monotonic() - start
        while pending and now >= pending[0][0]:
            sim.send(pending.pop(0)[1])
        seen, got = sim.frames_since(seen)
        for pixels in got:
            if pixels != previous:
                shots.append((now * 1000,
                              Image.frombytes("RGB", (W, H), pixels)))
                previous = pixels
        time.sleep(0.002)
    for _, line in pending:
        sim.send(line)
    return shots


def tap(key, at=0.0):
    return [(at, "down " + key), (at + 2 / FPS, "up " + key)]


def usb_screen_up(shots):
    """The USB screen's frames, from the first that shows its backdrop
    where the list and Now Playing are white."""
    rest = shots[-1][1]
    for i, (_, image) in enumerate(shots):
        if all(image.getpixel(p) == rest.getpixel(p)
               for p in ((20, 120), (300, 120), (20, 40))):
            return shots[max(0, i - 1):]
    return shots


# A recording's motion starts where Fruit Company's own screen change ends.
TRIMS = {"usb": usb_screen_up}


def open_songs(sim):
    """Music > Songs > the first song, which plays every song in order:
    the next one is the same track number of the next album."""
    sim.tap("select", 60)
    for _ in range(5):
        sim.tap("down", 16)
    sim.tap("select", 60)
    sim.tap("select", 150)


def now_playing(sim, theme):
    open_songs(sim)
    shots = {}
    shots["np-track"] = capture(sim, 100, tap("right"))
    sim.wait(60)
    shots["np-seek-hold"] = capture(sim, 96, [(0, "down right")])
    # Paused as the scrub lands, so the progress row then holds still.
    shots["np-seek"] = capture(sim, 90, [(0, "up right")] +
                               tap("play", 0.05))
    sim.wait(60)
    shots["np-vol-in"] = capture(sim, 50, tap("up"))
    sim.wait(200)
    shots["np-vol-out"] = capture(sim, 80)
    sim.wait(30)
    for cycle in range(SHEET_CYCLES):
        suffix = "-%d" % (cycle + 1) if cycle else ""
        shots["sheet-in" + suffix] = capture(sim, 50, tap("usb"))
        sim.wait(20)
        # Charge only: the question sinks and Now Playing stays.
        shots["sheet-out" + suffix] = capture(sim, 40, tap("menu"))
        sim.tap("usb", 90)
    sim.tap("usb", 60)
    shots["usb"] = capture(sim, 90, tap("select"))
    sim.tap("usb", 120)
    return shots


def motion_frames(shots, box=None):
    """The frames of the motion: from the first change to the last."""
    return motion_span(shots, box=box)


def moving(shots, box=None):
    """SHOTS from the first frame that changed, leaving out the input's
    latency before it."""
    first = next((i for i in range(1, len(shots))
                  if difference(shots[i][1], shots[0][1], box) > 0), 0)
    return shots[first:]


def pooled(runs, box=None):
    """pacing() over several recordings of one motion, with their frame
    intervals taken together."""
    spans = [motion_span(moving(shots, box), box=box) for shots in runs]
    gaps = sorted(b[0] - a[0] for span in spans
                  for a, b in zip(span, span[1:]))
    if not gaps:
        return pacing(runs[0], box=box)
    return {"frames": len(spans[0]),
            "duration_ms": round(spans[0][-1][0] - spans[0][0][0], 1),
            "p50_ms": round(gaps[len(gaps) // 2], 1),
            "p95_ms": round(gaps[min(len(gaps) - 1,
                                     len(gaps) * 95 // 100)], 1),
            "max_ms": round(gaps[-1], 1),
            "hitches": sum(g > HITCH_MS for g in gaps),
            "recordings": len(runs)}


def differs(fruit, ultra, box=None):
    """Whether Ultra's motion shows frames Fruit Company's never does."""
    a = motion_frames(fruit, box)
    b = motion_frames(ultra, box)
    if len(a) < 2 or len(b) < 2:
        return len(a) != len(b)
    best = []
    for _, image in b[1:-1]:
        best.append(min(difference(image, other, box) for _, other in a))
    return max(best, default=0) > 1.0


def end_frames_equal(results, names):
    out = {}
    for name in names:
        a, b = results[FRUIT].get(name), results[ULTRA].get(name)
        if not a or not b:
            out[name] = None
            continue
        out[name] = round(difference(a[-1][1], b[-1][1],
                                     END_BOXES.get(name, END_BOX)), 3)
    return out


def record(args, fixture, folder):
    """One recording of every motion under both themes, and its checks."""
    results = compare(args.binary, fixture, args.output / folder, CONFIG,
                      now_playing)
    for shots in results.values():
        for name, trim in TRIMS.items():
            shots[name] = trim(shots[name])
    repeated = {n for names in REPEATS.values() for n in names[1:]}
    report = {name: {theme: pooled([results[theme][n]
                                    for n in REPEATS.get(name, [name])],
                                   BOXES.get(name))
                     for theme in (FRUIT, ULTRA)}
              for name in results[FRUIT] if name not in repeated}
    checks = {}
    for name, stats in report.items():
        if name in REVIEW_ONLY:
            continue
        fruit, ultra = stats[FRUIT], stats[ULTRA]
        box = BOXES.get(name)
        moved = differs(results[FRUIT][name], results[ULTRA][name],
                        DIFFER_BOXES.get(name, box))
        if name in NEW_MOTION:
            limit = FULL_P95_MS if NEW_MOTION[name] else BAND_P95_MS
        else:
            limit = (fruit["p95_ms"] if fruit else 0) + SLACK_MS
        ok = (ultra is not None and ultra["hitches"] == 0 and
              ultra["p95_ms"] <= limit and moved)
        checks[name] = {"fruit": fruit, "ultra": ultra, "limit_ms": limit,
                        "differs": moved, "ok": ok}
    return {"results": results, "checks": checks, "folder": folder}


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    for name in ("binary", "resource", "output"):
        parser.add_argument(name, type=lambda arg: Path(arg).resolve())
    parser.add_argument("--fixture", type=lambda arg: Path(arg).resolve())
    parser.add_argument("--review", type=lambda arg: Path(arg).resolve(),
                        default=REVIEW)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    fixture = args.fixture or args.output / "fixture"
    if not args.fixture:
        shutil.rmtree(fixture, ignore_errors=True)
        make_fixture(args.binary, args.resource, fixture)

    attempts = [record(args, fixture, "motion")]
    # The host's load shows in the pacing: a pacing failure is measured
    # once more, and a motion passes if either attempt holds its pace.
    first = attempts[0]["checks"].values()
    if (all(c["differs"] for c in first) and
            not all(c["ok"] for c in first)):
        attempts.append(record(args, fixture, "motion-2"))
    results = attempts[-1]["results"]
    write_review(results, args.review, boxes=BOXES)
    checks = {}
    failed = []
    for name in attempts[0]["checks"]:
        tries = [a["checks"][name] for a in attempts]
        ok = any(t["ok"] for t in tries)
        checks[name] = {"ok": ok, "attempts": tries}
        if not ok:
            failed.append(name)
    off = compare(args.binary, fixture, args.output / "off",
                  CONFIG + "animations: off\n", now_playing)

    still = end_frames_equal(off, results[FRUIT].keys())
    for name, delta in still.items():
        if delta is None or delta > 0:
            failed.append("off:" + name)
    perf = {theme: perf_routes(args.output / attempts[-1]["folder"] / theme)
            for theme in (FRUIT, ULTRA)}
    result = {"checks": checks, "motion_off_end_delta": still, "perf": perf,
              "failed": failed}
    (args.output / "result.json").write_text(json.dumps(result, indent=1) +
                                             "\n")
    for name, check in checks.items():
        for n, t in enumerate(check["attempts"], 1):
            print(name, "attempt", n, "ok" if t["ok"] else "FAIL",
                  "fruit", t["fruit"], "ultra", t["ultra"],
                  "limit", t["limit_ms"], "differs", t["differs"])
        print(name, "ok" if check["ok"] else "FAIL")
    print("motion off end delta", still)
    if failed:
        print("FAIL", *failed)
        sys.exit(1)
    print("PASS Ultra Now Playing, sheets and USB screen")


if __name__ == "__main__":
    main()
