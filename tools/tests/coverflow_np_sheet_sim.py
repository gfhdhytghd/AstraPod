#!/usr/bin/env python3
"""Check how Now Playing and Cover Flow give way to each other, in SDL.

For each theme with the 3D Cover Flow (Fruit Company, Classic, Scrim,
Light), every distinct frame the simulator presents is recorded, and:

  play      Play starts the centred album and raises Now Playing.
  flip-back MENU returns to the playing album with a cover turn.
  rise      Play pauses the current track and raises Now Playing even when
            another album is centred.
  sink      MENU sinks the page, revealing the same stationary covers.
  restart   With playback paused, Play starts the centred album and raises
            Now Playing.
  caption   The cover turn and the sink leave the cover visible through
            caption glyphs.

Arguments: ROCKBOXUI ZIP OUTPUT_DIRECTORY [--fixture DIRECTORY]
ZIP is the simulator build's rockbox.zip after the bundle steps. Writes
result.json and every recording's frames.
"""

import argparse
import json
import runpy
import shutil
import sys
import time
from pathlib import Path

import numpy as np
from PIL import Image

sys.dont_write_bytecode = True
from ui_stock_transitions_sim import CLASSIC, FPS, H, W, difference, \
    make_fixture, session  # noqa: E402

# A frame is a sheet when some offset explains its moving rows this well.
SHEET_ERROR = 2.5
# Glyph stems fill a few columns of a line edge to edge; a band fills many.
CAPTION_BOX = 12
STOCK_BAR = 17


def record(sim, frames, events):
    """Record every submitted frame, including repeats, for the event audit."""
    seen, got = sim.frames_since(0)
    shots = [(0.0, Image.frombytes("RGB", (W, H), got[-1]))]
    pending = sorted(events)
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
    return shots


def press(key):
    return [(0, "down " + key), (2 / FPS, "up " + key)]


