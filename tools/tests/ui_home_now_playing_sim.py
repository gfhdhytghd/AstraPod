#!/usr/bin/env python3
"""Measure Fruit Company's Home ↔ Now Playing transition on the SDL panel.

Press SELECT on Home's Now Playing row twice, returning with MENU each time.
Stock 2.0.5 QEMU supplies the timing. The stock iPod video supplies the
layer order: Now Playing crosses a fixed preview, pushing the menu rows
only while it overlaps their half. Play/Pause stays on Home.

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
            t_ms = ((clock - first) & 0xffffffff) / 1000
            writer.writerow([name, t_ms])
            frames.append((t_ms, image))
    return frames


def same(a, b):
    return ImageChops.difference(a, b).getbbox() is None


def check_slide(shots, entering, home):
    playback = shots[-1 if entering else 0][1]
    half = W // 2
    assert min(playback.getpixel((W - 1, H - 4))) > 220, "expected Now Playing"
    assert max(home.getpixel((W - 1, H - 4))) < 180, "expected a split Home menu"
    # The left side of the album art identifies the moving page without
    # depending on the track title, marquee or elapsed playback time.
    art = playback.crop((8, 75, 36, 105))
    poses = {}
    for t_ms, image in shots[1:-1]:
        for edge in range(18, W - 48):
            if not same(image.crop((edge + 8, 75, edge + 36, 105)), art):
                continue
            if not same(image.crop((edge, 0, edge + 48, 16)),
                        playback.crop((0, 0, 48, 16))):
                continue
            width = min(edge, half)
            if not same(image.crop((0, 0, width, 150)),
                        home.crop((half - width, 0, half, 150))):
                continue
            if edge > half and not same(image.crop((half, 35, edge, 150)),
                                        home.crop((half, 35, edge, 150))):
                continue
            poses.setdefault(edge, t_ms)
            break
    direction = "entry" if entering else "return"
    assert sum(edge < half for edge in poses) >= 3, (direction, poses)
    assert sum(edge > half for edge in poses) >= 3, (direction, poses)
    assert list(poses) == sorted(poses, reverse=entering), (direction, poses)
    first_edge, first_ms = next(iter(poses.items()))
    last_edge, last_ms = next(reversed(poses.items()))
    # Both 2.0.5 directions travel 320 px in about 275 ms. Estimate the
    # duration from the sampled movement to exclude input handling time.
    speed = (last_edge - first_edge) / (last_ms - first_ms)
    duration = W / abs(speed)
    assert 240 <= duration <= 330, (direction, duration, poses)
    # Allow one dropped panel transfer when checking constant speed.
    for edge, t_ms in poses.items():
        assert abs(edge - first_edge - speed * (t_ms - first_ms)) <= 24, (
            direction, edge, duration, poses)
    return round(duration)


def check_play_pause(paused, resumed):
    playing, pause = paused[0][1], paused[-1][1]
    for _, image in paused + resumed:
        assert same(image.crop((0, 20, W, H)), playing.crop((0, 20, W, H))), (
            "Play/Pause moved away from Home")
    icon = (104, 0, 127, 16)
    assert not same(playing.crop(icon), pause.crop(icon)), "missing pause icon"
    assert same(playing.crop(icon), resumed[-1][1].crop(icon)), (
        "missing playing icon")


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
            entry = record(sim, f"entry-{visit}", "select")
            returned = record(sim, f"exit-{visit}", "menu")
            home = returned[-1][1]
            results.append((check_slide(entry, True, home),
                            check_slide(returned, False, home)))
        paused = record(sim, "pause", "play", wait=84)
        resumed = record(sim, "resume", "play", wait=84)
        check_play_pause(paused, resumed)
    finally:
        sim.close()
        perf = rockbox / "ui-perf.txt"
        if perf.exists():
            shutil.copyfile(perf, out / perf.name)
        shutil.rmtree(disk)
    print(f"PASS Fruit Company Home ↔ Now Playing (ms): {results}; Play/Pause")


if __name__ == "__main__":
    main()
