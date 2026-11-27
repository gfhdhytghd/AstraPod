#!/usr/bin/env python3
"""Measure Fruit Company's Home ↔ Now Playing transition on the SDL panel.

Press SELECT on Home's Now Playing row twice, returning with MENU each time.
The stock return moves Home's preview across the left half before it starts
pushing Now Playing right. Save each transfer and its timestamp for inspection.

Arguments: ROCKBOXUI ZIP OUTPUT_DIRECTORY [--fixture DIRECTORY]
"""

import argparse
import csv
from pathlib import Path
import shutil
import sys

from PIL import Image, ImageChops

sys.dont_write_bytecode = True
from ui_list_hwscroll_sim import RECORD, RECORD_SIZE
from ui_stock_transitions_sim import H, W, Sim, make_fixture


def record(sim, name, key, wait=48):
    panel = sim.out / "panel.rec"
    start = panel.stat().st_size // RECORD_SIZE - 1
    assert start >= 0, "the panel has not drawn Home"
    sim.tap(key, wait)

    folder = sim.out / name
    folder.mkdir()
    frames = []
    with panel.open("rb") as raw, (folder / "frames.csv").open("w") as table:
        raw.seek(start * RECORD_SIZE)
        writer = csv.writer(table)
        writer.writerow(["file", "t_ms"])
        for index in range(panel.stat().st_size // RECORD_SIZE - start):
            header = raw.read(RECORD.size)
            magic, clock, *_ = RECORD.unpack(header)
            assert magic == b"PNL1"
            if index == 0:
                first = clock
            pixels = raw.read(RECORD_SIZE - RECORD.size)
            image = Image.frombytes("RGB", (W, H), pixels, "raw", "BGR;16")
            name = f"f{index:03d}.png"
            image.save(folder / name)
            writer.writerow([name, ((clock - first) & 0xffffffff) / 1000])
            frames.append(image)
    return frames


def same(a, b):
    return ImageChops.difference(a, b).getbbox() is None


def check_return(shots):
    playback, home = shots[0], shots[-1]
    # The right quarter of Home's preview is dark at the bottom. The white
    # playback page immediately after it locates the moving edge.
    y = H - 4
    assert min(playback.getpixel((W - 1, y))) > 220, "expected Now Playing"
    assert max(home.getpixel((W - 1, y))) < 180, "expected a split Home menu"
    early, late = set(), set()
    for image in shots[1:-1]:
        edge = max((x + 1 for x in range(W)
                    if max(image.getpixel((x, y))) < 180), default=0)
        if not 24 <= edge <= W - 48:
            continue
        # Match the translated preview, including the icon or cover. Match
        # the playback page separately, excluding the progress and title
        # marquee, which can update on the last frame before MENU.
        pane = min(edge, W // 2)
        if not same(image.crop((edge - pane, 35, edge, 150)),
                    home.crop((W - pane, 35, W, 150))):
            continue
        shift = max(0, edge - W // 2)
        if same(image.crop((edge, 87, W, 128)),
                playback.crop((edge - shift, 87, W - shift, 128))):
            (early if edge < W // 2 else late).add(edge)
    return len(early), len(late)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("binary", "resource", "output"):
        parser.add_argument(name, type=lambda value: Path(value).resolve())
    parser.add_argument("--fixture", type=lambda value: Path(value).resolve())
    args = parser.parse_args()
    out = args.output
    out.mkdir(parents=True, exist_ok=True)
    fixture = args.fixture or out / "fixture"
    if not args.fixture:
        make_fixture(args.binary, args.resource, fixture)
    disk = out / "disk"
    shutil.copytree(fixture, disk)
    rockbox = disk / ".rockbox"
    for name in ("config.cfg.new", ".resume.cfg.new", ".resume.cfg"):
        (rockbox / name).unlink(missing_ok=True)
    config = rockbox / "config.cfg"
    lines = [line for line in config.read_text().splitlines()
             if not line.startswith(("native theme:", "fruit main menu:"))]
    config.write_text("\n".join(lines) + "\nnative theme: fruit-company\n"
                      "idle poweroff: off\nsleep timer on startup: off\n")
    (rockbox / "ui-perf.enable").touch()
    sim = Sim(args.binary, disk, out,
              {"PODBOX_PANEL_RECORD": str(out / "panel.rec")})
    results = []
    try:
        sim.wait(360)
        for _ in range(6):
            sim.tap("down", 15)
        sim.tap("select", 120)  # Play the fixture via Shuffle Songs.
        sim.tap("menu", 45)
        sim.tap("down", 30)  # Home's newly visible Now Playing row.
        for visit in range(2):
            record(sim, f"entry-{visit}", "select")
            results.append(check_return(record(sim, f"exit-{visit}", "menu")))
        record(sim, "play", "play")
    finally:
        sim.close()
        perf = rockbox / "ui-perf.txt"
        if perf.exists():
            shutil.copyfile(perf, out / perf.name)
        shutil.rmtree(disk)
    assert all(early >= 2 and late >= 2 for early, late in results), (
        f"expected a stationary then pushed playback page, "
        f"got early/late matches {results}")
    print(f"PASS Fruit Company Home ← Now Playing: early/late matches {results}")


if __name__ == "__main__":
    main()
