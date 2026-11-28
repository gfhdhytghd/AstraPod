#!/usr/bin/env python3
"""Check Fruit Company and Ultra split-menu slides in the headless simulator.

Stock 2.0.5's full list crosses the screen in 270 ms. The list on the
split page moves only while the full list overlaps its half. The full
list progressively covers or reveals the pane at the same time.
A split-to-split transition leaves the preview at its resting x coordinate.
"""

import argparse
import json
import sys
from pathlib import Path

import numpy
from PIL import Image

sys.dont_write_bytecode = True
from ui_fruit_flow_entry_sim import HALF, SHADOW, SLIDE_MS, gray
from ui_stock_transitions_sim import H, W, make_fixture, session

SLIDE_SLOTS = 18


def albums(sim):
    sim.tap("select", 65)
    for _ in range(3):
        sim.tap("down", 16)
    sim.wait(15)
    push = sim.record(55, "select")
    return {"push": push, "pop": sim.record(55, "menu")}


def playlists(sim):
    sim.wait(45)
    _, frames = sim.frames_since(0)
    before = Image.frombytes("RGB", (W, H), frames[-1])
    return {"split-push": [(0, before)] + sim.record(55, "select")}


def error(a, b):
    return round(float(numpy.abs(a - b).mean()), 2)


def mixed(shots, push):
    """Track the full list's left edge and the pane behind it."""
    before, after = (gray(im)[28:165] for _, im in (shots[0], shots[-1]))
    split, full = (before, after) if push else (after, before)
    track = []
    pane_errors = []
    row_errors = []
    for ms, image in shots:
        now = gray(image)[28:165]
        mismatch, edge = min((error(now[:, x:], full[:, :W - x]), x)
                             for x in range(W - 42))
        if mismatch >= 3 or edge < 4:
            continue
        track.append((ms, edge))
        if edge > HALF + 20:
            pane_errors.append(error(now[:, HALF + 4:edge],
                                     split[:, HALF + 4:edge]))
        elif 45 < edge < HALF - 4:
            row_errors.append(error(now[:, :edge - 12],
                                    split[:, HALF - edge:HALF - 12]))
    # Stock covers 320 px in 270 ms. The simulator can skip a panel slot
    # when the 60 Hz host polls the 15 ms animation clock. Check both the
    # pixel error at the sampled slot and the wall time of that slot.
    errors = []
    starts = []
    for ms, edge in track:
        distance = W - edge if push else edge
        slot = round(SLIDE_SLOTS * distance / W)
        errors.append(abs(distance - W * slot / SLIDE_SLOTS))
        starts.append(ms - slot * SLIDE_MS / SLIDE_SLOTS)
    jitter = max(starts) - min(starts) if starts else None
    return {"track": track, "jitter_ms": jitter,
            "worst_px": max(errors, default=None),
            "pane_errors": pane_errors, "row_errors": row_errors}


def split_push(shots, push=True):
    """Measure the moving rows and compare the right pane with both endpoints."""
    before, after = (gray(im)[24:170] for _, im in (shots[0], shots[-1]))
    track = []
    for ms, image in shots:
        now = gray(image)[24:170]
        left, right = (before, after) if push else (after, before)
        candidates = []
        for x in range(12, HALF - 12):
            seam = x if push else HALF - x
            # The seam casts a shadow into the right-hand rows.
            left_error = error(now[:, :seam], left[:, HALF - seam:HALF])
            right_error = 0
            if seam + SHADOW < HALF:
                right_error = error(now[:, seam + SHADOW:HALF],
                                    right[:, SHADOW:HALF - seam])
            candidates.append((max(left_error, right_error), x))
        row_error, seam = min(candidates)
        if row_error > 3 or seam < 12:
            continue
        track.append((ms, seam, now[:, HALF:]))

    # Compare with the preview as the rows finish sliding. The Home art
    # can pan on its own later while the recording is still running.
    settled = after
    if track:
        for ms, image in shots:
            if ms > track[-1][0]:
                now = gray(image)[24:170]
                if error(now[:, :HALF], after[:, :HALF]) <= 3:
                    settled = now
                    break
    return {"different": error(before[:, HALF:], settled[:, HALF:]),
            "frames": [(ms, seam, error(pane, before[:, HALF:]),
                        error(pane, settled[:, HALF:]))
                       for ms, seam, pane in track]}


def round_trip(home_row, menu_row):
    def record(sim):
        for _ in range(home_row):
            sim.tap("down", 25)
        if menu_row is not None:
            sim.tap("select", 55)
            for _ in range(menu_row):
                sim.tap("down", 25)
        sim.wait(80)
        shots = {}
        for route, key in (("push", "select"), ("pop", "menu")):
            _, frames = sim.frames_since(0)
            before = Image.frombytes("RGB", (W, H), frames[-1])
            shots[route] = [(-1, before)] + sim.record(52, key)
        return shots
    return record


