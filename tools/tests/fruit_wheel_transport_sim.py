#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Walk Fruit Company in SDL dummy video and verify transport and Now Playing chords.

Usage: fruit_wheel_transport_sim.py ROCKBOXUI ROCKBOX_ZIP OUTPUT_DIR
Requires Linux, GDB, ffmpeg and the simulator's debug symbols. The test reads
the child process's playlist, playback and activity state without stopping it.
LCD frames independently check the selected row and visible page.
"""

import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from zipfile import ZipFile

import numpy as np
from PIL import Image

from ui_music_fixture import build_tagcache, create_fixture, install_sim_codecs
from ui_quickscreen_sim import ULTRA_FALLBACK_THEME
from ui_stock_walk_sim import CONFIG, Sim


CHORD_THEMES = ("fruit-company", "fruit-company-ultra", "classic", "scrim")
SCREEN_ACTIVITY = {
    "main-menu": "ACTIVITY_MAINMENU",
    "files": "ACTIVITY_FILEBROWSER",
    "now-playing": "ACTIVITY_WPS",
    "lyrics": "ACTIVITY_LYRICS",
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
            "cover": "&center_index",
            "cover_visible": "&'carousel.c'::pf_shown",
            "cover_state": "&'carousel.c'::pf_state",
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
        if name == "now-playing":
            lyrics = playing.activities["ACTIVITY_LYRICS"]
            press(sim, "play", hold=66)
            wait_for(lambda: playing.activity() == lyrics, "PLAY did not open Lyrics")
            assert playing.get("status") == 3, "Opening Lyrics resumed playback"
            press(sim, "play", hold=66)
            wait_for(lambda: playing.activity() == activity, "PLAY did not return")
            assert playing.get("status") == 3, "Closing Lyrics resumed playback"
        else:
            key("play", hold=60)
            assert playing.get("status") == 3, (name, "PLAY hold toggled playback")
    key("play")
    wait_for(lambda: playing.get("status") == 1, f"{name}: did not resume")
    key("play")
    wait_for(lambda: playing.get("status") == 3, f"{name}: did not pause")
    print(f"PASS {name}", flush=True)


def probe_paging(sim, playback, name, region=np.s_[35:220, :]):
    """Both arrows move the UI while PLAY controls the same playlist."""
    out = Path(sim.log.name).parent
    before = frame(sim)
    Image.fromarray(before).save(out / f"{name}-start.png")
    track = playback.get("track")
    elapsed = playback.get("elapsed", size=8)
    center = playback.get("cover") if name == "cover-flow" else None
    assert playback.get("status") == 3, f"{name} needs paused playback"
    for key in ("right", "back"):
        first = len(sim.seen)
        press(sim, key, hold=10)
        if center is not None:
            expected = center + (key == "right")
            wait_for(lambda: playback.get("cover") == expected and
                     playback.get("cover_state") == 0,
                     f"{name} {key} did not land exactly one album away")
        after = frame(sim)
        Image.fromarray(after).save(out / f"{name}-{key}.png")
        assert np.any(before[region] != after[region]), (name, key, "no paging")
        assert playback.get("track") == track, (name, key, "skipped track")
        assert playback.get("elapsed", size=8) == elapsed, (name, key, "sought")
        assert playback.get("status") == 3, (name, key, "changed playback")
        if name == "slideshow":
            for _, raw in sim.seen[first:]:
                picture = np.frombuffer(raw, np.uint8).reshape(240, 320, 3)
                if picture[100, 100, 0] > 15 and picture[100, 100, 1] > 15:
                    Image.fromarray(picture).save(out / f"{name}-{key}-fade.png")
                    break
            else:
                raise AssertionError((name, key, "configured fade did not run"))
        before = after

    press(sim, "play")
    wait_for(lambda: playback.get("status") == 1, f"{name} PLAY did not resume")
    press(sim, "play")
    wait_for(lambda: playback.get("status") == 3, f"{name} PLAY did not pause")
    assert playback.get("track") == track, (name, "PLAY changed the track")
    assert np.array_equal(before[region], frame(sim)[region]), (
        name, "PLAY changed the page")
    print(f"PASS {name} paging and playback", flush=True)


def create_indexed_photos(root):
    """Synthetic indexed photos, with enough entries to page the grid."""
    def record(tag, fields=(), body=b"", count=None):
        header = 12 + len(fields) * 4
        return struct.pack("<4sII" + "I" * len(fields), tag, header,
                           header + len(body) if count is None else count,
                           *fields) + body

    colors = (0xf800, 0x07e0, 0x001f, 0xffe0) * 4
    photos = root / "Photos"
    thumbs = photos / "Thumbs"
    thumbs.mkdir(parents=True, exist_ok=True)
    formats = ((1066, 64 * 64), (1024, 320 * 240))
    for fmt, pixels in formats:
        data = b"".join(struct.pack("<H", c) * pixels for c in colors)
        (thumbs / f"F{fmt}_1.ithmb").write_bytes(data)

    images = []
    for i in range(len(colors)):
        records = []
        for fmt, pixels in formats:
            path = f":Thumbs:F{fmt}_1.ithmb".encode()
            name = record(b"mhod", body=struct.pack("<III", len(path), 0, 0)
                          + path)
            size = pixels * 2
            thumb = record(b"mhni", (1, fmt, i * size, size, 0, 0), name)
            records.append(record(b"mhod", (2,), thumb))
        images.append(record(b"mhii", (len(records), i + 1), b"".join(records)))
    album = record(b"mhba", (0, 0, 0, 0, 1 << 16))
    sections = [
        record(b"mhsd", (1,), record(b"mhli", body=b"".join(images),
                                     count=len(images))),
        record(b"mhsd", (2,), record(b"mhla", body=album, count=1)),
    ]
    (photos / "Photo Database").write_bytes(record(b"mhfd", body=b"".join(sections)))


def start_paused(sim, playback):
    move(sim, 6)
    press(sim, "select", hold=10)
    wps = playback.activities["ACTIVITY_WPS"]
    wait_for(lambda: playback.get("status") == 1 and playback.activity() == wps,
             "Shuffle Songs did not enter Now Playing")
    sim.settle(quiet=.60, limit=2)
    press(sim, "play", hold=12)
    try:
        wait_for(lambda: playback.get("status") == 3,
                 "Now Playing did not pause", limit=1.2)
    except AssertionError:
        # Startup can carry SELECT into the first WPS poll. Only retry the
        # fixture setup; every paging and PLAY assertion uses a single key.
        assert playback.get("status") == 1 and playback.activity() == wps
        press(sim, "play", hold=12)
        wait_for(lambda: playback.get("status") == 3, "Now Playing did not pause")


def indexed_photos(sim, playback):
    start_paused(sim, playback)
    press(sim, "menu", hold=10)
    menu = playback.activities["ACTIVITY_MAINMENU"]
    wait_for(lambda: playback.activity() == menu, "Photos fixture did not reach Home")
    sim.settle(quiet=.4, limit=1.5)
    # With wrap disabled, reset to the first row. Now Playing can return
    # to either Music or Shuffle Songs depending on startup timing.
    move(sim, -8)
    move(sim, 2)
    press(sim, "select", hold=10)  # Photos
    press(sim, "select", hold=10)  # All Photos, indexed grid
    probe_paging(sim, playback, "photo-grid")
    press(sim, "select")  # Full-screen still
    probe_paging(sim, playback, "indexed-photo")
    press(sim, "select")  # Manual slideshow
    probe_paging(sim, playback, "slideshow")


def walk(sim, playback, sim_binary):
    # Main menu -> Shuffle Songs -> Now Playing, with a real queued playlist.
    out = Path(sim.log.name).parent
    Image.fromarray(frame(sim)).save(out / "start.png")
    start_paused(sim, playback)
    Image.fromarray(frame(sim)).save(out / "shuffled.png")
    probe(sim, playback, "now-playing", hold_seek=True, expect_body_stable=False)
    press(sim, "menu")

    # Enter each branch with the wheel and SELECT. MENU unwinds one level.
    probe(sim, playback, "main-menu", hold_seek=True, region=(22, 240, 0, 142))
    move(sim, -6)
    press(sim, "select")
    probe(sim, playback, "music-menu", region=(22, 240, 0, 142))
    press(sim, "select")
    wait_for(lambda: playback.get("cover_visible", size=1),
             "Cover Flow did not present", limit=12)
    if playback.get("cover") > 0:
        move(sim, -1)
    probe_paging(sim, playback, "cover-flow", np.s_[190:225, 65:255])
    press(sim, "select")
    probe(sim, playback, "album-panel")
    press(sim, "menu")
    press(sim, "menu")
    press(sim, "menu")

    move(sim, 2)
    press(sim, "select")
    press(sim, "select")
    probe_paging(sim, playback, "photos-list")
    press(sim, "select")
    probe_paging(sim, playback, "photo", np.s_[50:190, :])
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
    move(sim, 1)
    press(sim, "select")  # Calendars
    press(sim, "select")  # All Calendars, month view
    probe_paging(sim, playback, "calendar-month")
    press(sim, "menu")
    press(sim, "menu")
    move(sim, -1)
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
    press(sim, "select")  # About
    probe_paging(sim, playback, "about", np.s_[135:226, :])
    press(sim, "menu")
    press(sim, "menu")
    sim.settle(quiet=.4, limit=1.5)
    press(sim, "menu", hold=45)
    wait_for(lambda: playback.activity() == playback.activities["ACTIVITY_QUICKSCREEN"],
             "Control Centre did not open")
    # Control Centre is a page of controls, not a navigation page:
    # SELECT closes it cleanly back to the menu.
    press(sim, "select")
    assert playback.activity() != playback.activities["ACTIVITY_QUICKSCREEN"]
    print("PASS control-centre open and close", flush=True)


def probe_chord(sim, playback, theme):
    menu = playback.activities["ACTIVITY_MAINMENU"]
    wps = playback.activities["ACTIVITY_WPS"]
    files = playback.activities["ACTIVITY_FILEBROWSER"]
    wait_for(lambda: playback.activity() == files,
             f"{theme}: Files did not start")
    press(sim, "select")  # Enter the highlighted /Playlists folder.
    press(sim, "select")  # Start the first playlist.
    wait_for(lambda: playback.get("status") == 1 and playback.activity() == wps,
             f"{theme}: could not start fixture playlist")
    # WPS marks its activity before the initial artwork and fonts are ready.
    sim.settle(quiet=.40, limit=1.20)
    press(sim, "menu")
    wait_for(lambda: playback.activity() == files,
             f"{theme}: could not return to Files")
    press(sim, "menu")
    wait_for(lambda: playback.activity() == menu, f"{theme}: could not open menu")
    sim.settle(quiet=.40, limit=1.20)

    fruit = theme.startswith("fruit-company")
    expected = 1
    if fruit:
        press(sim, "play")
        wait_for(lambda: playback.get("status") == 3, f"{theme}: PLAY did not pause")
        assert playback.activity() == menu, (theme, "PLAY left menu")
        expected = 3

    # Let the first down clear the driver's chord window. In PLAY-first
    # order, transport can already have remembered a pending PLAY press.
    for first, second in (("play", "select"), ("select", "play")):
        sim.process.stdin.write(f"down {first}\n".encode())
        time.sleep(.12)
        assert playback.activity() == menu and playback.get("status") == expected, (
            theme, first, "first key acted before the chord")
        sim.process.stdin.write(f"down {second}\n".encode())
        wait_for(lambda: playback.activity() == wps,
                 f"{theme}: {first}+{second} did not open Now Playing")
        for key in (first, second):
            sim.process.stdin.write(f"up {key}\n".encode())
            sim.settle(quiet=.15, limit=.65)
            assert playback.get("status") == expected, (theme, first, key, "chord toggled playback")
            assert playback.activity() == wps, (theme, first, key, "chord left Now Playing")
        press(sim, "menu")
        wait_for(lambda: playback.activity() == menu, f"{theme}: could not return to menu")

    if fruit:
        press(sim, "play")
        wait_for(lambda: playback.get("status") == 1, f"{theme}: PLAY did not resume")
        assert playback.activity() == menu, (theme, "PLAY left menu")
    print(f"PASS {theme} Now Playing chord in both orders", flush=True)


def run_sim(binary, root, output, name, test):
    sim = Sim(binary, root, output, name, name)
    try:
        wait_for(lambda: bool(sim.seen), "simulator did not present", limit=10)
        time.sleep(1.5)
        playback = Playback(binary, sim.process.pid)
        try:
            test(sim, playback)
        finally:
            playback.close()
    finally:
        sim.close()
        sim.process.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for argument in ("binary", "zip", "output"):
        parser.add_argument(argument, type=lambda p: Path(p).resolve())
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    config = CONFIG + """\
animations: off
repeat: all
qs top: keyclick repeats
qs bottom: hardware keyclick
"""
    with tempfile.TemporaryDirectory(prefix="fruit-transport-") as temporary:
        fixture = Path(temporary) / "fixture"
        create_fixture(fixture, args.zip, config + "native theme: fruit-company\n")
        with ZipFile(args.zip) as resources:
            resources.extract(".rockbox/themes/stock-art.bin", fixture)
        for i in range(16):
            image = Image.new("RGB", (192, 192), (24 + i * 12, 180, 98))
            image.save(fixture / "Images" / f"Photo {i:02d}.jpg")
        install_sim_codecs(args.binary, fixture)
        build_tagcache(args.binary, fixture, args.output / "tagcache.log")
        # A database build can leave its temporary startup config behind.
        (fixture / ".rockbox/config.cfg.new").unlink(missing_ok=True)
        themes = fixture / ".rockbox/native-themes"
        themes.mkdir()
        (themes / "fruit-company-ultra.podtheme").write_text(ULTRA_FALLBACK_THEME)
        for theme in CHORD_THEMES:
            root = Path(temporary) / theme
            shutil.copytree(fixture, root)
            (root / ".rockbox/config.cfg").write_text(config + f"""\
native theme: {theme}
start in screen: files
start directory: /Playlists
""", encoding="utf-8")
            run_sim(args.binary, root, args.output, theme,
                    lambda sim, playback: probe_chord(sim, playback, theme))
            assert not (root / ".rockbox/native-theme.log").exists(), (
                theme, "native theme rejected")
        indexed = Path(temporary) / "indexed"
        shutil.copytree(fixture, indexed)
        create_indexed_photos(indexed)
        (indexed / ".rockbox/config.cfg").write_text(config + """\
native theme: fruit-company
photo duration: 0
photo transition: fade
list wraparound: off
""", encoding="utf-8")
        run_sim(args.binary, indexed, args.output, "indexed-photos", indexed_photos)
        run_sim(args.binary, fixture, args.output, "transport",
                lambda sim, playback: walk(sim, playback, args.binary))


if __name__ == "__main__":
    main()
