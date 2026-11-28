#!/usr/bin/env python3
"""Check Fruit Company Ultra's list motion against Fruit Company, in SDL.

Each theme runs the same keys in a simulator with no window:

  bar-step   the main menu, one detent: Ultra's bar glides on a spring and
             a glint crosses it once it settles; Fruit Company's jumps.
  row-glide  Songs, from its last visible row one detent on: Ultra's rows
             glide a row; Fruit Company's jump.
  spin       Songs, twelve detents 33 ms apart from the top, short of the
             end: Ultra's rows and bar keep moving without queueing;
             Fruit Company's jump.
  end-pull   Songs' last row, one detent on: Ultra's rows stretch past the
             end and spring back; Fruit Company's stand.
  indicator  the scroll indicator after row-glide: Ultra fades it every
             frame; Fruit Company in 14 steps of 34 ms.
  marquee    a long selected title: Ultra runs it a column a refresh,
             easing in and out; Fruit Company 2 columns every 34 ms.

For every animation it checks that Ultra moves where Fruit Company does
not, or more finely; that Ultra's frames come without a hitch and as
often as Fruit Company's, or as often as the simulator presents for a
motion Fruit Company lacks; and that both themes settle on the same
frame. A failure that other load on the host can cause alone -- of frame
timing, or themes that start a motion from different frames because a
delayed detent counted as a spin -- runs the motions once more;
result.json keeps both runs. A glide
is measured by where the bar or the rows stand in each
frame, over its travel: until it first comes within a pixel of rest, as
the spring's last pixel of overshoot then settles in frames of its own.
Last it runs the same keys with animations off, where every frame both
themes settle on must match.

Lists run without wraparound, as stock's do, so the wheel meets an end.
The simulator presents through SDL with vsync, at most 60 times a second,
so a motion the player presents at every refresh shows here at every
second one; the band limit allows for that, and the spin's limit for the
unchanged frame each detent presents (KEY_MS).

Arguments: ROCKBOXUI ZIP OUTPUT_DIRECTORY [--fixture DIRECTORY]
           [--review DIRECTORY]
Writes result.json, both themes' frames and ui-perf.txt to the output
directory, and the contact sheets and clips to the review directory
(/tmp/fruit-company-ultra-review/lists/ by default).
"""

import argparse
import json
import shutil
import sys
import time
from pathlib import Path

import numpy as np
from PIL import Image, ImageChops

sys.dont_write_bytecode = True
from ultra_sim import (FPS, FRUIT, H, ULTRA, W, compare,  # noqa: E402
                       difference, make_fixture, pacing, perf_routes,
                       write_review)
from ultra_sim import HITCH_MS, motion_span  # noqa: E402

