#!/usr/bin/env python3
"""Check Fruit Company's way into and out of Cover Flow against stock, in SDL.

Stock 2.0.5 enters Cover Flow from a split menu (Music, or the main menu
with Cover Flow turned on in Settings > Main Menu) like this, the time t
counted from the first frame that changes:

  - the covers stand still as the menu uncovers them;
  - the rows and their status bar slide left, and the art pane slides right,
    each by 320 * (t/270)^2 px, until both leave the screen at 191 ms;
  - Cover Flow's status bar drops in from above its own height,
    h * (1 - (1 - s/90)^2) rows showing s ms after it starts.

  - the caption shows as the bar starts, 355 ms after the rows.

Leaving, the bar and the caption go at once, and at the same moment the
rows start back from the left and the pane from the right, each
320 * (1 - t/270)^2 px from home, so neither shows until 79 ms in.

The same runs again against the simulator's remote library
(PODBOX_SIM_CIDER), whose Cover Flow comes from a host as it does on a
player connected to one.

Each session presses the key and records every distinct frame; each check
fits the start time and compares every frame's measured offset with the
model, within TOLERANCE px. A frame two refreshes late shifts a whole
curve, which the fitted start absorbs.

Arguments: ROCKBOXUI ZIP OUTPUT_DIRECTORY [--fixture DIRECTORY]
Writes result.json and every recording's frames.
"""

import argparse
import json
import os
import shutil
import struct
import sys
from pathlib import Path

import numpy

sys.dont_write_bytecode = True
from ui_stock_transitions_sim import (  # noqa: E402
    CF_ENTRY_CAPTURE_FRAMES, W, make_fixture, session,
)

from ui_music_fixture import MUSIC_ALBUMS

HALF = W // 2
SHADOW = 26
STOCK_FRAME_MS = 34.2
# Allow polling jitter without accepting a skipped 34 ms stock frame.
MAX_FRAME_MS = STOCK_FRAME_MS + 10
SLIDE_MS = 270.0
BAR_MS = 90.0
TOLERANCE = 12
# The column's right edge, where the list draws its scroll bar.
SCROLLBAR = 12
# Offsets past this leave too narrow a strip to measure.
WIDE = HALF - 40
# The covers before the caption and below the status bar.
COVERS = slice(40, 180)
# The caption, and the status bar, where the returning menu has not reached
# on the first frame of the way out.
CAPTION = (slice(190, 236), slice(60, 150))
TOP_BAR = (slice(0, 16), slice(60, 150))
# Stock shows the caption and starts the bar this long after the rows move.
CHROME_MS = 355
FRAME_MS = 34
# Caption text is darker than this; the covers' reflections are not.
INK = 80
# Fruit Company's main menu with Cover Flow turned on beside Music.
HOME_WITH_FLOW = "fruit main menu: 537948291\n"


def gray(image):
    return numpy.asarray(image.convert("L"), dtype=numpy.int16)


def column_offset(image, before, top=40, bottom=232):
    """How far left the menu's column has moved, 0..HALF; HALF when none of
    it is left. The column's right edge is left out: the list draws its
    scroll bar there once it is still."""
    now, then = gray(image), gray(before)
    best = (HALF, 1e9)
    for dx in range(HALF - 24):
        width = HALF - dx - SCROLLBAR
        error = numpy.abs(now[top:bottom, :width] -
                          then[top:bottom, dx:dx + width]).mean()
        if error < best[1]:
            best = (dx, error)
    return best[0] if best[1] < 2 else HALF


def pane_return(image, rest, top=40, bottom=232):
    """How far right of home the returning pane still is, 0..HALF."""
    now, then = gray(image), gray(rest)
    best = (HALF, 1e9)
    for dx in range(HALF - SHADOW - 8):
        left = HALF + SHADOW
        error = numpy.abs(now[top:bottom, left + dx:] -
                          then[top:bottom, left:W - dx]).mean()
        if error < best[1]:
            best = (dx, error)
    return best[0] if best[1] < 2 else HALF


def bar_rows(image, final, height):
    """How many rows of Cover Flow's status bar show, dropped from above."""
    now, then = gray(image), gray(final)
    for rows in range(height, 0, -1):
        if numpy.abs(now[:rows, HALF:] -
                     then[height - rows:height, HALF:]).mean() < 2:
            return rows
    return 0


def bar_height(final, rest):
    """The status bar's height: one past the last of the top 40 rows where
    Cover Flow at rest differs from the frame before the bar came."""
    now, then = gray(final), gray(rest)
    rows = [y for y in range(40)
            if numpy.abs(now[y, HALF:] - then[y, HALF:]).mean() >= 1]
    return rows[-1] + 1 if rows else 0


def fit(track, model):
    """The start time that best fits TRACK, (ms, px) pairs, to MODEL(t) for
    the frames inside the motion; returns (start, worst error)."""
    best = None
    first = round(track[0][0]) if track else 0
    for start10 in range(-3000, 3000):
        start = first + start10 / 10
        worst = max((abs(px - model(ms - start)) for ms, px in track),
                    default=0)
        if best is None or worst < best[1]:
            best = (start, worst)
    return best


