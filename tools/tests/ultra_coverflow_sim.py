#!/usr/bin/env python3
"""Check Fruit Company Ultra's Cover Flow against Fruit Company, in SDL.

One script runs under both themes and records:

  entry        Music > Cover Flow: Ultra flies the covers in from depth.
  scroll       one wheel click: Ultra lands it on a short overshoot.
  spin         four quick clicks, then the landing.
  album-open   Select turns the album under Ultra's lamp.
  album-close  Menu turns it back.
  rise         Select+Play on another album: Now Playing rises over receding covers.
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
import csv
import json
import sys
import time
from pathlib import Path

from PIL import Image

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
from ultra_sim import (FRUIT, HITCH_MS, REFRESH_MS, ULTRA,  # noqa: E402
                       compare, difference, motion_span,
                       perf_routes, write_review)
from coverflow_back_stack_sim import prepare_fixture  # noqa: E402
from fruit_wheel_transport_sim import Playback, wait_for  # noqa: E402
from ui_stock_transitions_sim import FPS, H, W  # noqa: E402

REVIEW = Path("/tmp/fruit-company-ultra-review/coverflow")
ROUTES = ("cover-flow-entry", "cover-flow", "cover-flip-open",
          "cover-flip-close", "cover-flow-wps-rise", "cover-flow-wps-sink")
TIMED_ROUTES = {"entry": "cover-flow-entry", "album-close": "cover-flip-close"}
STEPS = ("entry", "scroll", "spin", "album-open", "album-close", "rise",
         "sink", "exit")
# A motion Ultra changes must differ somewhere by this mean per channel.
DIFFERS = 2.0
# Motion off: the end of each step under both themes, leaving out Now
# Playing's title marquee, whose phase depends on when the frame was taken.
# The Now Playing progress and text scroll depend on when playback paused.
SAME = 0.05
SAME_PAGE = 0.3
WHOLE = [None]
STATIC_WPS = [(0, 0, 320, 60), (0, 60, 155, 195), (155, 125, 320, 195)]
MENU_ROWS = [(0, 17, 150, 240)]
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
    keys = key.split()
    hold = (2 if key in ("up", "down") else 6) / FPS
    pending = [(0, "down " + name) for name in keys]
    pending += [(hold, "up " + name) for name in keys]
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


def press(sim, key, settle):
    sim.send("down " + key)
    sim.wait(6)
    sim.send("up " + key)
    sim.wait(settle)


def open_music(sim):
    # Boot can still consume the first SELECT. Wait for the title change
    # before sending the SELECT that is supposed to enter Cover Flow.
    title = (0, 0, 126, 17)
    _, frames = sim.frames_since(0)
    home = Image.frombytes("RGB", (W, H), frames[-1]).crop(title).tobytes()
    for _ in range(2):
        press(sim, "select", 80)
        _, frames = sim.frames_since(0)
        if Image.frombytes("RGB", (W, H), frames[-1]).crop(title).tobytes() != home:
            return
    raise AssertionError("Music did not open from Home")


def prepare_paused_track(sim):
    playback = Playback(Path(sim.process.args[0]), sim.process.pid)
    try:
        covers = playback.activities["ACTIVITY_ALBUMCOVERS"]
        wps = playback.activities["ACTIVITY_WPS"]
        assert playback.activity() == covers, "Album close left Cover Flow"
        assert not playback.get("album_panel", size=1), "Album close stayed in the album"
        press(sim, "select", 95)   # Reopen the centred album.
        wait_for(lambda: playback.get("album_panel", size=1), "The album did not open")
        press(sim, "select", 150)  # Start its first track.
        wait_for(lambda: playback.activity() == wps and playback.get("status") == 1,
                 "The album track did not start")
        press(sim, "play", 30)
        wait_for(lambda: playback.get("status") == 3, "Now Playing did not pause")
        press(sim, "menu", 120)
        wait_for(lambda: playback.activity() == covers, "MENU did not return to Cover Flow")
        if playback.get("album_panel", size=1):
            press(sim, "menu", 100)
            assert playback.activity() == covers, "Album close left Cover Flow"
            assert not playback.get("album_panel", size=1), "Album close stayed in the album"
    finally:
        playback.close()


def script(sim, theme):
    sim.wait(200)
    open_music(sim)
    out = {"entry": record(sim, 210, "select")}  # Cover Flow
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
    prepare_paused_track(sim)
    sim.wait(45)
    for _ in range(4):
        sim.tap("down", 16)
    sim.wait(55)
    out["rise"] = record(sim, 105, "select play")
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


def cadence(gaps):
    smooth = sorted((a + b) / 2 for a, b in zip(gaps, gaps[1:])
                    if max(a, b) <= HITCH_MS)
    p95 = round(smooth[min(len(smooth) - 1, len(smooth) * 95 // 100)], 1) \
        if smooth else 0.0
    return sum(g > HITCH_MS for g in gaps), p95


def steady(shots):
    """Hitches and the p95 of the other frame intervals of the motion,
    from its first frame: the wait for that frame is the screen's own
    (an album's first read, a page's first draw), not the motion's. The
    p95 is over the mean of each two neighbouring intervals, which keeps
    a late refresh and cancels a frame read late from the capture ring
    (40 ms then 19 ms) on a busy host."""
    span = motion_span(shots)[1:]
    gaps = [b[0] - a[0] for a, b in zip(span, span[1:])]
    return cadence(gaps)


def traced_cadence(folder, route):
    # Both routes have firmware timestamps. The LCD ring also records
    # auxiliary updates and timestamps the polling batch, which can make
    # an on-time animation frame look late or count a static redraw.
    lines = (folder / "ui-perf.txt").read_text().splitlines()
    for i, line in enumerate(lines):
        if not line.startswith(f"route={route} "):
            continue
        fields = dict(field.split("=", 1) for field in line.split())
        period = int(fields["period_us"]) / 1000
        rows = csv.DictReader(lines[i + 1:i + 2 + int(fields["rows"])])
        intervals = [float(row["interval_f"]) for row in rows]
        # One animation deadline can submit several LCD bands.
        gaps = [interval * period for interval in intervals if interval >= 0.5]
        if len(gaps) >= 3:
            return cadence(gaps)
    raise AssertionError(f"{folder}: missing {route} animation cadence")


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


def check_flow_path(folder, animations):
    for theme in (FRUIT, ULTRA):
        route_log = (folder / theme / "ui-perf.txt").read_text().splitlines()
        entry = "slide enter_why=ok" if animations else "cut enter_why=motion-off"
        assert f"cover-flow-path enter={entry} leave=none leave_why=-" in route_log, (
            theme, "Cover Flow did not open from Music", route_log[-5:])
        wps = [line for line in route_log if line.startswith("cover-flow-wps-path ")]
        assert len(wps) == 1 and wps[0].startswith(
            "cover-flow-wps-path path=agents-wps centre=unknown "
            + ("result=rise " if animations else "result=skipped reason=motion-off ")), (
                theme, wps)


def pacing_check(results, report, folder):
    """{step: failures} and {step: {theme: p95}} of one motion pass."""
    slow, rows = {}, {}
    for name in STEPS:
        if name in TIMED_ROUTES:
            (fruit_hitches, fruit_p95), (ultra_hitches, ultra_p95) = (
                traced_cadence(folder / theme, TIMED_ROUTES[name])
                for theme in (FRUIT, ULTRA))
        else:
            (fruit_hitches, fruit_p95), (ultra_hitches, ultra_p95) = (
                steady(results[theme][name]) for theme in (FRUIT, ULTRA))
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
    fixture = args.fixture or prepare_fixture(args.binary, args.resource, args.output)

    failures = []
    result = {"attempts": [], "differs": {}, "motion_off": {}, "perf": {}}
    paced = set()
    for attempt in range(ATTEMPTS):
        folder = args.output / ("motion" if not attempt
                                else f"motion-{attempt + 1}")
        results = compare(args.binary, fixture, folder, "", script)
        check_flow_path(folder, animations=True)
        for theme in results:
            results[theme]["entry"] = after_load(results[theme]["entry"])
        report = write_review(results, args.review)
        slow, rows = pacing_check(results, report, folder)
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
                  "animations: off\nscrolling enabled: off\n", script)
    check_flow_path(args.output / "motion-off", animations=False)
    for name in STEPS:
        # The Music pane pans independently of the page animation.
        boxes = MENU_ROWS if name == "exit" else STATIC_WPS if name == "rise" else WHOLE
        change = round(same(off[FRUIT][name][-1][1],
                            off[ULTRA][name][-1][1], boxes), 3)
        result["motion_off"][name] = change
        if change > (SAME_PAGE if boxes is STATIC_WPS else SAME):
            failures.append(f"{name}: motion off differs ({change})")
    write_review(off, args.review, prefix="off-")

    result["failures"] = failures
    (args.output / "result.json").write_text(json.dumps(result, indent=2))
    print(json.dumps(result, indent=2))
    print("FAIL" if failures else "PASS", *failures, sep="\n  ")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