# The rows, the indicator's column aside, which fades on its own clock.
LIST = (0, 20, W - 20, H)
MENU = (0, 20, W // 2, H)
INDICATOR = (W - 16, 20, W, H)
# Over the end of the list, the rows above the last two: the last holds
# the bar, whose title runs on its own clock.
PULL = (0, 20, W - 20, 190)
RISE_MS = 200
FULL_MS = 36
# A frame the panel would show at every refresh shows here at every second
# one, as SDL's vsync holds each present to 60 Hz.
BAND_MS = 36
# Each detent's key handling also presents an unchanged frame, which SDL
# holds for a refresh of its own, so a spin's frame can wait three
# refreshes in either theme; Fruit Company's lands on 33 or 50 ms from run
# to run. A spin's middle interval must still match Fruit Company's.
KEY_MS = 51
SLACK_MS = 3
SPIN_DETENTS = 12
SPIN_GAP_F = 2
MARQUEE_FROM_MS = 1250
FADE_FROM_MS = 300
SHIFT = 40
CONFIG = "list wraparound: off\n"
# The simulator posts a detent processed within 50 ms of the last as a
# spin, which skips rows; a host under load can delay one key that far.
INPUT = "input"


def drive(sim, frames, events):
    """Distinct frames for FRAMES after sending EVENTS, (seconds, line)."""
    seen, got = sim.frames_since(1 << 30)
    shots, previous = [], None
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
    assert shots, "no frame reached the panel"
    return shots


def current(sim):
    """The frame on the panel now, as shots."""
    seen, _ = sim.frames_since(1 << 30)
    _, got = sim.frames_since(seen - 1)
    assert got, "no frame reached the panel"
    return [(0, Image.frombytes("RGB", (W, H), got[-1]))]


def detents(key, count, gap_f=SPIN_GAP_F):
    return [((gap_f * i + held) / FPS, f"{action} {key}")
            for i in range(count)
            for held, action in enumerate(("down", "up"))]


def press(sim, key, frames):
    return drive(sim, frames, detents(key, 1))


def spin(sim, key, count):
    sim.wait(1)
    drive(sim, SPIN_GAP_F * count + 60, detents(key, count))


def to_songs(sim):
    sim.tap("up", 30)
    sim.tap("select", 90)
    for _ in range(5):
        sim.tap("down", 20)
    sim.tap("select", 120)


def script(sim, theme):
    sim.wait(120)
    out = {"bar-step start": current(sim)}
    out["bar-step"] = press(sim, "down", 70)
    to_songs(sim)
    for _ in range(8):
        sim.tap("down", 20)
    sim.wait(60)
    out["row-glide start"] = current(sim)
    out["row-glide"] = press(sim, "down", 80)
    out["indicator-rise"] = press(sim, "down", 30)
    spin(sim, "up", 30)
    sim.wait(60)
    out["spin"] = drive(sim, SPIN_GAP_F * SPIN_DETENTS + 70,
                        detents("down", SPIN_DETENTS))
    spin(sim, "down", 60)
    sim.wait(60)
    out["end-pull start"] = current(sim)
    out["end-pull"] = press(sim, "down", 60)
    sim.tap("up", 60)
    out["marquee"] = press(sim, "down", 250)
    out["indicator"] = out["row-glide"]
    return out


def still_script(sim, theme):
    """The same keys, keeping only the frame before each step and the
    frame it settles on."""
    out = {}

    def step(name, frames):
        before = current(sim)
        out[name] = before + press(sim, "down", frames)[-1:]

    sim.wait(120)
    step("bar-step", 70)
    to_songs(sim)
    for _ in range(8):
        sim.tap("down", 20)
    sim.wait(60)
    step("row-glide", 80)
    for _ in range(3):
        sim.tap("down", 20)
    step("steps", 60)
    spin(sim, "down", 60)
    sim.wait(60)
    step("end-pull", 60)
    return out


def after(shots, ms):
    """SHOTS from the last frame before MS on."""
    i = max((k for k, (t, _) in enumerate(shots) if t < ms), default=0)
    return shots[i:]


def run_from(shots, ms, box):
    """The frames of the first motion within BOX after MS, from its first
    changed frame to the last before a pause of 150 ms."""
    tail = after(shots, ms)
    span = motion_span(tail, box=box)
    return span[1:]


def glint_frames(shots, box=MENU):
    """Frames after the bar has arrived that are lighter than where it
    rests: the glint crossing it."""
    rest = shots[-1][1].crop(box).convert("L")
    count = 0
    for _, image in shots:
        lighter = ImageChops.subtract(image.crop(box).convert("L"), rest)
        if lighter.getextrema()[1] > 24 and difference(
                image, shots[-1][1], box) < 3:
            count += 1
    return count


def marquee_steps(shots, box):
    """The columns the selected title moves left between frames."""
    steps = []
    for (_, a), (_, b) in zip(shots, shots[1:]):
        a, b = a.crop(box), b.crop(box)
        best = None
        for dx in range(0, 7):
            error = difference(b.crop((0, 0, b.width - dx, b.height)),
                               a.crop((dx, 0, a.width, a.height)))
            if best is None or error < best[1]:
                best = (dx, error)
        steps.append(best[0])
    return steps


def gray(image, box):
    return np.asarray(image.crop(box).convert("L"), dtype=np.int16)


def bar_rows(image, box):
    """The rows of BOX the selection bar's blue fills."""
    rgb = np.asarray(image.crop(box), dtype=np.int16)
    blue = (rgb[:, :, 2] > 150) & (rgb[:, :, 0] < 120)
    return np.nonzero(blue.mean(axis=1) > 0.5)[0]


def bar_top(image, box):
    rows = bar_rows(image, box)
    return int(rows[0]) if len(rows) else -1


def shift(a, b):
    """The rows B is A moved down by, within SHIFT."""
    best = None
    for dy in range(-SHIFT, SHIFT + 1):
        if dy >= 0:
            error = np.abs(b[dy:] - a[:a.shape[0] - dy]).mean()
        else:
            error = np.abs(b[:dy] - a[-dy:]).mean()
        if best is None or error < best[1]:
            best = (dy, error)
    return best[0]


def positions(shots, box, bar):
    """Where the bar (BAR) or the rows stand in each of SHOTS, in pixels."""
    if bar:
        return [bar_top(image, box) for _, image in shots]
    out, at, last = [0], 0, gray(shots[0][1], box)
    for _, image in shots[1:]:
        now = gray(image, box)
        at += shift(last, now)
        out.append(at)
        last = now
    return out


def travel(shots, box, bar=False):
    """Frame times and positions from the frame before the first move to
    the first frame after its furthest point within a pixel of where the
    motion ends."""
    at = positions(shots, box, bar)
    first = next((i for i in range(1, len(at)) if at[i] != at[0]), None)
    if first is None:
        return [], []
    peak = max(range(first, len(at)), key=lambda i: abs(at[i] - at[-1]))
    end = next(i for i in range(peak, len(at)) if abs(at[i] - at[-1]) <= 1)
    span = range(first - 1, end + 1)
    return [shots[i][0] for i in span], [at[i] for i in span]


def travel_pacing(times, at):
    """The intervals of a travel after its first frame, which the key's
    latency sets. A hitch is an interval over HITCH_MS across which it
    moved 2 px or more; the percentiles count the intervals that moved
    2 px or more, as a stretch slower than a pixel a frame owes a frame
    only when a pixel changes."""
    gaps = [(b - a, abs(y - x)) for a, b, x, y in
            zip(times[1:], times[2:], at[1:], at[2:])]
    moving = sorted(g for g, d in gaps if d >= 2) or [0]
    return {"frames": max(0, len(times) - 1),
            "distance_px": at[-1] - at[0] if at else 0,
            "p50_ms": round(moving[len(moving) // 2], 1),
            "p95_ms": round(moving[min(len(moving) - 1,
                                       len(moving) * 95 // 100)], 1),
            "max_ms": round(max((g for g, _ in gaps), default=0), 1),
            "hitches": sum(1 for g, d in gaps if g > HITCH_MS and d >= 2)}


def interval_pacing(shots):
    """Intervals between SHOTS, all of which change."""
    gaps = sorted(b[0] - a[0] for a, b in zip(shots, shots[1:])) or [0]
    return {"frames": len(shots),
            "p50_ms": round(gaps[len(gaps) // 2], 1),
            "p95_ms": round(gaps[min(len(gaps) - 1, len(gaps) * 95 // 100)],
                            1),
            "max_ms": round(gaps[-1], 1),
            "hitches": sum(1 for g in gaps if g > HITCH_MS)}


def masked(image, box):
    """BOX of IMAGE as grey, with the selection bar's rows blanked: the
    marquee runs there on its own clock."""
    out = gray(image, box)
    out[bar_rows(image, box)] = 0
    return out


def ink(image, box):
    """How dark BOX is, the bar's rows aside."""
    grey = masked(image, box)
    return int((255 - grey)[grey > 0].sum())


def measure(results):
    fruit, ultra = results[FRUIT], results[ULTRA]
    report, failures = {}, []

    def fail(name, why):
        failures.append(f"{name}: {why}")

    boxes = {"bar-step": MENU, "row-glide": LIST, "spin": LIST,
             "end-pull": PULL}
    for name, box in boxes.items():
        bar = name == "bar-step"
        frames = (pacing(fruit[name], box=box), pacing(ultra[name], box=box))
        # A spin's rows look alike from row to row, so its frames are
        # timed as they come rather than by where the rows stand.
        if name == "spin":
            a, b = frames
        else:
            a = travel_pacing(*travel(fruit[name], box, bar))
            b = travel_pacing(*travel(ultra[name], box, bar))
        limit = (frames[0]["p95_ms"] + SLACK_MS if frames[0]["frames"] > 3
                 else BAND_MS if bar else FULL_MS)
        if name == "spin":
            limit = max(limit, KEY_MS)
            if b["p50_ms"] > a["p50_ms"] + SLACK_MS:
                fail(name, f"p50 {b['p50_ms']} ms over Fruit Company's "
                           f"{a['p50_ms']} ms")
        same = not np.any(masked(fruit[name][-1][1], box) !=
                          masked(ultra[name][-1][1], box))
        start = f"{name} start"
        began = start not in fruit or not np.any(
            masked(fruit[start][0][1], box) != masked(ultra[start][0][1], box))
        report[name] = {"fruit": a, "ultra": b, "p95_limit_ms": limit,
                        "frames": [frames[0]["frames"], frames[1]["frames"]],
                        "started_equal": began, "settled_equal": same}
        if b["hitches"]:
            fail(name, f"{b['hitches']} hitches")
        if b["p95_ms"] > limit:
            fail(name, f"p95 {b['p95_ms']} ms over {limit} ms")
        if b["frames"] < 3 or frames[1]["frames"] <= frames[0]["frames"]:
            fail(name, f"Ultra moves in {frames[1]['frames']} frames, Fruit "
                       f"Company in {frames[0]['frames']}")
        # A spin's last row depends on the wheel's speed, which the
        # simulator's timing varies.
        if not began:
            fail(name, f"{INPUT}: the themes start from different frames")
        elif name != "spin" and not same:
            fail(name, "the themes settle on different frames")

    glints = (glint_frames(fruit["bar-step"]), glint_frames(ultra["bar-step"]))
    report["glint"] = {"frames": glints}
    if glints[0] or glints[1] < 4:
        fail("glint", f"lighter bar frames {glints}")

    report["indicator"] = {}
    for theme, shots in (("fruit", fruit), ("ultra", ultra)):
        rise = [ink(image, INDICATOR) for ms, image in shots["indicator-rise"]
                if ms < RISE_MS]
        top = max(rise) or 1
        fade = run_from(shots["indicator"], FADE_FROM_MS, INDICATOR)
        report["indicator"][theme] = {
            "rising_frames": sum(1 for v in rise[1:]
                                 if 0.1 * top < v < 0.9 * top),
            "fade": interval_pacing(fade)}
    got = report["indicator"]
    if got["fruit"]["rising_frames"] or not got["ultra"]["rising_frames"]:
        fail("indicator", "only Ultra's indicator must rise")
    if got["ultra"]["fade"]["hitches"] or \
            got["ultra"]["fade"]["p95_ms"] > BAND_MS or \
            got["ultra"]["fade"]["frames"] < got["fruit"]["fade"]["frames"]:
        fail("indicator", f"fade {got['ultra']['fade']}")

    report["marquee"] = {}
    for theme, shots in (("fruit", fruit), ("ultra", ultra)):
        row = selected_row(shots["marquee"][-1][1])
        span = run_from(shots["marquee"], MARQUEE_FROM_MS, row) if row else []
        steps = marquee_steps(span, row) if span else []
        report["marquee"][theme] = {"row": row, "steps": steps[:12],
                                    "pacing": interval_pacing(span)}
    got = report["marquee"]
    if not got["fruit"]["steps"] or not got["ultra"]["steps"]:
        fail("marquee", "a theme's title did not run")
    elif got["ultra"]["steps"][0] >= got["fruit"]["steps"][0]:
        fail("marquee", "Ultra's run does not ease in")
    if got["ultra"]["pacing"]["hitches"] or \
            got["ultra"]["pacing"]["p95_ms"] > BAND_MS:
        fail("marquee", f"pacing {got['ultra']['pacing']}")
    return report, failures


def load_failure(line):
    """Whether a failure is one that other load on the host can cause: of
    frame timing, or of a detent the simulator took as part of a spin."""
    return any(word in line for word in ("hitches", "p95", "p50", "pacing",
                                         "fade {", INPUT))


def selected_row(image):
    """The box of the bar's rows in IMAGE, by its blue, or None."""
    rows = bar_rows(image, (0, 0, W - 24, H))
    if not len(rows):
        return None
    return (6, int(rows[0]) + 2, W - 30, int(rows[-1]) - 1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("binary", "resource", "output"):
        parser.add_argument(name, type=lambda arg: Path(arg).resolve())
    parser.add_argument("--fixture", type=lambda arg: Path(arg).resolve())
    parser.add_argument("--review", type=lambda arg: Path(arg).resolve(),
                        default=Path("/tmp/fruit-company-ultra-review/lists"))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    fixture = args.fixture or args.output / "fixture"
    if not args.fixture:
        shutil.rmtree(fixture, ignore_errors=True)
        make_fixture(args.binary, args.resource, fixture)

    # The host runs other simulators, so a pacing failure alone earns one
    # more run; both runs are recorded and the second decides.
    attempts = []
    for attempt in range(2):
        results = compare(args.binary, fixture, args.output / "motion",
                          CONFIG, script)
        report, failures = measure(results)
        attempts.append({"failures": failures, **report})
        if not failures or not all(load_failure(f) for f in failures):
            break
    report["attempts"] = attempts
    report["review"] = write_review(
        {theme: {k: v for k, v in shots.items()
                 if k not in ("indicator", "indicator-rise") and
                 not k.endswith(" start")}
         for theme, shots in results.items()}, args.review,
        boxes={"bar-step": MENU, "row-glide": LIST, "spin": LIST,
               "end-pull": LIST, "marquee": LIST})
    report["perf"] = {
        theme: {route: perf_routes(args.output / "motion" / theme, route)
                for route in ("list-glide", "list-glint")}
        for theme in results}

    report["animations_off"] = []
    for attempt in range(2):
        still = compare(args.binary, fixture, args.output / "still",
                        CONFIG + "animations: off\n", still_script)
        off, retry = {}, False
        for name in still[FRUIT]:
            began, same = (not np.any(
                masked(still[FRUIT][name][i][1], (0, 0, W, H)) !=
                masked(still[ULTRA][name][i][1], (0, 0, W, H)))
                for i in (0, -1))
            off[name] = {"started_equal": began, "settled_equal": same}
            retry |= not began
        report["animations_off"].append(off)
        if not retry:
            break
    for name, got in off.items():
        if not got["started_equal"]:
            failures.append(f"{name}: {INPUT}: the themes start from "
                            "different frames with animations off")
        elif not got["settled_equal"]:
            failures.append(f"{name}: frames differ with animations off")

    report["failures"] = failures
    (args.output / "result.json").write_text(json.dumps(report, indent=2))
    for line in failures:
        print("FAIL", line)
    print("PASS" if not failures else "FAIL")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
