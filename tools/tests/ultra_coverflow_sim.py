#!/usr/bin/env python3
"""Check Fruit Company Ultra's Cover Flow against Fruit Company, in SDL.

One script runs under both themes and records:

  entry        Music > Cover Flow: Ultra flies the covers in from depth.
  scroll       one wheel click: Ultra lands it on a short overshoot.
  spin         four quick clicks, then the landing.
  album-open   Select turns the album under Ultra's lamp.
  album-close  Menu turns it back.
  rise         Play on another album: the page rises over receding covers.
  sink         Menu sinks it again.
  exit         Menu leaves Cover Flow: Ultra's covers recede.

For each, Ultra must hold Fruit Company's pacing (no hitch Fruit Company
does not show, p95 interval within 3 ms of Fruit Company's) in one of
ATTEMPTS recordings, and must differ from it. With
`animations: off` the end of every step must look the same under both
themes. ui-perf summaries of both themes are printed and kept.

Arguments: ROCKBOXUI ZIP OUTPUT_DIR [--fixture DIR] [--review DIR]
Writes result.json in OUTPUT_DIR and the review into --review
(default /tmp/fruit-company-ultra-review/coverflow/).
"""

import argparse
import json
import sys
import time
from pathlib import Path

from PIL import Image

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
from ultra_sim import (FRUIT, HITCH_MS, REFRESH_MS, ULTRA,  # noqa: E402
                       compare, difference, make_fixture, motion_span,
                       perf_routes, write_review)
from ui_stock_transitions_sim import FPS, H, W  # noqa: E402

REVIEW = Path("/tmp/fruit-company-ultra-review/coverflow")
ROUTES = ("cover-flow-entry", "cover-flow", "cover-flip-open",
          "cover-flip-close", "cover-flow-wps-rise", "cover-flow-wps-sink")
STEPS = ("entry", "scroll", "spin", "album-open", "album-close", "rise",
         "sink", "exit")
# A motion Ultra changes must differ somewhere by this mean per channel.
DIFFERS = 2.0
# Motion off: the end of each step under both themes, leaving out Now
# Playing's title marquee, whose phase depends on when the frame was taken.
# The art's lower edge and the progress bar on that page move with the
# time the frame was taken, between two Fruit Company runs as well, hence
# the page's larger allowance; an Ultra page (scaled, over dimmed covers)
# differs by several units.
SAME = 0.05
SAME_PAGE = 0.3
WHOLE = [None]
NOT_MARQUEE = [(0, 0, 320, 60), (0, 60, 155, 240), (155, 125, 320, 240)]
SLACK_MS = 3.0
# A busy host delays single frames by several ms; a step whose pacing fails
# is recorded again under both themes, and passes if any attempt does.
ATTEMPTS = 2


def record(sim, frames, key, second=None, at_ms=0):
    """Every frame submitted in FRAMES after KEY, repeats included, after
    the frame before it; SECOND follows AT_MS later. A pose that holds
    still for a frame (the top of an overshoot) is a repeat, not a gap."""
    seen, got = sim.frames_since(0)
    shots = [(0.0, Image.frombytes("RGB", (W, H), got[-1]))]
    pending = [(0, "down " + key), (2 / FPS, "up " + key)]
    if second:
        pending += [(at_ms / 1000, "down " + second),
                    (at_ms / 1000 + 2 / FPS, "up " + second)]
    pending.sort()
    start = time.monotonic()
    while time.monotonic() - start < frames / FPS:
        now = time.monotonic() - start
        while pending and now >= pending[0][0]:
            sim.send(pending.pop(0)[1])
        count, got = sim.frames_since(seen)
        assert len(got) == count - seen, "the capture ring lost an LCD frame"
        seen = count
        shots.extend((now * 1000, Image.frombytes("RGB", (W, H), data))
                     for data in got)
        time.sleep(0.002)
    for _, line in pending:
        sim.send(line)
    assert sim.process.poll() is None, "the simulator stopped"
    return shots


def script(sim, theme):
    sim.wait(200)
    sim.tap("select", 40)                       # Music
    out = {"entry": record(sim, 150, "select")}  # Cover Flow
    sim.wait(40)
    out["scroll"] = record(sim, 80, "down")
    sim.wait(30)
    out["spin"] = record(sim, 110, "down", "down", 60)
    sim.tap("down", 6)
    sim.tap("down", 70)
    sim.wait(20)
    out["album-open"] = record(sim, 110, "select")
    sim.wait(30)
    out["album-close"] = record(sim, 100, "menu")
    sim.wait(40)
    sim.tap("play", 205)                        # start the centred album
    sim.tap("menu", 120)                        # turn back to the covers
    sim.wait(45)
    for _ in range(4):
        sim.tap("down", 16)
    sim.wait(55)
    out["rise"] = record(sim, 70, "play")
    sim.wait(30)
    out["sink"] = record(sim, 85, "menu")
    sim.wait(40)
    out["exit"] = record(sim, 60, "menu")
    return out