def slide_out(t):
    return HALF if t >= SLIDE_MS else \
        min(HALF, round(2 * HALF * (max(0, t) / SLIDE_MS) ** 2))


def slide_back(t):
    return 0 if t >= SLIDE_MS else \
        min(HALF, round(2 * HALF * (1 - max(0, t) / SLIDE_MS) ** 2))


def measure_entry(shots):
    before = shots[0][1]
    final = shots[-1][1]
    moving = [(ms, column_offset(image, before)) for ms, image in shots]
    panes = [(ms, pane_return(image, before)) for ms, image in shots]
    first = next(ms for ms, dx in moving if dx > 0)
    track = [(ms - first, dx) for ms, dx in moving if 0 < dx < WIDE]
    pane = [(ms - first, dx) for ms, dx in panes if 0 < dx < WIDE]
    paired = [abs(rows - art) for (_, rows), (_, art) in zip(moving, panes)
              if 0 < rows < WIDE]
    start, worst = fit(track, slide_out)
    # The trackers lose their reference when just a sliver of menu is left.
    # Use the first frame whose whole cover area is clear.
    gone, rest = next((ms, image) for ms, image in shots
                      if numpy.abs(gray(image)[COVERS] -
                                   gray(final)[COVERS]).mean() < 0.25)
    height = bar_height(final, rest)
    bars = [(ms - first, bar_rows(image, final, height))
            for ms, image in shots if ms >= gone]
    arrived = [(ms, rows) for ms, rows in bars if 0 < rows < height]
    bar_start = None
    bar_worst = None
    if height:
        bar_start, bar_worst = fit(
            arrived, lambda s: 0 if s <= 0 else height if s >= BAR_MS else
            round(height * (1 - (1 - s / BAR_MS) ** 2)))
    caption_at = next(ms for ms, image in shots
                      if numpy.abs(gray(image)[CAPTION] -
                                   gray(final)[CAPTION]).mean() < 2)
    bar_first = next((ms for ms, rows in bars if rows), None)
    visible = [ms for ms, dx in moving if 0 < dx < HALF]
    frame_gaps = [round(b - a, 2) for a, b in zip(visible, visible[1:])]
    # The old viewport crop left the wall exposed before the translated seam.
    # Measure inside that gap, leaving out the moving seam's shadow.
    revealed = []
    for (ms, dx), (_, image) in zip(moving, shots):
        if 30 <= dx < WIDE:
            cols = slice(HALF - dx + SHADOW, HALF + dx - 2)
            revealed.append(float(numpy.abs(
                gray(image)[COVERS, cols] - gray(final)[COVERS, cols]).mean()))
    return {"track": track, "fit_start_ms": start, "worst_px": worst,
            "frame_gaps_ms": frame_gaps, "revealed": revealed,
            "gone_ms": round(gone - first), "pane": pane,
            "pane_gap_px": max(paired, default=HALF), "bar_height": height,
            "bar": arrived, "bar_start_ms": bar_start,
            "bar_worst_rows": bar_worst,
            "chrome_after_ms": None if bar_start is None
            else round(bar_start - start),
            "caption_ms": round(caption_at - first),
            "bar_first_ms": None if bar_first is None else round(bar_first)}


def measure_exit(shots):
    rest = shots[-1][1]
    rows = [(ms, column_offset(image, rest)) for ms, image in shots]
    panes = [(ms, pane_return(image, rest)) for ms, image in shots]
    first = shots[1][0]
    row_track = [(ms - first, dx) for ms, dx in rows if 0 < dx < WIDE]
    pane_track = [(ms - first, dx) for ms, dx in panes if 0 < dx < WIDE]
    row_start, row_worst = fit(row_track, slide_back)
    pane_start, pane_worst = fit(pane_track, slide_back)
    paired = [abs(row - pane) for (_, row), (_, pane) in zip(rows, panes)
              if 0 < row < WIDE and 0 < pane < WIDE]
    before, after = gray(shots[0][1]), gray(shots[1][1])
    chrome_gone = bool(after[TOP_BAR].std() < 2 and
                   numpy.abs(before[TOP_BAR] - after[TOP_BAR]).mean() >= 2 and
                   (after[CAPTION] < INK).sum() < 10 and
                   (before[CAPTION] < INK).sum() >= 50)
    return {"rows": row_track, "rows_fit_start_ms": row_start,
            "rows_worst_px": row_worst, "pane": pane_track,
            "pane_fit_start_ms": pane_start, "pane_worst_px": pane_worst,
            "pane_gap_px": max(paired, default=HALF),
            "chrome_gone_at_once": chrome_gone}


def from_music(sim):
    sim.wait(200)
    sim.tap("select", 90)
    enter = sim.record(CF_ENTRY_CAPTURE_FRAMES, "select")
    sim.wait(90)
    return {"enter": enter, "exit": sim.record(50, "menu")}


def from_home(sim):
    sim.wait(200)
    sim.tap("down", 60)
    enter = sim.record(CF_ENTRY_CAPTURE_FRAMES, "select")
    sim.wait(90)
    return {"enter": enter, "exit": sim.record(50, "menu")}


