#!/usr/bin/env python3
"""Play 2048 headless in the SDL simulator and record its frames.

Usage: rock_2048_sim.py ROCKBOXUI SIMDISK OUTPUT_DIRECTORY [--theme NAME]
       [--rock 2048.rock]

SIMDISK is a simulator root with .rockbox in it, as build-sim.sh leaves;
the game is the 2048.rock there unless --rock names another. The script
opens the game from the theme's Games list, plays a fixed list of moves,
resumes from the pause menu, quits with a save and resumes the save, and
fails if either resume changes the board. PODBOX_PANEL_RECORD captures
every LCD submission. Writes each move's frames as PNGs, sheet.png (one row
per move), frames.json with the interval between submissions and the area
each one changed, and summary.json.
"""
import argparse
import json
import mmap
import os
from pathlib import Path
import shutil
import signal
import struct
import subprocess
import time

from PIL import Image, ImageChops, ImageDraw

WIDTH, HEIGHT = 320, 240
FRAME_BYTES = WIDTH * HEIGHT * 3
STATE_BYTES = 12 + 8 * (4 + FRAME_BYTES)
RECORD = struct.Struct("=4sIcBhhhI32h")
PANEL_BYTES = WIDTH * HEIGHT * 2
MOVES = ["right", "play", "back", "menu", "right", "play", "back", "play",
         "right", "menu", "back", "play"]
# From the main menu to the installed games: Extras > Games in Fruit
# Company, Plugins > Games in the others.
ROUTES = {
    "fruit-company": [("down", 4), ("select", 1), ("down", 4), ("select", 1)],
    "classic": [("up", 3), ("select", 1), ("down", 1), ("select", 1)],
    "scrim": [("up", 3), ("select", 1), ("down", 1), ("select", 1)],
}


class Simulator:
    def __init__(self, proc, frames):
        self.proc = proc
        self.frames = frames

    def key(self, verb, name):
        self.proc.stdin.write(f"{verb} {name}\n".encode())
        self.proc.stdin.flush()

    def tap(self, name, hold=.08, gap=.45):
        self.key("down", name)
        time.sleep(hold)
        self.key("up", name)
        time.sleep(gap)

    def frame(self):
        assert self.proc.poll() is None, self.proc.returncode
        for _ in range(50):
            sequence, = struct.unpack_from("=I", self.frames, 4)
            if not sequence:
                time.sleep(.05)
                continue
            offset = 12 + ((sequence - 1) % 8) * (FRAME_BYTES + 4)
            pixels = self.frames[offset + 4:offset + 4 + FRAME_BYTES]
            stable, = struct.unpack_from("=I", self.frames, offset)
            if stable == sequence:
                return Image.frombytes("RGB", (WIDTH, HEIGHT), pixels)
            time.sleep(.01)
        raise AssertionError("no frame")


def make_disk(simdisk, case, theme, rock):
    disk = case / "disk"
    rockbox = disk / ".rockbox"
    rockbox.mkdir(parents=True)
    for entry in (simdisk / ".rockbox").iterdir():
        if entry.name == "rocks":
            continue
        if entry.name in ("fonts", "codecs", "speech"):
            (rockbox / entry.name).symlink_to(entry.resolve())
        elif entry.is_dir():
            shutil.copytree(entry, rockbox / entry.name, symlinks=True)
        else:
            shutil.copy2(entry, rockbox / entry.name)
    games = rockbox / "rocks" / "games"
    games.mkdir(parents=True)
    shutil.copy2(rock, games / "2048.rock")
    config = (simdisk / ".rockbox" / "default-config.cfg").read_text()
    (rockbox / "config.cfg").write_text(
        config + f"\nnative theme: {theme}\nbacklight timeout: off\n"
        "voice menus: off\nbeep: off\n")
    return disk


