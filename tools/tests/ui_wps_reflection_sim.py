#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Record Fruit Company's album panel turn to Now Playing with SDL dummy.

Run a cold local start with delayed art and glyphs, then return to the album
and repeat with the track already playing and its bitmap ready.
Arguments: ROCKBOXUI ZIP OUTPUT_DIRECTORY [--fixture DIRECTORY]
"""

import argparse
import json
import mmap
import runpy
import shutil
import struct
import subprocess
import sys
from pathlib import Path

from PIL import Image, ImageChops, ImageStat

sys.dont_write_bytecode = True
from ui_first_run_sim import read_perf
from ui_stock_transitions_sim import Sim, make_fixture

# The cover ends at y=168. Progress/elapsed start at y=203.
REFLECTION = (9, 169, 145, 203)
TRACK_INDEX = (159, 144, 315, 161)
TITLE_BAR = (30, 0, 290, 17)
PANEL_HEADER = struct.Struct("<4sIcBhhhI32h")
PANEL_BYTES = 320 * 240 * 2
PANEL_FRAME = PANEL_HEADER.size + PANEL_BYTES


def changed(a, b):
    diff = ImageChops.difference(a, b)
    red, green, blue = diff.split()
    return a.width * a.height - ImageChops.lighter(
        ImageChops.lighter(red, green), blue).histogram()[0]


def rgb565(image):
    # PIL truncates expanded RGB565 channels while SDL rounds them.
    return image.point([n >> shift for shift in (3, 2, 3) for n in range(256)])


def check(shots, record, first, last):
    # The title bar and track index identify the end of the chrome fade.
    # Together they distinguish full opacity from a quantized blend.
    index = shots[-1][1].crop(TRACK_INDEX)
    title = shots[-1][1].crop(TITLE_BAR)
    assert changed(index, Image.new("RGB", index.size, "white")) > 30
    landing = next(i for i, (_, frame) in enumerate(shots)
                   if frame.crop(TRACK_INDEX).tobytes() == index.tobytes()
                   and frame.crop(TITLE_BAR).tobytes() == title.tobytes())
    span = shots[-1][0] - shots[landing][0]
    assert landing >= 3 and span >= 1000, (landing, span)
    window = shots[landing - 3:]
    reflection = [frame.crop(REFLECTION) for _, frame in window]
    settled = reflection[-1]
    white = Image.new("RGB", settled.size, "white")
    index565 = rgb565(index).tobytes()
    settled565 = rgb565(settled)

    # Direct carousel LCD writes appear in the SDL frame ring. Retained-page
    # updates also appear in the panel record, including repeated submits.
    with record.open("rb") as handle, mmap.mmap(
            handle.fileno(), 0, access=mmap.ACCESS_READ) as data:
        panel = []
        for i in range(first, last):
            pos = i * PANEL_FRAME
            magic = PANEL_HEADER.unpack_from(data, pos)[0]
            assert magic == b"PNL1", magic
            start = pos + PANEL_HEADER.size
            frame = Image.frombytes("RGB", (320, 240),
                                    data[start:start + PANEL_BYTES],
                                    "raw", "BGR;16")
            if rgb565(frame.crop(TRACK_INDEX)).tobytes() == index565:
                panel.append(changed(rgb565(frame.crop(REFLECTION)), settled565))
    assert len(panel) >= 4
    changes = [changed(im, settled) for im in reflection[3:]]
    return {
        "landing_ms": round(shots[landing][0]),
        "span_ms": round(span),
        "last_turn_and_landing_ink": [changed(im, white) for im in reflection[:4]],
        "last_turn_and_landing_error": [
            round(sum(ImageStat.Stat(ImageChops.difference(im, settled)).mean)
                  / 3, 2) for im in reflection[:4]],
        "changed_pixels_at_landing": changes[0],
        "max_changed_pixels_after_landing": max(changes[1:]),
        "wps_frames_after_landing": len(changes) - 1,
        "settled_cover_rgb": shots[-1][1].getpixel((32, 145)),
        "panel_submissions": len(panel),
        "max_panel_changed_pixels": max(panel),
    }, window


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("binary", "resource", "output"):
        parser.add_argument(name, type=lambda path: Path(path).resolve())
    parser.add_argument("--fixture", type=lambda path: Path(path).resolve())
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    fixture = args.fixture or args.output / "fixture"
    if args.fixture is None and not fixture.exists():
        make_fixture(args.binary, args.resource, fixture)
        cache = runpy.run_path(str(Path(__file__).resolve().parents[1] /
                                   "capture-readme-gifs.py"))
        config = fixture / ".rockbox/config.cfg"
        saved = config.read_bytes()
        try:
            cache["build_library_cache"](args.binary, fixture, args.output,
                                         tracks=24, albums=9)
        finally:
            config.write_bytes(saved)

    root = args.output / "disk"
    shutil.rmtree(root, ignore_errors=True)
    shutil.copytree(fixture, root)
    rockbox = root / ".rockbox"
    config = rockbox / "config.cfg"
    for stale in ("config.cfg.new", "config.cfg.old", ".resume.cfg.new"):
        (rockbox / stale).unlink(missing_ok=True)
    lines = [line for line in config.read_text().splitlines()
             if not line.startswith("native theme:")]
    config.write_text("\n".join(lines) + "\nnative theme: fruit-company\n"
                      "album covers sort albums by: year descending\n"
                      "album art: prefer image file\nscrolling enabled: off\n"
                      "idle poweroff: off\ntime in title: off\nrepeat: one\n")
    (rockbox / "ui-perf.enable").touch()
    perf = rockbox / "ui-perf.txt"
    perf.unlink(missing_ok=True)
    shim = args.output / "cold-storage.so"
    subprocess.run(["cc", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC",
                    str(Path(__file__).with_name("ui_cold_storage.c")),
                    "-o", str(shim), "-ldl", "-pthread"], check=True)

    record = args.output / "panel.rec"
    sim = Sim(args.binary, root, args.output, {
        "LD_PRELOAD": str(shim), "PODBOX_COLD_READ_MS": "1400",
        "PODBOX_COLD_READ_ROOT": str(root), "PODBOX_COLD_READ_SUFFIX": ".bmp",
        "PODBOX_COLD_READ_COOPERATIVE": "1",
        "PODBOX_GLYPH_DELAY_MS": "400", "PODBOX_PANEL_RECORD": str(record),
    })
    captures = {}
    try:
        sim.wait(150)
        sim.tap("select", 50)   # Home → Music
        sim.tap("select", 210)  # Music → Cover Flow
        sim.tap("select", 135)  # Cover Flow → album panel
        # A track-specific image differs from the cached album thumbnail.
        # Add it after entering the panel so only playback reads it.
        media = root / "Music/Library/Collection/2026"
        track, = media.rglob("01 *.mp3")
        Image.new("RGB", (192, 192), (230, 25, 130)).save(
            track.with_suffix(".bmp"))
        for name in ("cold", "already-playing"):
            first = record.stat().st_size // PANEL_FRAME
            shots = sim.record(240, "select")
            last = record.stat().st_size // PANEL_FRAME
            captures[name] = (shots, first, last)
            if name == "cold":
                sim.tap("menu", 100)  # Now Playing → same album panel
    finally:
        sim.close()

    assert "cold storage: delaying first .bmp read" in (
        args.output / "firmware.log").read_text()
    routes = read_perf(perf)
    shutil.copyfile(perf, args.output / perf.name)
    assert [r["first_run"] for r in routes["cover-panel-wps"]] == ["1", "0"]
    assert "wps-cover-panel" in routes
    setups = [dict(part.split("=", 1) for part in line.split()[1:])
              for line in perf.read_text().splitlines()
              if line.startswith("cover-wps route=C ")]
    assert [s["bitmap"] for s in setups] == ["not-painted", "ready"], setups
    results = {}
    for name, (shots, first, last) in captures.items():
        results[name], window = check(shots, record, first, last)
        folder = args.output / name
        folder.mkdir(exist_ok=True)
        for i, (ms, frame) in enumerate(window):
            frame.save(folder / f"{i:03d}-{ms:06.1f}.png")
    (args.output / "result.json").write_text(json.dumps(results, indent=2) + "\n")
    for name, result in results.items():
        assert min(result["last_turn_and_landing_ink"]) > 100, (name, result)
        assert result["changed_pixels_at_landing"] == 0, (name, result)
        assert result["max_changed_pixels_after_landing"] == 0, (name, result)
        assert result["max_panel_changed_pixels"] == 0, (name, result)
    r, g, b = results["already-playing"]["settled_cover_rgb"]
    assert r > 200 and g < 50 and b > 100, results
    assert (results["cold"]["settled_cover_rgb"] !=
            results["already-playing"]["settled_cover_rgb"]), results
    print("PASS reflection is continuous at the turn and stable after landing",
          results)


if __name__ == "__main__":
    main()
