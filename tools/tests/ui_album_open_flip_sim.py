#!/usr/bin/env python3
"""Check that Select sends a turning album back before the live panel.

Arguments: ROCKBOXUI ZIP OUTPUT_DIRECTORY [--fixture DIRECTORY]
Sessions run serially with SDL dummy video.
"""

import argparse
import os
from pathlib import Path
from unittest.mock import patch

from ui_album_open_glyphs_sim import glyph_ink
from ui_stock_transitions_sim import difference, make_fixture, session


SORT = "album covers sort albums by: year descending\n"


def perf_marker(root):
    (root / ".rockbox/ui-perf.enable").touch()


def missing_panel_font(root):
    perf_marker(root)
    # Withhold the private panel font. The worker cannot publish a page
    # before the local readiness deadline, even on a fast host.
    (root / ".rockbox/fonts/14-noto-sans.pbf").unlink()


def music(sim):
    sim.tap("select", 30)


def covers(sim, wait=145):
    music(sim)
    sim.tap("select", wait)


def local(sim):
    music(sim)
    shots = {"entry": sim.record(158, "select", "select", 180)}
    sim.tap("menu", 82)
    shots["reopen"] = sim.record(135, "select")
    shots["reverse-close"] = sim.record(130, "menu", "select", 390)
    sim.tap("menu", 82)
    shots["scroll"] = sim.record(158, "down", "select", 80)
    sim.tap("select", 165)  # Start the selected track in Now Playing.
    sim.tap("menu", 100)    # Return to the open panel.
    sim.tap("menu", 90)     # Close it and return to covers.
    shots["after-wps"] = sim.record(135, "select")
    return shots


def settled(sim, frames=135):
    # Entry can spend two seconds waiting for the visible covers.
    covers(sim, wait=210)
    return {"rest": sim.record(frames, "select")}


def check_back(name, shots, animate):
    if name == "reverse-close":
        # Exclude the closing turn before the second key, at 390 ms.
        shots = [(ms, image) for ms, image in shots if ms >= 390]
    widths = [glyph_ink(image)[0] for _, image in shots]
    # The same blue header has over 240 columns at rest and fewer while
    # projected onto a turning face. Covers and menus have no such header.
    final = next((i for i, width in enumerate(widths) if width >= 240), None)
    assert final is not None, (name, "no live album panel", widths)
    tilted = sum(25 <= width < 240 for width in widths[:final])
    assert (tilted >= 2 if animate else tilted == 0), (
        name, "missing flip" if animate else "unexpected flip", widths)


def check_flat(shots):
    front = shots[0][1]
    back = shots[-1][1]
    # Flat draws a colored turning card. It draws text only after landing.
    # The center crop ignores captions, status and neighboring covers.
    crop = (125, 70, 195, 160)
    assert difference(front, back, crop) > 4, "no live flat album panel"
    assert sum(difference(frame, front, crop) > 4 and
               difference(frame, back, crop) > 4
               for _, frame in shots[1:-1]) >= 2, "missing flat flip"


def run(args, fixture, name, script, prepare=perf_marker, extra="",
        animate=True, reason="ready", flat=False, source="local"):
    output = args.output / name

    def capture(sim):
        shots = script(sim)
        perf = sim.root / ".rockbox/ui-perf.txt"
        if perf.exists():
            (output / perf.name).write_bytes(perf.read_bytes())
        return shots

    shots = session(args.binary, fixture, output, "fruit-company",
                    SORT + extra, capture, prepare)
    for path, frames in shots.items():
        if flat:
            check_flat(frames)
        else:
            check_back(path, frames, animate)
        if name == "remote":
            # The host sends its tracks after the loading face has landed.
            # The final selection band must contain the name of a track.
            assert glyph_ink(frames[-1][1])[1][2] > 100, "remote rows stayed blank"
    perf = (output / "ui-perf.txt").read_text()
    paths = [dict(field.split("=", 1) for field in line.split()[1:])
             for line in perf.splitlines() if line.startswith("cover-album-open ")]
    assert [entry["path"] for entry in paths] == list(shots), paths
    for entry in paths:
        expected = "reversed" if entry["path"] == "reverse-close" else reason
        assert entry["reason"] == expected, entry
        assert entry["source"] == source, entry
        assert entry["result"] == ("flip" if animate else "cut"), entry
        frames = int(entry["frames"])
        assert (frames >= 2 if animate else frames == 0), entry
        if name == "warm" or entry["path"] == "reopen":
            assert entry["cache"] == "hit", entry
        if name == "cold":
            assert entry["art"] == "placeholder" and entry["cache"] == "miss", entry
        if entry["path"] == "entry":
            assert entry["scene"] == "busy", entry
    if animate:
        traces = [line for line in perf.splitlines()
                  if line.startswith("route=cover-flip-open ")]
        assert len(traces) == len(shots), (name, traces)
    print(f"PASS {name}", paths, flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("binary", "resource", "output"):
        parser.add_argument(name, type=lambda path: Path(path).resolve())
    parser.add_argument("--fixture", type=lambda path: Path(path).resolve())
    parser.add_argument("--only",
                        choices=("cold", "local", "warm", "flat", "off", "remote"))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    fixture = args.fixture or args.output / "fixture"
    if not args.fixture:
        make_fixture(args.binary, args.resource, fixture)
    cases = {
        # Record past the four-second local wait and the turn itself.
        "cold": dict(script=lambda sim: settled(sim, 300),
                     prepare=missing_panel_font, reason="page-timeout"),
        "local": dict(script=local),
        "warm": dict(script=settled),
        "flat": dict(script=settled,
                     extra="album covers view mode: flat\n", flat=True),
        "off": dict(script=settled, extra="animations: off\n",
                    animate=False, reason="motion-off"),
    }
    for name, options in cases.items():
        if not args.only or name == args.only:
            run(args, fixture, name, **options)
    if args.only in (None, "remote"):
        with patch.dict(os.environ, PODBOX_SIM_CIDER="4500"):
            run(args, fixture, "remote", lambda sim: settled(sim, 265),
                reason="remote-loading", source="remote")


if __name__ == "__main__":
    main()
