#!/usr/bin/env python3
"""Check that Now Playing can shrink the audio buffer while the audio thread
is laying out a new one, in SDL.

An 8 kHz track makes the mixer move to 48 kHz as it loads, and the audio
thread then lays the buffer out again. Now Playing opens at the same moment
and loads two fonts the theme does not already hold, which shrinks the
playing buffer. The simulator must still be running, with no panic in its
log, after Now Playing closes and frees those fonts.

Arguments: ROCKBOXUI ZIP OUTPUT_DIRECTORY
Xvfb is private to this process. Writes the simulator log and a screenshot
of each step to the output directory.
"""

import argparse
import os
import subprocess
import tempfile
import time
import wave
from pathlib import Path
from zipfile import ZipFile

from PIL import Image

from ui_menu_sim import FPS, Screen, process


def fixture(root, resource):
    root.mkdir()
    with ZipFile(resource) as archive:
        for path in archive.namelist():
            if (path.startswith(".rockbox/fonts/") or
                    path == ".rockbox/codecs/wav.codec"):
                archive.extract(path, root)
    rockbox = root / ".rockbox"
    (rockbox / "native-themes").mkdir()
    (rockbox / "native-themes/plain.podtheme").write_text("podbox-theme=1\n")
    (rockbox / "config.cfg").write_text(
        "start in screen: files\nshow files: supported\n"
        "backlight timeout: off\nvoice menus: off\nkeyclick: off\n"
        "beep: off\nrepeat: one\nnative theme: plain\n")
    album = root / "Album"
    album.mkdir()
    Image.new("RGB", (128, 128), (200, 60, 40)).save(album / "cover.bmp")
    with wave.open(str(album / "01 Song.wav"), "wb") as audio:
        audio.setparams((1, 2, 8000, 0, "NONE", "not compressed"))
        audio.writeframes(bytes(8000 * 2 * 60))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("binary", "resource", "output"):
        parser.add_argument(name, type=lambda arg: Path(arg).resolve())
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    log_path = args.output / "sim.log"

    with tempfile.TemporaryDirectory(prefix="podbox-shrink-") as folder, \
            tempfile.TemporaryFile() as display, \
            (args.output / "xvfb.log").open("w") as xlog:
        root = Path(folder) / "root"
        fixture(root, args.resource)
        runtime = Path(folder) / "runtime"
        runtime.mkdir(mode=0o700)
        with process(["Xvfb", "-displayfd", str(display.fileno()), "-screen",
                      "0", "800x600x24", "-nolisten", "tcp"], xlog,
                     pass_fds=(display.fileno(),)):
            for _ in range(FPS * 5):
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
            with log_path.open("w") as log, process(
                    [str(args.binary), "--root", str(root), "--nobackground",
                     "--zoom", "1", "--screenshot"], log, cwd=root,
                    env=env) as sim:
                time.sleep(240 / FPS)
                screen = Screen(env, sim)

                def step(name, key, wait):
                    assert sim.poll() is None, ("stopped before", name)
                    subprocess.run(
                        ["xdotool", "keydown", "--window", screen.window, key,
                         "sleep", str(2 / FPS), "keyup", "--window",
                         screen.window, key], env=env, check=True)
                    time.sleep(wait)
                    if sim.poll() is None:
                        screen.frame().save(args.output / f"{name}.png")

                step("album", "Return", 90 / FPS)
                step("now-playing", "Return", 180 / FPS)
                step("left", "Escape", 120 / FPS)
                step("root", "Escape", 120 / FPS)
                alive = sim.poll() is None

    panics = [line for line in log_path.read_text(errors="replace")
              .splitlines() if "PANIC" in line or "wacky" in line]
    assert alive and not panics, panics or "the simulator stopped"
    print("PASS Now Playing shrinks the buffer during a re-layout")


if __name__ == "__main__":
    main()