def install_preview_art(root):
    """
    Seed the row-major RGB565 WPS cache with distinctive 272 px test covers.
    This avoids depending on when the background worker scans the fixture.
    """
    target = root / ".rockbox/library/cache/art/wps"
    target.mkdir(parents=True, exist_ok=True)
    edge = 272
    y, x = numpy.indices((edge, edge))
    for index, (album, artist, _, _) in enumerate(MUSIC_ALBUMS[:6]):
        # art_name_album_hash(), for the ASCII and CJK names in this fixture.
        name = "/".join("".join(c for c in word.lower() if c.isalnum())
                        for word in (album, artist))
        key = 0x811C9DC5 ^ 0xA5A5A5A5
        for char in name:
            key = ((key ^ ord(char)) * 16777619) & 0xffffffff
        rgb565 = (((x // 5 + y // 19) % 32) << 11 |
                  ((y // 4 + x // 23) % 64) << 5 | (7 + 3 * index))
        header = struct.pack("<6H", 0x5441, 8, edge, edge, 0, 0)
        (target / f"{key:08x}.aat").write_bytes(
            header + rgb565.astype("<u2").tobytes())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("binary", "resource", "output"):
        parser.add_argument(name, type=lambda arg: Path(arg).resolve())
    parser.add_argument("--fixture", type=lambda arg: Path(arg).resolve())
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    fixture = args.fixture or args.output / "fixture"
    if not args.fixture:
        shutil.rmtree(fixture, ignore_errors=True)
        make_fixture(args.binary, args.resource, fixture)

    result = {}
    for name, extra, script, remote in (
            ("music", "", from_music, False),
            ("home", HOME_WITH_FLOW, from_home, False),
            ("remote-music", "", from_music, True),
            ("remote-home", HOME_WITH_FLOW, from_home, True),
            ("slideshow", "", from_music, False)):
        if remote:
            os.environ["PODBOX_SIM_CIDER"] = "60000"
        try:
            shots = session(args.binary, fixture, args.output / name,
                            "fruit-company", extra, script,
                            prepare=install_preview_art
                            if name == "slideshow" else None)
        finally:
            os.environ.pop("PODBOX_SIM_CIDER", None)
        result[name] = {"enter": measure_entry(shots["enter"]),
                        "exit": measure_exit(shots["exit"])}
    (args.output / "result.json").write_text(json.dumps(result, indent=1) +
                                             "\n")

    failed = []
    for name, got in result.items():
        enter, leave = got["enter"], got["exit"]
        if len(enter["track"]) < 3 or enter["worst_px"] > TOLERANCE:
            failed.append((name, "rows leave", enter["worst_px"],
                           enter["track"]))
        gaps = enter["frame_gaps_ms"]
        if len(gaps) < 3 or max(gaps) > MAX_FRAME_MS:
            failed.append((name, "entry frame interval", gaps))
        if name == "slideshow" and (len(enter["revealed"]) < 2 or
                                     min(enter["revealed"]) < 12):
            failed.append((name, "hidden square becomes visible",
                           enter["revealed"]))
        if len(enter["pane"]) < 3 or enter["pane_gap_px"] > 3:
            failed.append((name, "pane leaves with rows", enter["pane_gap_px"],
                           enter["pane"]))
        if not enter["bar"] or enter["bar_worst_rows"] > 4:
            failed.append((name, "bar drops", enter["bar"]))
        elif abs(enter["chrome_after_ms"] - CHROME_MS) > FRAME_MS:
            failed.append((name, "bar starts", enter["chrome_after_ms"]))
        if enter["bar_start_ms"] is None or \
                abs(enter["caption_ms"] - enter["bar_start_ms"]) > FRAME_MS:
            failed.append((name, "caption shows with the bar",
                           enter["caption_ms"], enter["bar_start_ms"]))
        if not leave["chrome_gone_at_once"]:
            failed.append((name, "bar and caption go at once"))
        if leave["pane_gap_px"] > 3:
            failed.append((name, "pane returns with rows", leave["pane_gap_px"]))
        for part in ("rows", "pane"):
            if len(leave[part]) < 3 or leave[part + "_worst_px"] > TOLERANCE:
                failed.append((name, part + " return",
                               leave[part + "_worst_px"], leave[part]))
            elif abs(leave[part + "_fit_start_ms"]) > FRAME_MS:
                failed.append((name, part + " return with the chrome",
                               leave[part + "_fit_start_ms"]))
    for failure in failed:
        print("FAIL", *failure)
    if failed:
        sys.exit(1)
    print("PASS Fruit Company Cover Flow entry and exit follow stock: " +
          ", ".join(f"{name} rows {got['enter']['worst_px']} px, "
                    f"pane gap {got['enter']['pane_gap_px']} px, "
                    f"bar {got['enter']['bar_worst_rows']} rows "
                    f"{got['enter']['chrome_after_ms']} ms in, "
                    f"return {got['exit']['rows_worst_px']}/"
                    f"{got['exit']['pane_worst_px']} px"
                    for name, got in result.items()))


if __name__ == "__main__":
    main()
