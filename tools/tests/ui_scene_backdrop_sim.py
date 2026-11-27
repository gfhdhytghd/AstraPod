#!/usr/bin/env python3
"""Check that a list push and pop over the cover backdrop move only what is
drawn over it, in SDL.

Under Scrim's tokens a track with a cover plays, so the file lists show its
blurred cover behind their rows. With PODBOX_PANEL_RECORD, every frame of
the pop to the root folder, the push into a folder and the pop back must
show the backdrop where it stands: at least STILL percent of the screen
below the status bar matches the first or the last frame, which share it.
Before the change a frame matched under half.

The theme gives Now Playing the list's own fonts. Loading fonts of its own
while the track plays makes the playing buffer shrink, and the simulator
then corrupts its heap.

Arguments: ROCKBOXUI ZIP OUTPUT_DIRECTORY
Xvfb is private to this process. Writes result.json, the simulator log and
a frame strip of each transition to the output directory.
"""

import argparse
import json
import os
import shutil
import struct
import subprocess
import tempfile
import time
import wave
from pathlib import Path
from zipfile import ZipFile

from PIL import Image, ImageChops, ImageDraw

from ui_list_snap_sim import click
from ui_menu_sim import FPS, Screen, process


STATUS = 35
STILL = 80
RECORD = struct.Struct("<4sIcBhhhI32h")
RECORD_SIZE = RECORD.size + 320 * 240 * 2
THEME = ("podbox-theme=1\nfont.wps_title = 18-noto-sans\n"
         "font.wps_meta = 18-noto-sans\nfont.wps_time = 18-noto-sans\n")


