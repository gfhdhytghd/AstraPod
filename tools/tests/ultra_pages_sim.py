#!/usr/bin/env python3
"""Check Fruit Company Ultra's page motion against Fruit Company, in SDL.

Records under both themes, with Fruit Company left and Ultra right in the
review:

  home-music      main menu to Music: two split menus
  music-home      and back
  music-artists   Music to Artists: a split menu to a full list
  artists-albums  Artists to an artist's albums: two full lists
  albums-artists  and back
  artists-music   Artists to Music
  menu-np         the main menu to Now Playing
  np-menu         and back
  pane-row        a new row's pane replacing the last one
  shutdown        the shutdown fade

Each motion is recorded RUNS times and its frame intervals pooled. Each
Ultra motion must keep its pace from its first moving frame: no more
hitches than Fruit Company's same motion, and a p95 frame interval no worse
than Fruit Company's by more than SLACK_MS, or for a motion Fruit Company
does not have, no hitch and a p95 no worse than OWN_P95_MS. A motion ends
at its last change outside the lists' scroll bars. Now Playing's
clock ticks every 130 ms or so once it is in, so its motions settle after
SETTLE_NP_MS without a change. Each must
differ from Fruit Company's somewhere in its course, and with Animations
off the two themes must end on the same frames.

Arguments: ROCKBOXUI ZIP OUTPUT_DIRECTORY [--fixture DIRECTORY]
[--review DIRECTORY]. Writes result.json, both themes' frames and ui-perf
logs, and per motion a contact sheet and a side-by-side MP4 into the
review directory.
"""

import argparse
import json
import shutil
import sys
import time
from pathlib import Path

from PIL import Image, ImageDraw

sys.dont_write_bytecode = True
from ultra_sim import (  # noqa: E402
    FRUIT, HITCH_MS, ULTRA, compare, difference, make_fixture, motion_span,
    perf_routes, write_review)

REVIEW = Path("/tmp/fruit-company-ultra-review/pages")
SLACK_MS = 3.0
# Each motion is recorded this many times, in fresh sessions, and its
# intervals pooled. The host is shared, so a set whose pacing fails is
# recorded once more and judged by the second; result.json keeps both.
RUNS = 3
ATTEMPTS = 2
# The pane's motions, Fruit Company's included, present at every second
# refresh, so its dissolve is held to the full-screen bound.
OWN_P95_MS = {"pane-row": 36.0}
# A frame differs from the other theme's whole motion by at least this.
DISTINCT = 2.0
# A page's motion has started once a frame differs from the one before the
# key by this much.
STARTED = 0.5
# The pane, for the motion of a new row's pane.
PANE = (160, 0, 320, 240)
SETTLE_NP_MS = 100
# The columns of a split menu's scroll bar and a full list's.
SCROLL_BARS = ((152, 160), (312, 320))


def pages(sim, theme):
    out = {}
    sim.wait(120)
    out["home-music"] = sim.record(45, "select")
    out["music-home"] = sim.record(45, "menu")
    sim.tap("select", 60)
    sim.tap("down", 50)
    sim.tap("down", 50)
    out["music-artists"] = sim.record(45, "select")
    sim.wait(30)
    out["artists-albums"] = sim.record(45, "select")
    sim.wait(30)
    out["albums-artists"] = sim.record(45, "menu")
    sim.wait(30)
    out["artists-music"] = sim.record(45, "menu")
    sim.wait(30)
    out["pane-row"] = sim.record(60, "up")
    sim.tap("menu", 60)
    for _ in range(6):
        sim.tap("down", 20)
    sim.wait(40)
    sim.tap("select", 150)
    out["np-menu"] = sim.record(50, "menu")
    sim.wait(40)
    sim.tap("down", 50)
    out["menu-np"] = sim.record(50, "select")
    sim.wait(30)
    return out


def shutdown(sim, theme):
    """Hold Play until the player shuts down, recording every frame."""
    sim.wait(120)
    seen, _ = sim.frames_since(1 << 30)
    sim.send("down play")
    shots, previous, start = [], None, time.monotonic()
    while time.monotonic() - start < 6 and sim.process.poll() is None:
        seen, got = sim.frames_since(seen)
        for pixels in got:
            if pixels != previous:
                shots.append(((time.monotonic() - start) * 1000,
                              Image.frombytes("RGB", (320, 240), pixels)))
                previous = pixels
        time.sleep(0.002)
    # The fade starts after the hold, from the "Shutting down" frame.
    return {"shutdown": shots[1:]}


def still(sim, theme):
    """The same walk with Animations off: each step's settled frame."""
    out = {}
    sim.wait(120)
    for name, key in (("music", "select"), ("artists", None),
                      ("albums", "select"), ("back", "menu"),
                      ("home", "menu")):
        if name == "artists":
            sim.tap("down", 50)
            sim.tap("down", 50)
            key = "select"
        out[name] = sim.record(60, key)[-1:]
    return out


def moving(name, shots):
    """SHOTS from the first frame that moves, so that the key's latency, and
    for a pane the pause before it changes, are not counted. A list's scroll
    bar can change first, so a page's motion starts with the first frame
    that differs by more than STARTED."""
    box = PANE if name == "pane-row" else None
    least = 0 if box else STARTED
    start = next((i for i in range(1, len(shots))
                  if difference(shots[i][1], shots[0][1], box) > least),
                 len(shots) - 1)
    return shots[start:], box