def read_panel(path):
    data = path.read_bytes()
    size = RECORD.size + PANEL_BYTES
    records = []
    for at in range(0, len(data) - size + 1, size):
        magic, usec = RECORD.unpack_from(data, at)[:2]
        assert magic == b"PNL1"
        raw = data[at + RECORD.size:at + size]
        image = Image.frombytes("RGB", (WIDTH, HEIGHT), raw, "raw", "BGR;16")
        records.append((usec, image))
    return records


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("simulator", type=Path)
    parser.add_argument("simdisk", type=Path)
    parser.add_argument("out", type=Path)
    parser.add_argument("--theme", default="scrim")
    parser.add_argument("--rock", type=Path)
    args = parser.parse_args()
    case = args.out.resolve() / args.theme
    shutil.rmtree(case, ignore_errors=True)
    case.mkdir(parents=True)
    rock = args.rock or args.simdisk / ".rockbox/rocks/games/2048.rock"
    disk = make_disk(args.simdisk.resolve(), case, args.theme, rock)
    binary = args.simulator.resolve()
    state = case / "state.bin"
    panel = case / "panel.bin"
    with state.open("w+b") as handle, (case / "firmware.log").open("wb") as log:
        handle.truncate(STATE_BYTES)
        command = [
            "bwrap", "--die-with-parent", "--ro-bind", "/", "/",
            "--dev", "/dev", "--proc", "/proc", "--tmpfs", "/tmp",
            "--unshare-net", "--ro-bind", str(binary.parent),
            str(binary.parent), "--bind", str(case), str(case),
            "--chdir", str(disk), "--", str(binary),
            "--root", ".", "--nobackground", "--zoom", "1", "--screenshot",
            "--keys", "-"]
        env = dict(os.environ, SDL_VIDEODRIVER="dummy",
                   SDL_AUDIODRIVER="dummy", SDL_RENDER_DRIVER="software",
                   XDG_RUNTIME_DIR="/tmp", PODBOX_UI_WALK_STATE=str(state),
                   PODBOX_PANEL_RECORD=str(panel), TZ="UTC")
        with mmap.mmap(handle.fileno(), 0, access=mmap.ACCESS_READ) as ring, \
                subprocess.Popen(command, stdin=subprocess.PIPE, env=env,
                                 cwd=disk, stdout=log,
                                 stderr=subprocess.STDOUT,
                                 start_new_session=True) as proc:
            sim = Simulator(proc, ring)
            try:
                time.sleep(4)
                play(sim, case, args.theme)
            finally:
                if proc.poll() is None:
                    os.killpg(proc.pid, signal.SIGTERM)
                proc.wait(timeout=8)


def play(sim, case, theme):
    panel = case / "panel.bin"
    size = RECORD.size + PANEL_BYTES
    for key, count in ROUTES[theme]:
        for _ in range(count):
            sim.tap(key)
    sim.frame().save(case / "games.png")
    sim.tap("select")        # 2048
    sim.frame().save(case / "menu.png")
    sim.tap("select", gap=1.2)   # Start New Game
    sim.frame().save(case / "game.png")
    marks = []
    for key in MOVES:
        first = panel.stat().st_size // size
        sim.tap(key, gap=.9)
        marks.append((key, first, panel.stat().st_size // size))
    played = sim.frame()
    played.save(case / "played.png")
    pause(sim)
    sim.frame().save(case / "pause.png")
    sim.tap("select", gap=1.2)   # Resume Game
    resumed = sim.frame()
    resumed.save(case / "resumed.png")
    assert same_board(played, resumed), "Resume Game changed the board"
    pause(sim)
    for _ in range(6):
        sim.tap("down")
    sim.tap("select", gap=1.5)   # Quit, which saves the game
    sim.frame().save(case / "left.png")
    sim.tap("select")            # 2048 again
    sim.tap("select", gap=1.2)   # Resume Game, from the save
    reopened = sim.frame()
    reopened.save(case / "reopened.png")
    assert same_board(played, reopened), "the save did not restore the board"
    report(case, marks)


def pause(sim):
    sim.key("down", "select")
    time.sleep(1.5)
    sim.key("up", "select")
    time.sleep(.8)


def same_board(a, b):
    return not ImageChops.difference(a, b).getbbox()


def report(case, marks):
    records = read_panel(case / "panel.bin")
    moves = []
    sheet_rows = []
    for key, first, last in marks:
        frames = records[first:last]
        if not frames:
            continue
        times = [usec for usec, _ in frames]
        gaps = [(b - a) / 1000 for a, b in zip(times, times[1:])]
        changed = []
        for (_, a), (_, b) in zip([records[first - 1]] + frames, frames):
            box = ImageChops.difference(a, b).getbbox()
            changed.append(list(box) if box else None)
        moves.append({"key": key, "frames": len(frames),
                      "span_ms": (times[-1] - times[0]) / 1000,
                      "gaps_ms": gaps, "changed": changed})
        sheet_rows.append([image for _, image in frames])
        folder = case / f"move{len(moves):02d}-{key}"
        folder.mkdir()
        for i, (usec, image) in enumerate(frames):
            image.save(folder / f"{i:02d}-{usec}.png")
    (case / "frames.json").write_text(json.dumps(moves, indent=1))
    gaps = [g for move in moves for g in move["gaps_ms"]]
    summary = {
        "moves": len(moves),
        "frames": sum(move["frames"] for move in moves),
        "mean_gap_ms": sum(gaps) / len(gaps) if gaps else None,
        "max_gap_ms": max(gaps) if gaps else None,
        "frames_per_move": [move["frames"] for move in moves],
    }
    (case / "summary.json").write_text(json.dumps(summary, indent=1))
    print(case.name, json.dumps(summary), flush=True)
    contact(case / "sheet.png", sheet_rows[:6])


def contact(path, rows, scale=2):
    """One row per move, its frames left to right at half size."""
    w, h = WIDTH // scale, HEIGHT // scale
    columns = max(len(row) for row in rows)
    sheet = Image.new("RGB", (columns * (w + 4), len(rows) * (h + 4)), "white")
    for r, row in enumerate(rows):
        for c, image in enumerate(row):
            sheet.paste(image.resize((w, h)), (c * (w + 4), r * (h + 4)))
    sheet.save(path)


if __name__ == "__main__":
    main()