def after_load(shots, gap_ms=500):
    """SHOTS from the frame before the motion, past the load that can hold
    the panel still for seconds after the key."""
    for i in range(1, len(shots)):
        if shots[i][0] - shots[i - 1][0] > gap_ms:
            still = (shots[i][0] - 2 * REFRESH_MS, shots[i - 1][1])
            return [still] + shots[i:]
    return shots


def steady(shots):
    """Hitches and the p95 of the other frame intervals of the motion,
    from its first frame: the wait for that frame is the screen's own
    (an album's first read, a page's first draw), not the motion's. The
    p95 is over the mean of each two neighbouring intervals, which keeps
    a late refresh and cancels a frame read late from the capture ring
    (40 ms then 19 ms) on a busy host."""
    span = motion_span(shots)[1:]
    gaps = [b[0] - a[0] for a, b in zip(span, span[1:])]
    smooth = sorted((a + b) / 2 for a, b in zip(gaps, gaps[1:])
                    if max(a, b) <= HITCH_MS)
    p95 = round(smooth[min(len(smooth) - 1, len(smooth) * 95 // 100)], 1) \
        if smooth else 0.0
    return sum(g > HITCH_MS for g in gaps), p95


def same(a, b, boxes):
    return max(difference(a, b, box) for box in boxes)


def differs(a, b):
    """The largest difference between a frame of A and B's frame at the
    same time from the key."""
    worst = 0.0
    for ms, image in a:
        other = min(b, key=lambda shot: abs(shot[0] - ms))[1]
        worst = max(worst, difference(image, other))
    return round(worst, 2)


def perf_summary(folder):
    rows = {}
    for route in ROUTES:
        lines = perf_routes(folder, route)
        if lines:
            keys = ("seen", "work_p99_f", "work_worst_f", "render_worst_f",
                    "interval_p99_f", "present_gaps", "rate")
            rows[route] = [{k: line.get(k) for k in keys} for line in lines]
    return rows


def pacing_check(results, report):
    """{step: failures} and {step: {theme: p95}} of one motion pass."""
    slow, rows = {}, {}
    for name in STEPS:
        (fruit_hitches, fruit_p95), (ultra_hitches, ultra_p95) = \
            steady(results[FRUIT][name]), steady(results[ULTRA][name])
        rows[name] = {FRUIT: fruit_p95, ULTRA: ultra_p95}
        slow[name] = []
        if ultra_hitches > fruit_hitches:
            slow[name].append(f"{name}: {ultra_hitches} hitches in Ultra, "
                              f"{fruit_hitches} in Fruit Company")
        limit = fruit_p95 + SLACK_MS \
            if report[name][FRUIT]["frames"] > 2 else 36.0
        if ultra_p95 > limit:
            slow[name].append(f"{name}: Ultra p95 {ultra_p95} ms > {limit}")
    return slow, rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("binary", "resource", "output"):
        parser.add_argument(name, type=lambda p: Path(p).resolve())
    parser.add_argument("--fixture", type=lambda p: Path(p).resolve())
    parser.add_argument("--review", type=lambda p: Path(p).resolve(),
                        default=REVIEW)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    fixture = args.fixture or args.output / "fixture"
    if not args.fixture:
        make_fixture(args.binary, args.resource, fixture)

    failures = []
    result = {"attempts": [], "differs": {}, "motion_off": {}, "perf": {}}
    paced = set()
    for attempt in range(ATTEMPTS):
        folder = args.output / ("motion" if not attempt
                                else f"motion-{attempt + 1}")
        results = compare(args.binary, fixture, folder, "", script)
        for theme in results:
            results[theme]["entry"] = after_load(results[theme]["entry"])
        report = write_review(results, args.review)
        slow, rows = pacing_check(results, report)
        result["attempts"].append({"pacing": report, "steady_p95": rows,
                                   "slow": slow})
        paced |= {name for name in STEPS if not slow[name]}
        if not attempt:
            for name in STEPS:
                result["differs"][name] = differs(results[ULTRA][name],
                                                  results[FRUIT][name])
                if result["differs"][name] < DIFFERS:
                    failures.append(f"{name}: Ultra looks like Fruit Company "
                                    f"({result['differs'][name]})")
            for theme in (FRUIT, ULTRA):
                result["perf"][theme] = perf_summary(folder / theme)
        if paced == set(STEPS):
            break
    for name in STEPS:
        if name not in paced:
            failures += result["attempts"][-1]["slow"][name]

    off = compare(args.binary, fixture, args.output / "motion-off",
                  "animations: off\n", script)
    for name in STEPS:
        boxes = NOT_MARQUEE if name in ("rise", "sink") else WHOLE
        change = round(same(off[FRUIT][name][-1][1],
                            off[ULTRA][name][-1][1], boxes), 3)
        result["motion_off"][name] = change
        if change > (SAME_PAGE if boxes is NOT_MARQUEE else SAME):
            failures.append(f"{name}: motion off differs ({change})")
    write_review(off, args.review, prefix="off-")

    result["failures"] = failures
    (args.output / "result.json").write_text(json.dumps(result, indent=2))
    print(json.dumps(result, indent=2))
    print("FAIL" if failures else "PASS", *failures, sep="\n  ")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
