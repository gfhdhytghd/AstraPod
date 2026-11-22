#!/usr/bin/env python3
"""Run XRick on a private ipod6g simulator volume.

Pass the extracted stock .rockbox/xrick directory to reject a truncated
copy, then play the game, quit through its menu and take over USB during
play. Without game data the run
checks the missing-data screen only.
"""
import argparse
import mmap
import os
from pathlib import Path
import re
import shutil
import signal
import struct
import subprocess
import time
from zipfile import ZipFile

from PIL import Image, ImageOps


FRAME_SIZE = (320, 240)
FRAME_BYTES = 320 * 240 * 3
STATE_BYTES = 12 + 8 * (4 + FRAME_BYTES)


def prepare(case, archive, data, damaged):
    disk = case / "disk"
    with ZipFile(archive) as package:
        for name in (".rockbox/rocks/games/xrick.rock",
                     ".rockbox/fonts/18-noto-sans.pbf",
                     ".rockbox/fonts/18-noto-sans-bold.pbf",
                     ".rockbox/fonts/24x24-icons.fnt"):
            package.extract(name, disk)
    (disk / ".rockbox/config.cfg").write_text(
        "start in screen: root\nroot menu order: plugins,files,settings\n"
        "animations: off\nbacklight timeout: off\nvoice menus: off\n"
        "usb mode: mass storage\n"
        "font: /.rockbox/fonts/18-noto-sans.pbf\n"
        "font bold: /.rockbox/fonts/18-noto-sans-bold.pbf\n"
        "native theme: scrim\n"
    )
    if data:
        shutil.copytree(data, disk / ".rockbox/xrick")
    if damaged:
        maps = disk / ".rockbox/xrick/misc/maps.dat"
        maps.write_bytes(maps.read_bytes()[:5])
    return disk


def exercise(simulator, archive, case, data, damaged=False):
    case.mkdir(parents=True)
    disk = prepare(case, archive, data, damaged)
    state_path = case / "state.bin"
    command = [
        "bwrap", "--die-with-parent", "--ro-bind", "/", "/",
        "--dev", "/dev", "--proc", "/proc", "--tmpfs", "/tmp",
        "--unshare-net", "--bind", str(case), str(case), "--chdir", str(disk),
        "--", str(simulator), "--root", ".", "--nobackground", "--zoom", "1",
        "--keys", "-",
    ]
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy",
               SDL_RENDER_DRIVER="software", XDG_RUNTIME_DIR="/tmp",
               PODBOX_UI_WALK_STATE=str(state_path))
    env.pop("LD_PRELOAD", None)
    with state_path.open("w+b") as handle, (case / "firmware.log").open("wb") as log:
        handle.truncate(STATE_BYTES)
        with mmap.mmap(handle.fileno(), 0, access=mmap.ACCESS_READ) as state, \
                subprocess.Popen(command, stdin=subprocess.PIPE, env=env, cwd=disk,
                                 stdout=log, stderr=subprocess.STDOUT,
                                 start_new_session=True) as sim:
            def press(key, seconds=.12):
                sim.stdin.write(f"down {key}\n".encode())
                sim.stdin.flush()
                time.sleep(seconds)
                sim.stdin.write(f"up {key}\n".encode())
                sim.stdin.flush()
                time.sleep(.3)

            def capture(label, *expected, changed_from=None, seconds=15,
                        psm="11"):
                deadline = time.monotonic() + seconds
                words = ""
                while time.monotonic() < deadline:
                    if sim.poll() is not None:
                        raise RuntimeError(f"SDL exited with status {sim.returncode}")
                    _, seq, _ = struct.unpack_from("=III", state)
                    if seq:
                        offset = 12 + ((seq - 1) % 8) * (FRAME_BYTES + 4)
                        pixels = state[offset + 4:offset + 4 + FRAME_BYTES]
                        present, = struct.unpack_from("=I", state, offset)
                        if present == seq and pixels != changed_from:
                            frame = Image.frombytes("RGB", FRAME_SIZE, pixels)
                            frame.save(case / f"{label}.png")
                            if not expected:
                                return pixels
                            image = ImageOps.autocontrast(ImageOps.grayscale(frame))
                            if image.resize((1, 1)).getpixel((0, 0)) < 127:
                                image = ImageOps.invert(image)
                            image.resize((image.width * 3, image.height * 3)).save(
                                case / "ocr.png")
                            words = subprocess.check_output(
                                ["tesseract", str(case / "ocr.png"), "stdout",
                                 "--psm", psm], text=True, stderr=subprocess.DEVNULL,
                                env=dict(os.environ, OMP_THREAD_LIMIT="1"),
                            )
                            words = " ".join(re.findall(r"[a-z0-9]+", words.casefold()))
                            with (case / "ocr.log").open("a") as trace:
                                trace.write(f"{label}: {words}\n")
                            if all(term in words for term in expected):
                                print(case.name, label, words, flush=True)
                                return pixels
                    time.sleep(.12)
                raise AssertionError((case.name, label, expected, words))

            def colours(pixels):
                image = Image.frombytes("RGB", FRAME_SIZE, pixels)
                return len(image.getcolors(1 << 16) or ())

            def launch():
                capture("home", "plugins")
                press("select")
                capture("categories", "games")
                press("down")
                press("select")
                capture("browser", "games")
                press("select")

            try:
                launch()
                if not data:
                    capture("missing", "missing xrick data", "xrick data zip",
                            "menu return")
                    press("menu")
                    capture("returned", "games")
                    return
                if damaged:
                    capture("damaged", "error", "maps", psm="6")
                    capture("returned", "games", seconds=20)
                    return

                splash = capture("splash", seconds=5)
                time.sleep(3)
                intro = capture("intro", changed_from=splash)
                assert colours(intro) >= 4, "XRick drew no picture"
                for _ in range(3):       # splash, hall of fame, map intro
                    press("select")
                    time.sleep(2.5)
                before = capture("game")
                press("right", 1.5)
                moved = capture("game-moved", changed_from=before)
                assert colours(moved) >= 4
                press("menu")
                capture("menu", "resume", "quit")
                press("select")          # Resume
                capture("resumed", changed_from=None)
                press("menu")
                capture("menu-again", "resume", "quit")
                for _ in range(3):
                    press("down")
                press("select")
                capture("returned", "games")

                press("select")
                time.sleep(4)
                capture("relaunched")
                press("usb")
                time.sleep(2)
                capture("usb", "computer connected", "charge only")
                press("select")          # Connect: storage leaves the game
                time.sleep(3)
                capture("usb-connected")
                press("usb")
                capture("after-usb", "games", seconds=20)
            finally:
                if sim.poll() is None:
                    os.killpg(sim.pid, signal.SIGTERM)
                sim.wait(timeout=8)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("simulator", type=Path)
    parser.add_argument("archive", type=Path)
    parser.add_argument("evidence", type=Path)
    parser.add_argument("--data", type=Path,
                        help="extracted stock .rockbox/xrick directory")
    args = parser.parse_args()
    evidence = args.evidence.resolve()
    exercise(args.simulator.resolve(), args.archive.resolve(),
             evidence / "missing", None)
    if args.data:
        exercise(args.simulator.resolve(), args.archive.resolve(),
                 evidence / "damaged", args.data.resolve(), damaged=True)
        exercise(args.simulator.resolve(), args.archive.resolve(),
                 evidence / "play", args.data.resolve())


if __name__ == "__main__":
    main()
