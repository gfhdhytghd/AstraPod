#!/usr/bin/env python3
"""Record the Now Playing sheet going up from Cover Flow and back down.

The album track starts from the track list. PLAY controls the music from
Cover Flow, and SELECT+PLAY opens Now Playing while preserving transport.

Usage: coverflow_np_sheet_sim.py ROCKBOXUI ZIP OUTPUT [--fixture DIRECTORY]
Requires Linux, GDB, ffmpeg and a simulator with debug symbols.
"""

import argparse
import sys
from pathlib import Path

import numpy as np

sys.dont_write_bytecode = True

from coverflow_back_stack_sim import prepare_fixture
from fruit_wheel_transport_sim import Playback, wait_for
from ui_stock_transitions_sim import CLASSIC, H
from ultra_coverflow_sim import open_music, record
from ultra_sim import compare, perf_routes

SHEET_ERROR = 2.5


def sheet_fit(image, page, under):
    """Find the vertical seam using the full page, including its title."""
    f, p, u = (np.asarray(im, dtype=np.int16) for im in (image, page, under))
    # Sample columns to locate the seam cheaply. Verify the best fit using
    # all columns, so an isolated title, overlay or caption error counts.
    f_sample, p_sample, u_sample = (im[:, ::8] for im in (f, p, u))
    scores = [np.abs(f_sample[y:] - p_sample[:H-y]).sum()
              + np.abs(f_sample[:y] - u_sample[:y]).sum()
              for y in range(H + 1)]
    y = min((0, H, *range(1, H)), key=scores.__getitem__)
    error = (np.abs(f[y:] - p[:H-y]).sum()
             + np.abs(f[:y] - u[:y]).sum()) / f.size
    return y, error


def sheet_motion(shots, page, under, rising):
    track = [(round(ms), *sheet_fit(image, page, under)) for ms, image in shots]
    interior = [row for row in track if 0 < row[1] < H]
    offsets = [offset for _, offset, _ in interior]
    steps = [b - a for a, b in zip(offsets, offsets[1:])]
    begin, finish = (H, 0) if rising else (0, H)
    first = next((i for i, (_, y, _) in enumerate(track) if y != begin), 0)
    last = next((i for i in range(first, len(track))
                 if track[i][1] == finish), len(track) - 1)
    window = track[first:last + 1]
    return {"frames": len(interior), "ms": [ms for ms, _, _ in interior],
            "offsets": offsets, "motion_ms": window[-1][0] - window[0][0],
            "landed": window[-1][1] == finish,
            "worst_error": round(max(
                (e for _, _, e in window), default=255), 2),
            "max_gap_ms": max((b[0] - a[0] for a, b in
                               zip(window, window[1:])), default=0),
            "ordered": all(s < 0 for s in steps) if rising else
            all(s > 0 for s in steps)}


def playback_page(shots):
    """First frame whose top and body are at the settled Now Playing pose."""
    under = shots[0][1]
    for i in range(3, len(shots)):
        page = shots[i][1]
        before = [sheet_fit(image, page, under)
                  for _, image in shots[i - 2:i]]
        # A still-shifted page can explain the previous two poses as well.
        # The last pose before landing is within a few pixels of the top.
        if (all(0 < offset < H and error < SHEET_ERROR
                for offset, error in before) and
                before[0][0] > before[1][0] and before[1][0] <= 5):
            return shots[:i + 1], page
    return shots, shots[-1][1]


def turning_frames(shots):
    page, settled = shots[0][1], shots[-1][1]
    return sum(1 for _, image in shots[1:]
               if sheet_fit(image, page, settled)[1] > 3 * SHEET_ERROR)


def classic_enter(sim):
    sim.tap("select", 210)


def fruit_enter(sim):
    open_music(sim)
    classic_enter(sim)


THEMES = {
    "scrim": (classic_enter, CLASSIC, 0),
    "classic": (classic_enter, CLASSIC, 17),
    "fruit-company": (fruit_enter, "", 17),
    "fruit-company-ultra": (fruit_enter, "", 17),
}


def flow(sim, theme):
    playback = Playback(Path(sim.process.args[0]), sim.process.pid)
    try:
        covers = playback.activities["ACTIVITY_ALBUMCOVERS"]
        wps = playback.activities["ACTIVITY_WPS"]
        THEMES[theme][0](sim)
        wait_for(lambda: playback.activity() == covers, "Cover Flow did not open")
        sim.tap("select", 105)  # Open the album.
        sim.tap("select", 150)  # Play its first track.
        wait_for(lambda: playback.activity() == wps and playback.get("status") == 1,
                 "The album track did not start")
        sim.tap("menu", 100)
        wait_for(lambda: playback.activity() == covers, "MENU did not return to the album")
        sim.tap("menu", 100)    # Close the track list.
        sim.tap("down", 55)    # Keep the selected cover separate from playback.

        shots = {}
        for state, status in (("playing", 1), ("paused", 3)):
            if state == "paused":
                sim.tap("play", 24)
            wait_for(lambda: playback.get("status") == status, f"Playback is not {state}")
            track = playback.get("track")
            shots[state + "-rise"] = record(sim, 100, "select play")
            wait_for(lambda: playback.activity() == wps, "The chord did not open Now Playing")
            assert playback.get("status") == status and playback.get("track") == track
            shots[state + "-sink"] = record(sim, 100, "menu")
            wait_for(lambda: playback.activity() == covers, "MENU did not return to Cover Flow")
        return shots
    finally:
        playback.close()


def check_paths(folder):
    log = (folder / "ui-perf.txt").read_text().splitlines()
    paths = [dict(field.split("=", 1) for field in line.split()[1:])
             for line in log if line.startswith("cover-flow-wps-path ")]
    assert [(row["path"], row["result"], row["reason"]) for row in paths] == [
        ("agents-wps", "rise", "ready"),
        ("agents-wps", "rise", "ready"),
    ], paths
    sinks = perf_routes(folder, "cover-flow-wps-sink")
    assert len(sinks) == 2, sinks


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("binary", "resource", "output"):
        parser.add_argument(name, type=lambda arg: Path(arg).resolve())
    parser.add_argument("--fixture", type=lambda arg: Path(arg).resolve())
    parser.add_argument("--themes", default=",".join(THEMES))
    parser.add_argument("--record-only", action="store_true")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    fixture = args.fixture or prepare_fixture(args.binary, args.resource, args.output)

    for theme in args.themes.split(","):
        _, extra, _ = THEMES[theme]
        extra += "album covers view mode: 3d\nscrolling enabled: off\n"
        shots = compare(args.binary, fixture, args.output, extra, flow, themes=(theme,))
        if not args.record_only:
            check_paths(args.output / theme)
            for name, frames in shots[theme].items():
                poses = {image.crop((0, 24, 320, 220)).tobytes() for _, image in frames}
                assert len(poses) >= 5, (theme, name, len(poses))
        print(f"PASS {theme}: Cover Flow chords, sheet frames and playback state", flush=True)


if __name__ == "__main__":
    main()