def mixed_samples(shots, track, push, folder):
    """Save the measured quarter, midpoint and three-quarter positions."""
    samples = []
    for fraction in (25, 50, 75):
        edge = W * (100 - fraction if push else fraction) // 100
        if not track:
            break
        ms, x = min(track, key=lambda sample: abs(sample[1] - edge))
        image = next(im for t, im in shots if t == ms)
        image.save(folder / f"progress-{fraction}.png")
        samples.append(x)
    return samples


def check_both_themes(common, out):
    routes = {"extras-clock": (4, 0), "music-albums": (0, 3),
              "settings-about": (5, 0), "home-music": (0, None)}
    results = {}
    failures = []
    for theme in ("fruit-company", "fruit-company-ultra"):
        results[theme] = {}
        for route, rows in routes.items():
            folder = out / theme / route
            # Playlists changes the pane as Music arrives, exposing any
            # lateral movement even when the preview on either page is static.
            extra = "fruit music menu: 2\n" if route == "home-music" else ""
            shots = session(*common, folder, theme, extra, round_trip(*rows))
            for key, frames in shots.items():
                push = key == "push"
                tag = f"{route}-{key}"
                if route == "home-music":
                    result = split_push(frames, push)
                    valid = (result["different"] >= 8 and
                             len(result["frames"]) >= 3 and
                             max(after for _, _, _, after in result["frames"]) <= 3)
                    track = [(ms, seam * 2) for ms, seam, _, _
                             in result["frames"]]
                    # The rows on a pop arrive from the left. The measured
                    # offset is their remaining travel, just as on a push.
                    result["quarter_edges"] = mixed_samples(
                        frames, track, True, folder / key)
                else:
                    result = mixed(frames, push)
                    valid = (len(result["track"]) >= 6 and
                             len(result["pane_errors"]) >= 2 and
                             len(result["row_errors"]) >= 2 and
                             max(result["pane_errors"] + result["row_errors"]) <= 7)
                    result["quarter_edges"] = mixed_samples(
                        frames, result["track"], push, folder / key)
                results[theme][tag] = result
                expected = [240, 160, 80] if push or route == "home-music" else [80, 160, 240]
                edges = result["quarter_edges"]
                if not valid or len(edges) != 3 or any(
                        abs(got - want) > 28 for got, want in zip(edges, expected)):
                    failures.append((theme, tag, result))
    (out / "result.json").write_text(json.dumps(results, indent=1) + "\n")
    for failure in failures:
        print("FAIL", *failure)
    if failures:
        sys.exit(1)
    print("PASS both Fruit themes keep the preview fixed on page changes")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("binary", "resource", "output"):
        parser.add_argument(name, type=lambda arg: Path(arg).resolve())
    parser.add_argument("--fixture", type=lambda arg: Path(arg).resolve())
    parser.add_argument("--both-themes", action="store_true",
                        help="check retained pages and capture quarter poses in both themes")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    fixture = args.fixture or args.output / "fixture"
    if not args.fixture:
        make_fixture(args.binary, args.resource, fixture)
    common = (args.binary, fixture)
    if args.both_themes:
        check_both_themes(common, args.output)
        return
    shots = session(*common, args.output / "albums", "fruit-company", "", albums)
    # Make Playlists the first Music row so the incoming pane differs from
    # the Music preview on Home.
    split = session(*common, args.output / "playlists", "fruit-company",
                    "fruit music menu: 2\n", playlists)
    unchanged = session(*common, args.output / "music", "fruit-company",
                        "", playlists)
    result = {key: mixed(frames, push=key == "push")
              for key, frames in shots.items()}
    result["split-push"] = split_push(split["split-push"])
    result["same-pane"] = split_push(unchanged["split-push"])
    (args.output / "result.json").write_text(json.dumps(result, indent=1) + "\n")
    failed = []
    for key in ("push", "pop"):
        got = result[key]
        if len(got["track"]) < 6 or got["worst_px"] > 2 or got["jitter_ms"] > 25:
            failed.append((key, "full list curve", got))
        if len(got["pane_errors"]) < 2 or max(got["pane_errors"]) > 7:
            failed.append((key, "pane visible behind full list", got))
        if len(got["row_errors"]) < 2 or max(got["row_errors"]) > 7:
            failed.append((key, "split rows follow full list", got))
    for key, changed in (("split-push", True), ("same-pane", False)):
        pane = result[key]
        if (pane["different"] >= 8) != changed or len(pane["frames"]) < 3:
            failed.append((key, "missing split-menu evidence", pane))
        if any(after > 3 for _, _, _, after in pane["frames"]):
            failed.append((key, "right pane must cut in place", pane))
    for item in failed:
        print("FAIL", *item)
    if failed:
        sys.exit(1)
    print("PASS Fruit Company split pages: full-list curve errors "
          f"{result['push']['worst_px']:.1f}/{result['pop']['worst_px']:.1f} px, "
          "both split pushes keep the right pane in place")


if __name__ == "__main__":
    main()
