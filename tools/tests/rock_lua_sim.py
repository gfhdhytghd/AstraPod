#!/usr/bin/env python3
"""Play the three bundled Lua games on a private ipod6g simulator volume."""
import argparse
import mmap
import os
from pathlib import Path
import re
import signal
import struct
import subprocess
import time
from zipfile import ZipFile

from PIL import Image, ImageOps


FRAME_SIZE = (320, 240)
FRAME_BYTES = 320 * 240 * 3
STATE_BYTES = 12 + 8 * (4 + FRAME_BYTES)
GAMES = ("boomshine", "picross", "pixel-painter")


def prepare(case, archive, theme):
    disk = case / "disk"
    with ZipFile(archive) as package:
        for name in package.namelist():
            if (name.startswith((".rockbox/rocks/viewers/lua/",
                                 ".rockbox/rocks/games/.picross/"))
                or name == ".rockbox/rocks/viewers/lua.rock"
                or name in {f".rockbox/rocks/games/{game}.lua" for game in GAMES}
                or name in {f".rockbox/fonts/{font}" for font in
                            ("18-noto-sans.pbf", "18-noto-sans-bold.pbf",
                             "24x24-icons.fnt")}):
                package.extract(name, disk)
    (disk / ".rockbox/config.cfg").write_text(
        "start in screen: root\nroot menu order: plugins,files,settings\n"
        "animations: off\nbacklight timeout: off\nvoice menus: off\n"
        "font: /.rockbox/fonts/18-noto-sans.pbf\n"
        "font bold: /.rockbox/fonts/18-noto-sans-bold.pbf\n"
        f"native theme: {theme}\n"
    )
    assert (disk / ".rockbox/rocks/viewers/lua.rock").is_file()
    for game in GAMES:
        assert (disk / f".rockbox/rocks/games/{game}.lua").is_file()
    return disk


def exercise(simulator, archive, case, theme):
    case.mkdir(parents=True)
    disk = prepare(case, archive, theme)
    state_path = case / "state.bin"
    command = [
        "bwrap", "--die-with-parent", "--ro-bind", "/", "/",
        "--dev", "/dev", "--proc", "/proc", "--tmpfs", "/tmp",
        "--unshare-net", "--bind", str(case), str(case), "--chdir", str(disk),
        "--", str(simulator), "--root", ".", "--nobackground", "--zoom", "1",
        "--screenshot", "--keys", "-",
    ]
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy",
               SDL_RENDER_DRIVER="software", XDG_RUNTIME_DIR="/tmp",
               PODBOX_UI_WALK_STATE=str(state_path))
    with state_path.open("w+b") as handle, (case / "firmware.log").open("wb") as log:
        handle.truncate(STATE_BYTES)
        with mmap.mmap(handle.fileno(), 0, access=mmap.ACCESS_READ) as state, \
                subprocess.Popen(command, stdin=subprocess.PIPE, env=env, cwd=disk,
                                 stdout=log, stderr=subprocess.STDOUT,
                                 start_new_session=True) as sim:
            def tap(*keys):
                command = "".join(f"down {key}\n" for key in keys)
                sim.stdin.write(command.encode())
                sim.stdin.flush()
                time.sleep(.12)
                command = "".join(f"up {key}\n" for key in reversed(keys))
                sim.stdin.write(command.encode())
                sim.stdin.flush()
                time.sleep(.30)

            def capture(label, *expected, changed_from=None, seconds=10):
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
                            image.resize((960, 720)).save(case / "ocr.png")
                            words = subprocess.check_output(
                                ["tesseract", str(case / "ocr.png"), "stdout",
                                 "--psm", "11"], text=True, stderr=subprocess.DEVNULL,
                            )
                            words = " ".join(re.findall(r"[a-z0-9]+", words.casefold()))
                            if all(term in words for term in expected):
                                print(theme, label, words, flush=True)
                                return pixels
                    time.sleep(.12)
                raise AssertionError((theme, label, expected, words))

            try:
                capture("home", "plugins")
                tap("select")
                capture("categories", "games")
                tap("down")
                tap("select")
                browser = capture("games", "boomshine", "picross", "pixel")

                tap("select")
                capture("boomshine-title", "boomshine")
                capture("boomshine-round", "total points", seconds=25)
                tap("right")
                capture("boomshine-move")
                tap("menu")
                capture("boomshine-exit", "press button to exit")
                # Boomshine clears queued input 500 ms after drawing its prompt.
                time.sleep(.5)
                tap("back")
                capture("boomshine-return", "boomshine", "picross", "pixel")

                tap("down")
                tap("select")
                board = capture("picross-board", changed_from=browser)
                tap("right")
                tap("select")
                capture("picross-play", changed_from=board)
                tap("menu")
                capture("picross-menu", "resume", "restart puzzle", "picross")
                for _ in range(6):
                    tap("down")
                tap("select")
                capture("picross-return", "boomshine", "picross", "pixel")
                save = disk / ".rockbox/rocks/games/picross.sav"
                assert save.read_text().startswith("#/.rockbox/rocks/games/.picross/")
                assert not save.with_suffix(".sav.tmp").exists()

                tap("down")
                before = capture("pixel-browser", "games", "pixel")
                tap("select")
                board = capture("pixel-board", changed_from=before)
                tap("down")
                tap("select")
                capture("pixel-play", changed_from=board)
                tap("menu")
                capture("pixel-menu", "resume game", "change difficulty", "quit without saving")
                for _ in range(5):
                    tap("down")
                tap("select")
                capture("pixel-return", "boomshine", "picross", "pixel")
                save = disk / ".rockbox/rocks/games/pixel-painter.save"
                assert save.read_text().splitlines()[0] == "2"
                assert not save.with_suffix(".save.tmp").exists()
            finally:
                if sim.poll() is None:
                    os.killpg(sim.pid, signal.SIGTERM)
                sim.wait(timeout=8)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("simulator", type=Path)
    parser.add_argument("archive", type=Path)
    parser.add_argument("evidence", type=Path)
    args = parser.parse_args()
    for theme in ("scrim",):
        exercise(args.simulator.resolve(), args.archive.resolve(),
                 args.evidence.resolve() / theme, theme)


if __name__ == "__main__":
    main()
