#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Walk Fruit Company in SDL dummy video and verify physical transport keys.

Usage: fruit_wheel_transport_sim.py ROCKBOXUI ROCKBOX_ZIP OUTPUT_DIR
Requires Linux, GDB, ffmpeg and the simulator's debug symbols. The test reads
the child process's playlist, playback and activity state without stopping it.
LCD frames independently check the selected row and visible page.
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from zipfile import ZipFile

import numpy as np
from PIL import Image

from ui_music_fixture import build_tagcache, create_fixture, install_sim_codecs
from ui_stock_walk_sim import CONFIG, Sim


SCREEN_ACTIVITY = {
    "now-playing": "ACTIVITY_WPS",
    "cover-flow": "ACTIVITY_ALBUMCOVERS",
    "album-panel": "ACTIVITY_ALBUMCOVERS",
    "photo": "ACTIVITY_IMAGEVIEWER",
    "clocks": "ACTIVITY_TIMEDATESCREEN",
    "clock-sheet": "ACTIVITY_TIMEDATESCREEN",
    "agents": "ACTIVITY_AGENTS",
    "control-centre": "ACTIVITY_QUICKSCREEN",
}


class Playback:
    def __init__(self, binary, pid):
        expressions = {
            "status": "&'playback.c'::play_status",
            "track": "&'playlist.c'::current_playlist.index",
            "elapsed": "&'playback.c'::static_id3_entries[0].elapsed",
            "activity": "&'activity.c'::current_activity[0]",
            "depth": "&'activity.c'::current_activity_top",
        }
        activities = sorted(set(SCREEN_ACTIVITY.values()))
        args = ["gdb", "-nx", "-batch", str(binary)]
        for expression in (*expressions.values(), *activities):
            args += ["-ex", f"p/x (unsigned long)({expression})"]
        results = subprocess.check_output(args, text=True)
        offsets = re.findall(r"^\$\d+ = (0x[0-9a-f]+)$", results, re.M)
        if len(offsets) != len(expressions) + len(activities):
            raise RuntimeError(results)
        self.activities = dict(zip(activities, (int(x, 16) for x in
                                                offsets[len(expressions):])))
        maps = Path(f"/proc/{pid}/maps").read_text().splitlines()
        base = next(int(line.split("-", 1)[0], 16) for line in maps
                    if line.endswith(str(binary)) and line.split()[2] == "00000000")
        self.address = dict(zip(expressions, (base + int(x, 16) for x in offsets)))
        self.mem = os.open(f"/proc/{pid}/mem", os.O_RDONLY)

    def get(self, field, offset=0, size=4):
        value = os.pread(self.mem, size, self.address[field] + offset)
        assert len(value) == size, field
        return int.from_bytes(value, sys.byteorder)

    def activity(self):
        return self.get("activity", 4 * max(0, self.get("depth") - 1))

    def close(self):
        os.close(self.mem)


def wait_for(predicate, label, limit=4):
    until = time.monotonic() + limit
    while time.monotonic() < until:
        if predicate():
            return
        time.sleep(.025)
    raise AssertionError(label)


def frame(sim):
    sim.settle(quiet=.17, limit=.8)
    return np.frombuffer(sim.seen[-1][1], dtype=np.uint8).reshape(240, 320, 3)


def press(sim, key, hold=6):
    sim.key(key, hold=hold)
    sim.settle(quiet=.15, limit=.65)


def move(sim, amount):
    for _ in range(abs(amount)):
        press(sim, "down" if amount > 0 else "up")


def probe(sim, playing, name, hold_seek=False, region=(22, 240, 0, 320),
          expect_body_stable=True):
    sim.step = name
    activity = playing.activity()
    if name in SCREEN_ACTIVITY:
        assert activity == playing.activities[SCREEN_ACTIVITY[name]], (name, activity)
    original = frame(sim)
    Image.fromarray(original).save(Path(sim.log.name).parent / f"{name}.png")
    top, bot, left, right = region

    def key(key_name, hold=6):
        before = frame(sim)
        press(sim, key_name, hold)
        after = frame(sim)
        assert playing.activity() == activity, (name, key_name, "activity changed")
        if expect_body_stable:
            diff = np.any((after != before)[top:bot, left:right], axis=2)
            if diff.mean() > .012:
                out = Path(sim.log.name).parent
                Image.fromarray(before).save(out / f"{name}-{key_name}-before.png")
                Image.fromarray(after).save(out / f"{name}-{key_name}-after.png")
                raise AssertionError((name, key_name, "LCD content changed",
                                      float(diff.mean())))

    # Some viewers temporarily release the audio buffer while opening.
    if playing.get("status") == 0:
        key("play")
        wait_for(lambda: playing.get("status") == 1,
                 f"{name}: could not resume playlist")
    if playing.get("status") == 1:
        key("play")
    wait_for(lambda: playing.get("status") == 3, f"{name}: playback did not pause")
    index = playing.get("track")
    key("right")
    wait_for(lambda: playing.get("track") != index, f"{name}: no next track")
    key("back")
    wait_for(lambda: playing.get("track") == index, f"{name}: no previous track")
    if hold_seek:
        before = playing.get("elapsed", size=8)
        key("right", hold=60)
        wait_for(lambda: playing.get("elapsed", size=8) > before + 500,
                 f"{name}: no forward seek")
        before = playing.get("elapsed", size=8)
        key("back", hold=60)
        wait_for(lambda: playing.get("elapsed", size=8) < before - 500,
                 f"{name}: no backward seek")
        assert playing.get("track") == index, (name, "seek changed track")
        key("play", hold=60)
        assert playing.get("status") == 3, (name, "PLAY hold toggled playback")
    key("play")
    wait_for(lambda: playing.get("status") == 1, f"{name}: did not resume")
    key("play")
    wait_for(lambda: playing.get("status") == 3, f"{name}: did not pause")
    print(f"PASS {name}", flush=True)