def frames_in(record, start, end):
    data = record.read_bytes()
    end = min(end, len(data) // RECORD_SIZE)
    return [Image.frombytes("RGB", (320, 240),
                            data[i * RECORD_SIZE + RECORD.size:
                                 (i + 1) * RECORD_SIZE], "raw", "BGR;16")
            for i in range(start, end)]


def count(record):
    return record.stat().st_size // RECORD_SIZE if record.is_file() else 0


def stillness(frames):
    """Per frame, the percentage of the screen below the status bar that
    matches the first or the last frame exactly."""
    box = (0, STATUS, 320, 240)
    first, last = frames[0].crop(box), frames[-1].crop(box)
    out = []
    for frame in frames:
        body = frame.crop(box)
        same = ImageChops.lighter(
            ImageChops.difference(body, first).convert("L").point(
                lambda v: 255 if v == 0 else 0),
            ImageChops.difference(body, last).convert("L").point(
                lambda v: 255 if v == 0 else 0))
        out.append(round(100 * same.histogram()[255] /
                         (320 * (240 - STATUS))))
    return out


def strip(frames, path):
    shots = frames[:32]
    sheet = Image.new("RGB", (160 * min(8, len(shots)),
                              120 * ((len(shots) + 7) // 8)))
    for i, shot in enumerate(shots):
        sheet.paste(shot.resize((160, 120)), (160 * (i % 8), 120 * (i // 8)))
    sheet.save(path)


def add_wave(path):
    with wave.open(str(path), "wb") as audio:
        audio.setparams((1, 2, 8000, 0, "NONE", "not compressed"))
        audio.writeframes(bytes(8000 * 2 * 60))


def fixture(root, binary, resource):
    root.mkdir()
    with ZipFile(resource) as archive:
        for path in archive.namelist():
            if path.startswith(".rockbox/fonts/"):
                archive.extract(path, root)
    rockbox = root / ".rockbox"
    (rockbox / "codecs").mkdir()
    shutil.copy(binary.parent / "lib/rbcodec/codecs/wav.codec",
                rockbox / "codecs")
    (rockbox / "native-themes").mkdir()
    (rockbox / "native-themes/backdrop.podtheme").write_text(THEME)
    (rockbox / "config.cfg").write_text(
        "start in screen: files\nshow files: supported\nlist wraparound: off\n"
        "backlight timeout: off\nvoice menus: off\nkeyclick: off\n"
        "animations: on\nbeep: off\nrepeat: one\nnative theme: backdrop\n")
    album = root / "Album"
    album.mkdir()
    cover = Image.new("RGB", (128, 128))
    draw = ImageDraw.Draw(cover)
    for y in range(128):
        draw.line((0, y, 127, y), fill=(220 - y, 40 + y // 2, 60 + y))
    draw.ellipse((10, 16, 64, 70), fill=(250, 200, 40))
    draw.rectangle((74, 64, 122, 122), fill=(30, 200, 90))
    cover.save(album / "cover.bmp")
    add_wave(album / "01 Song.wav")
    for i in range(8):
        folder = root / f"Folder {i + 1:02d}"
        folder.mkdir()
        for k in range(6):
            (folder / f"Sub {k + 1}").mkdir()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("binary", "resource", "output"):
        parser.add_argument(name, type=lambda arg: Path(arg).resolve())
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)

    result = {}
    with tempfile.TemporaryDirectory(prefix="podbox-backdrop-") as folder, \
            tempfile.TemporaryFile() as display, \
            (args.output / "xvfb.log").open("w") as xlog:
        root = Path(folder) / "root"
        fixture(root, args.binary, args.resource)
        runtime = Path(folder) / "runtime"
        runtime.mkdir(mode=0o700)
        with process(["Xvfb", "-displayfd", str(display.fileno()), "-screen",
                      "0", "800x600x24", "-nolisten", "tcp"], xlog,
                     pass_fds=(display.fileno(),)):
            for _ in range(120):
                display.seek(0)
                number = display.read().decode().strip()
                if number:
                    break
                time.sleep(1 / FPS)
            else:
                raise RuntimeError("Xvfb did not start")
            env = dict(os.environ, DISPLAY=":" + number,
                       XDG_RUNTIME_DIR=str(runtime), SDL_VIDEODRIVER="x11",
                       SDL_AUDIODRIVER="dummy", SDL_RENDER_DRIVER="software",
                       LIBGL_ALWAYS_SOFTWARE="1")
            record = root / "panel.rec"
            with (args.output / "sim.log").open("w") as log, process(
                    [str(args.binary), "--root", str(root), "--nobackground",
                     "--zoom", "1", "--screenshot"], log, cwd=root,
                    env=dict(env, PODBOX_PANEL_RECORD=str(record))) as sim:
                time.sleep(240 / FPS)
                screen = Screen(env, sim)
                x, y, _, _ = screen.bbox
                subprocess.run(["xdotool", "mousemove", str(x + 120),
                                str(y + 120)], env=env, check=True)

                def tap(key, wait):
                    subprocess.run(
                        ["xdotool", "keydown", "--window", screen.window, key,
                         "sleep", str(2 / FPS), "keyup", "--window",
                         screen.window, key], env=env, check=True)
                    time.sleep(wait)

                def transition(name, action):
                    start = count(record)
                    action()
                    frames = frames_in(record, start, count(record))
                    assert frames, (name, "no frame reached the panel")
                    strip(frames, args.output / f"{name}.png")
                    result[name] = {"frames": len(frames),
                                    "still": stillness(frames)}

                tap("Return", 90 / FPS)
                tap("Return", 180 / FPS)
                tap("Escape", 120 / FPS)
                transition("pop-root", lambda: tap("Escape", 90 / FPS))
                click(screen, 1, 5, 250)
                time.sleep(60 / FPS)
                transition("push-folder", lambda: tap("Return", 90 / FPS))
                transition("pop-folder", lambda: tap("Escape", 90 / FPS))
                assert sim.poll() is None, "the simulator stopped"
    (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n")

    for name, outcome in result.items():
        assert outcome["frames"] >= 4, (name, outcome)
        assert min(outcome["still"]) >= STILL, (name, outcome)
        assert min(outcome["still"]) < 100, (name, "nothing moved", outcome)
    print("PASS backdrop stays still: " + ", ".join(
        f"{name} {min(o['still'])}%" for name, o in result.items()))


if __name__ == "__main__":
    main()