def pixels(image):
    return np.asarray(image, dtype=np.float32)


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
            "offsets": offsets, "worst_error": round(max(
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
        if all(0 < offset < H and error < SHEET_ERROR
               for offset, error in before) and before[0][0] > before[1][0]:
            return shots[:i + 1], page
    return shots, shots[-1][1]


def turning_frames(shots):
    page, settled = shots[0][1], shots[-1][1]
    return sum(1 for _, image in shots[1:]
               if sheet_fit(image, page, settled)[1] > 3 * SHEET_ERROR)


def caption_style(settled):
    """The caption's background and ink colours in SETTLED, Cover Flow at
    rest, and the rows of each of its lines below mid-screen."""
    region = pixels(settled)[H // 2:]
    bg = pixels(settled)[H - 2, 2]
    distance = np.abs(region - bg).sum(axis=2)
    fg = region.reshape(-1, 3)[distance.argmax()]
    glyph = np.abs(region - fg).sum(axis=2) < 60
    lines = []
    for y in np.where(glyph[:, 20:W - 20].sum(axis=1) > 0)[0]:
        if lines and y <= lines[-1][1] + 1:
            lines[-1][1] = y
        else:
            lines.append([y, y])
    return bg, fg, glyph, lines


def boxed_columns(image, style):
    """Per caption line of STYLE that IMAGE shows where SETTLED does, the
    columns with a cover above and below the line's ink and nothing of it
    between the glyphs."""
    bg, fg, settled_glyph, lines = style
    region = pixels(image)[H // 2:]
    seg = fg - bg
    rel = region - bg
    t = np.clip((rel @ seg) / max(float(seg @ seg), 1.0), 0, 1)
    off_line = np.abs(rel - t[..., None] * seg).sum(axis=2) > 24
    glyph = np.abs(region - fg).sum(axis=2) < 60
    worst = 0
    for a, b in lines:
        ink = settled_glyph[a:b + 1]
        if (glyph[a:b + 1] & ink).sum() < 0.8 * ink.sum():
            continue
        cols = np.where(ink.any(axis=0))[0]
        boxed = 0
        for x in range(cols[0], cols[-1] + 1):
            above = off_line[max(0, a - 20):a, x].any()
            below = off_line[b + 1:b + 21, x].any()
            if above and below and not off_line[a:b + 1, x].any():
                boxed += 1
        worst = max(worst, boxed)
    return worst


def caption_box(image, settled):
    """Columns of a caption line drawn on an opaque band over a cover, more
    than SETTLED, the same caption at rest, shows."""
    style = caption_style(settled)
    return max(0, boxed_columns(image, style) - boxed_columns(settled, style))


def flow(enter):
    def script(sim):
        sim.wait(200)
        enter(sim)
        sim.wait(120)
        out = {}
        out["play"] = record(sim, 90, press("play"))
        sim.wait(50)
        out["flip-back"] = record(sim, 120, press("menu"))
        sim.wait(45)
        for _ in range(4):
            sim.tap("down", 16)
        sim.wait(55)
        out["rise"] = record(sim, 70, press("play"))
        sim.wait(30)
        out["sink"] = record(sim, 85, press("menu"))
        sim.wait(30)
        out["restart"] = record(sim, 80, press("play"))
        perf = sim.out / "disk/.rockbox/ui-perf.txt"
        if perf.exists():
            shutil.copyfile(perf, sim.out / perf.name)
        return out
    return script


def classic_enter(sim):
    sim.tap("select", 150)


def fruit_enter(sim):
    sim.tap("select", 40)
    sim.tap("select", 150)


THEMES = {
    "fruit-company": (fruit_enter, "", STOCK_BAR),
    "classic": (classic_enter, CLASSIC, STOCK_BAR),
    "scrim": (classic_enter, CLASSIC, 0),
    "light": (classic_enter, CLASSIC, 0),
}


def check(shots, top):
    # Repeated submissions matter to the raw timeline, while the geometry
    # checks count each distinct pose once.
    shots = {name: [shot for i, shot in enumerate(frames)
                    if i == 0 or frames[i - 1][1] != shot[1]]
             for name, frames in shots.items()}
    result = {}
    for name in ("play", "rise", "restart"):
        moving, page = playback_page(shots[name])
        result[name] = sheet_motion(moving, page, moving[0][1], True)
    result["sink"] = sheet_motion(shots["sink"], shots["sink"][0][1],
                                  shots["sink"][-1][1], False)
    result["flip_turning_frames"] = turning_frames(shots["flip-back"])
    result["sink_rest_change"] = round(difference(
        shots["rise"][0][1], shots["sink"][-1][1], (0, top, W, H)), 2)
    # The sheet's seam can cross a caption in mid-glyph. The layer fit
    # above already checks its exposed caption against the settled page.
    result["caption_box_columns"] = max(
        caption_box(image, shots["flip-back"][-1][1])
        for _, image in shots["flip-back"])
    result["restart_body_delta"] = round(difference(
        shots["rise"][-1][1], shots["restart"][-1][1], (0, top, W, H - 20)), 2)
    result["paused_body_delta"] = round(difference(
        shots["rise"][-1][1], shots["sink"][0][1], (0, top, W, H)), 2)
    failed = []
    for key in ("play", "rise", "restart", "sink"):
        motion = result[key]
        if (motion["frames"] < 2 or not motion["ordered"] or
                motion["worst_error"] > SHEET_ERROR or
                motion["max_gap_ms"] > 65):
            failed.append(key)
    if result["flip_turning_frames"] < 3:
        failed.append("flip_turning_frames")
    if result["sink_rest_change"] > 1.5:
        failed.append("sink_rest_change")
    if result["caption_box_columns"] > CAPTION_BOX:
        failed.append("caption_box_columns")
    if result["paused_body_delta"] > 0.5:
        failed.append("paused_body_delta")
    if result["restart_body_delta"] < 2:
        failed.append("restart_body_delta")
    return result, failed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("binary", "resource", "output"):
        parser.add_argument(name, type=lambda arg: Path(arg).resolve())
    parser.add_argument("--fixture", type=lambda arg: Path(arg).resolve())
    parser.add_argument("--themes", default=",".join(THEMES))
    parser.add_argument("--record-only", action="store_true")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    fixture = args.fixture or args.output / "fixture"
    if not args.fixture:
        shutil.rmtree(fixture, ignore_errors=True)
        make_fixture(args.binary, args.resource, fixture)
        capture = runpy.run_path(str(Path(__file__).resolve().parents[1] /
                                     "capture-readme-gifs.py"))
        config = fixture / ".rockbox/config.cfg"
        saved = config.read_bytes()
        try:
            capture["build_library_cache"](args.binary, fixture, args.output,
                                           tracks=24, albums=9)
        finally:
            config.write_bytes(saved)
        (fixture / ".rockbox/ui-perf.enable").touch()

    results, failures = {}, []
    for name in args.themes.split(","):
        enter, extra, top = THEMES[name]
        shots = session(args.binary, fixture, args.output / name, name,
                        extra + "scrolling enabled: off\n", flow(enter))
        if args.record_only:
            continue
        results[name], failed = check(shots, top)
        failures += [(name, key) for key in failed]
    if args.record_only:
        return
    (args.output / "result.json").write_text(json.dumps(results, indent=1) +
                                             "\n")
    for name, key in failures:
        print("FAIL", name, key, results[name].get(key, results[name]))
    if failures:
        sys.exit(1)
    print("PASS Now Playing rises, sinks and turns back as it should:",
          json.dumps(results))


if __name__ == "__main__":
    main()
