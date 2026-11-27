#!/usr/bin/env python3
"""Check Fruit Company's split-menu page slides in the headless simulator.

Stock 2.0.5's full list crosses the screen in 270 ms. The list on the
split page moves only while the full list overlaps its half. The full
list progressively covers or reveals the pane at the same time.
"""

import argparse
import json
import sys
from pathlib import Path

import numpy

sys.dont_write_bytecode = True
from ui_fruit_flow_entry_sim import HALF, SLIDE_MS, gray
from ui_stock_transitions_sim import W, make_fixture, session

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
    return {"split-push": sim.record(55, "select")}


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


def split_push(shots):
    """Track the seam between two different panes against the list seam."""
    before, after = (gray(im)[24:170] for _, im in (shots[0], shots[-1]))
    track = []
    if error(before[:, HALF:], after[:, HALF:]) < 8:
        return track
    for ms, image in shots:
        now = gray(image)[24:170]
        row_error, seam = min(
            (error(now[:, x + 8:HALF], after[:, 8:HALF - x]), x)
            for x in range(HALF - 45)
        )
        if row_error > 3 or seam < 12:
            continue
        pane_error, pane_seam = min(
            (error(now[:, HALF + x:], after[:, HALF:W - x]), x)
            for x in range(HALF - 28)
        )
        track.append((ms, seam, pane_seam, pane_error))
    return track


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("binary", "resource", "output"):
        parser.add_argument(name, type=lambda arg: Path(arg).resolve())
    parser.add_argument("--fixture", type=lambda arg: Path(arg).resolve())
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    fixture = args.fixture or args.output / "fixture"
    if not args.fixture:
        make_fixture(args.binary, args.resource, fixture)
    common = (args.binary, fixture)
    shots = session(*common, args.output / "albums", "fruit-company", "", albums)
    # Make Playlists the first Music row so the incoming pane differs from
    # the Music preview on Home.
    split = session(*common, args.output / "playlists", "fruit-company",
                    "fruit music menu: 2\n", playlists)
    result = {key: mixed(frames, push=key == "push")
              for key, frames in shots.items()}
    changing = result["split-push"] = split_push(split["split-push"])
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
    if len(changing) < 3 or any(abs(row - pane) > 2 or diff > 3
                                for _, row, pane, diff in changing):
        failed.append(("split-push", "the new pane travels with the rows", changing))
    for item in failed:
        print("FAIL", *item)
    if failed:
        sys.exit(1)
    print("PASS Fruit Company split pages: full-list curve errors "
          f"{result['push']['worst_px']:.1f}/{result['pop']['worst_px']:.1f} px, "
          "split-push pane moves with the rows")


if __name__ == "__main__":
    main()