def masked(image):
    """IMAGE without the lists' scroll bars, which fade on their own once a
    page is in."""
    image = image.copy()
    draw = ImageDraw.Draw(image)
    for x0, x1 in SCROLL_BARS:
        draw.rectangle((x0, 20, x1 - 1, 239), fill=(0, 0, 0))
    return image


def gaps(name, shots):
    """The frame intervals of the motion in SHOTS, in ms."""
    shots, box = moving(name, shots)
    settle = SETTLE_NP_MS if "np" in name else 150
    if not box:
        shots = [(ms, masked(image)) for ms, image in shots]
        box = (0, 0, 320, 240)
    span = motion_span(shots, settle, box)
    return [b[0] - a[0] for a, b in zip(span, span[1:])]


def pooled(name, runs):
    """pacing() over the motions NAME of several RUNS together."""
    pool = sorted(g for run in runs for g in gaps(name, run[name]))
    if not pool:
        return {"intervals": 0, "p50_ms": 0, "p95_ms": 0, "max_ms": 0,
                "hitches": 0}
    return {"intervals": len(pool),
            "p50_ms": round(pool[len(pool) // 2], 1),
            "p95_ms": round(pool[min(len(pool) - 1, len(pool) * 95 // 100)],
                            1),
            "max_ms": round(pool[-1], 1),
            "hitches": sum(g > HITCH_MS for g in pool)}


def distinct(name, fruit, ultra):
    """Ultra's motion frames that match no frame of Fruit Company's."""
    a, box = moving(name, fruit)
    b, _ = moving(name, ultra)
    a, b = motion_span(a, box=box), motion_span(b, box=box)
    return sum(1 for _, image in b
               if min(difference(image, other, box) for _, other in a) >
               DISTINCT)


def record(args, fixture, attempt):
    """RUNS recordings of every motion under both themes."""
    runs = []
    for i in range(RUNS):
        tag = f"{attempt}-{i + 1}"
        run = compare(args.binary, fixture, args.output / f"motion{tag}",
                      "", pages)
        down = compare(args.binary, fixture, args.output / f"down{tag}",
                       "", shutdown)
        for theme in run:
            run[theme].update(down[theme])
        runs.append(run)
    return runs


def paced(runs):
    """Each motion's pooled pacing under both themes, and the motions whose
    Ultra pacing fails."""
    motion, failed = {}, []
    for name in sorted(runs[0][ULTRA]):
        fruit = pooled(name, [run[FRUIT] for run in runs])
        ultra = pooled(name, [run[ULTRA] for run in runs])
        motion[name] = {FRUIT: fruit, ULTRA: ultra}
        own = OWN_P95_MS.get(name)
        allowed = 0 if own else fruit["hitches"]
        if ultra["hitches"] > allowed:
            failed.append((name, "hitches", ultra, allowed))
        limit = own or fruit["p95_ms"] + SLACK_MS
        if ultra["p95_ms"] > limit:
            failed.append((name, "p95", ultra["p95_ms"], limit))
    return motion, failed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
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

    attempts = []
    for attempt in range(1, ATTEMPTS + 1):
        runs = record(args, fixture, attempt)
        motion, failed = paced(runs)
        attempts.append({"runs": runs, "motion": motion, "failed": failed})
        if not failed:
            break
        print(f"attempt {attempt}: pacing", *failed)
    runs = attempts[-1]["runs"]
    results = runs[0]
    write_review(results, args.review, boxes={"pane-row": PANE})
    off = compare(args.binary, fixture, args.output / "off",
                  "animations: off\n", still)

    failed = list(attempts[-1]["failed"])
    result = {"attempts": [{"motion": a["motion"],
                            "failed": [list(map(str, f)) for f in a["failed"]]}
                           for a in attempts],
              "motion": attempts[-1]["motion"], "distinct": {}, "off": {},
              "perf": {}}
    for name in sorted(results[ULTRA]):
        frames = distinct(name, results[FRUIT][name], results[ULTRA][name])
        result["distinct"][name] = frames
        if frames < 2:
            failed.append((name, "Ultra matches Fruit Company", frames))
    for name in off[FRUIT]:
        a, b = off[FRUIT][name][-1][1], off[ULTRA][name][-1][1]
        result["off"][name] = round(difference(a, b), 3)
        if difference(a, b) > 0:
            failed.append((name, "Animations off differs", difference(a, b)))
    last = len(attempts)
    for theme in (FRUIT, ULTRA):
        result["perf"][theme] = [
            perf_routes(args.output / f"motion{last}-{i + 1}" / theme)
            for i in range(RUNS)]
    (args.output / "result.json").write_text(json.dumps(result, indent=1) +
                                             "\n")
    for failure in failed:
        print("FAIL", *failure)
    if failed:
        sys.exit(1)
    print("PASS Ultra pages: " + ", ".join(
        f"{name} p95 {themes[ULTRA]['p95_ms']} ms"
        for name, themes in sorted(result["motion"].items())))


if __name__ == "__main__":
    main()