def walk(sim, playback, sim_binary):
    # Main menu -> Shuffle Songs -> Now Playing, with a real queued playlist.
    out = Path(sim.log.name).parent
    Image.fromarray(frame(sim)).save(out / "start.png")
    move(sim, 6)
    press(sim, "select")
    Image.fromarray(frame(sim)).save(out / "shuffled.png")
    wait_for(lambda: playback.get("status") == 1, "Shuffle Songs did not start")
    press(sim, "play")
    wait_for(lambda: playback.get("status") == 3, "Now Playing did not pause")
    probe(sim, playback, "now-playing", hold_seek=True, expect_body_stable=False)
    press(sim, "menu")

    # Enter each branch with the wheel and SELECT. MENU unwinds one level.
    probe(sim, playback, "main-menu", hold_seek=True, region=(22, 240, 0, 142))
    move(sim, -6)
    press(sim, "select")
    probe(sim, playback, "music-menu", region=(22, 240, 0, 142))
    press(sim, "select")
    probe(sim, playback, "cover-flow", region=(190, 225, 70, 250))
    press(sim, "select")
    probe(sim, playback, "album-panel")
    press(sim, "menu")
    press(sim, "menu")
    press(sim, "menu")

    move(sim, 2)
    press(sim, "select")
    press(sim, "select")
    probe(sim, playback, "photos-list")
    press(sim, "select")
    probe(sim, playback, "photo")
    before = frame(sim)
    move(sim, 1)
    assert np.any(frame(sim)[50:190] != before[50:190]), "wheel did not page photos"
    press(sim, "menu")
    press(sim, "menu")
    press(sim, "menu")

    move(sim, 2)
    press(sim, "select")
    probe(sim, playback, "extras-menu", region=(22, 240, 0, 142))
    press(sim, "select")
    probe(sim, playback, "clocks")
    press(sim, "select")
    probe(sim, playback, "clock-sheet")
    press(sim, "menu")
    press(sim, "menu")
    # The standard SDL build has no USB PodLink service or Agents screen.
    symbols = subprocess.check_output(["nm", str(sim_binary)], text=True)
    if any(line.endswith(" agents_screen") for line in symbols.splitlines()):
        move(sim, 11)
        press(sim, "select")
        move(sim, 5)
        press(sim, "select")
        probe(sim, playback, "agents", region=(22, 215, 0, 320))
        press(sim, "menu")
        press(sim, "menu")
    else:
        print("SKIP agents (SDL build excludes USB PodLink)", flush=True)
    press(sim, "menu")

    move(sim, 1)
    press(sim, "select")
    probe(sim, playback, "settings", region=(22, 240, 0, 142))
    press(sim, "menu")
    press(sim, "menu", hold=45)
    assert playback.activity() == playback.activities["ACTIVITY_QUICKSCREEN"]
    # Control Centre is a direct four-tile modal, not a navigation page:
    # SELECT closes it cleanly back to the menu.
    press(sim, "select")
    assert playback.activity() != playback.activities["ACTIVITY_QUICKSCREEN"]
    print("PASS control-centre open and close", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for argument in ("binary", "zip", "output"):
        parser.add_argument(argument, type=lambda p: Path(p).resolve())
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="fruit-transport-") as temporary:
        root = Path(temporary) / "simdisk"
        create_fixture(root, args.zip, CONFIG + """
native theme: fruit-company
animations: off
repeat: all
qs top: keyclick repeats
qs bottom: hardware keyclick
""")
        with ZipFile(args.zip) as resources:
            resources.extract(".rockbox/themes/stock-art.bin", root)
        Image.new("RGB", (192, 192), (24, 180, 98)).save(root / "Images/Other.jpg")
        install_sim_codecs(args.binary, root)
        build_tagcache(args.binary, root, args.output / "tagcache.log")
        # A database build can leave its temporary startup config behind.
        (root / ".rockbox/config.cfg.new").unlink(missing_ok=True)
        sim = Sim(args.binary, root, args.output, "transport", "Fruit Company")
        try:
            wait_for(lambda: bool(sim.seen), "simulator did not present", limit=10)
            time.sleep(1.5)
            playback = Playback(args.binary, sim.process.pid)
            try:
                walk(sim, playback, args.binary)
            finally:
                playback.close()
        finally:
            sim.close()


if __name__ == "__main__":
    main()
