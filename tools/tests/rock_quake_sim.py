#!/usr/bin/env python3
"""Exercise a built ipod6g simulator with an external Quake shareware PACK."""
import argparse
import mmap
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import time
import wave
from zipfile import ZipFile

from PIL import Image, ImageOps


WIDTH, HEIGHT = 320, 240
FRAME_BYTES = WIDTH * HEIGHT * 3
STATE_BYTES = 12 + 8 * (FRAME_BYTES + 4)


def wait_for(check, label, timeout=15):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = check()
        if result:
            return result
        time.sleep(0.04)
    raise AssertionError(f"timed out waiting for {label}")


def exercise(build, external_pak, work, fonts_from):
    work.mkdir(parents=True)
    disk = work / "disk"
    rockbox = disk / ".rockbox"
    games = rockbox / "rocks/games"
    data = rockbox / "quake/id1"
    games.mkdir(parents=True)
    data.mkdir(parents=True)
    fonts = ("18-noto-sans.pbf", "18-noto-sans-bold.pbf", "24x24-icons.fnt")
    with ZipFile(fonts_from) as package:
        for font in fonts:
            package.extract(f".rockbox/fonts/{font}", disk)
        for name in package.namelist():
            if name.startswith(".rockbox/codecs/"):
                package.extract(name, disk)
    (rockbox / "config.cfg").write_text(
        "start in screen: root\nroot menu order: plugins,files,settings\n"
        "animations: off\nbacklight timeout: off\nvoice menus: off\n"
        "volume: -18\n"
        "font: /.rockbox/fonts/18-noto-sans.pbf\n"
        "font bold: /.rockbox/fonts/18-noto-sans-bold.pbf\n"
    )
    shutil.copyfile(build / "rock-compat/modules/games/quake.rock",
                    games / "quake.rock")
    with wave.open(str(disk / "tone.wav"), "wb") as tone:
        tone.setnchannels(2)
        tone.setsampwidth(2)
        tone.setframerate(44100)
        tone.writeframes(b"".join(
            struct.pack("<hh", value, value) for value in
            (8000 if i // 50 % 2 else -8000 for i in range(44100 * 20))))

    state_file = work / "frames"
    pcm = work / "pcm.raw"
    pak = data / "pak0.pak"
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="disk",
               SDL_DISKAUDIOFILE=str(pcm), SDL_RENDER_DRIVER="software",
               PODBOX_UI_WALK_STATE=str(state_file))
    with state_file.open("w+b") as state, (work / "sim.log").open("wb") as log:
        state.truncate(STATE_BYTES)
        with mmap.mmap(state.fileno(), 0, access=mmap.ACCESS_READ) as frames, \
                subprocess.Popen(
                    [str(build / "rockboxui"), "--root", str(disk), "--zoom",
                     "1", "--nobackground", "--screenshot", "--keys", "-"],
                    cwd=work, env=env, stdin=subprocess.PIPE, stdout=log,
                    stderr=subprocess.STDOUT
                ) as sim:

            def send(action, key):
                sim.stdin.write(f"{action} {key}\n".encode())
                sim.stdin.flush()

            def tap(key, hold=0.09):
                send("down", key)
                time.sleep(hold)
                send("up", key)

            def frame():
                assert sim.poll() is None, f"simulator exited ({sim.returncode})"
                _, seq, _ = struct.unpack_from("=III", frames)
                if seq:
                    start = 12 + (seq - 1) % 8 * (FRAME_BYTES + 4)
                    pixels = frames[start + 4:start + 4 + FRAME_BYTES]
                    recorded, = struct.unpack_from("=I", frames, start)
                    if recorded == seq:
                        return pixels
                return None

            def text(label, *expected, exclude=()):
                last = None

                def read():
                    nonlocal last
                    pixels = frame()
                    if not pixels or pixels == last:
                        return None
                    last = pixels
                    image = Image.frombytes("RGB", (WIDTH, HEIGHT), pixels)
                    gray = ImageOps.autocontrast(ImageOps.grayscale(image))
                    if gray.resize((1, 1)).getpixel((0, 0)) < 127:
                        gray = ImageOps.invert(gray)
                    gray.resize((960, 720)).save(work / "ocr.png")
                    words = subprocess.check_output(
                        ["tesseract", str(work / "ocr.png"), "stdout",
                         "--psm", "11"], text=True, stderr=subprocess.DEVNULL)
                    normalized = " ".join(re.findall(r"[a-z0-9]+", words.lower()))
                    if (all(word in normalized for word in expected) and
                            not any(word in normalized for word in exclude)):
                        image.save(work / f"{label}.png")
                        return pixels
                    image.save(work / f"{label}-last.png")
                    return None

                return wait_for(read, label)

            def audible_since(offset):
                if not pcm.exists() or pcm.stat().st_size <= offset:
                    return False
                with pcm.open("rb") as audio:
                    audio.seek(offset)
                    return any(audio.read())

            def check_rejection(label, contents, words=("invalid", "pak")):
                if contents is not None:
                    pak.write_bytes(contents)
                tap("select")
                text(label, *words)
                text(f"{label}-return", "quake", exclude=words)
                assert not (data / "config.cfg").exists()
                print(f"PASS {label}", flush=True)

            def open_games():
                text("home", "plugins")
                tap("select")
                text("categories", "games")
                tap("down")
                tap("select")
                return text("games", "quake")

            try:
                games_frame = open_games()
                check_rejection("missing-pack", None, ("found", "pak"))
                check_rejection("wrong-magic", b"WAD2" + b"\0" * 8)
                check_rejection("truncated-directory",
                                b"PACK" + struct.pack("<II", 12, 64))
                check_rejection(
                    "invalid-extent", b"PACK" + struct.pack("<II", 12, 64)
                    + b"gfx.wad".ljust(56, b"\0") + struct.pack("<II", 12, 1))

                shutil.copyfile(external_pak, pak)
                tap("select")
                wait_for(lambda: audible_since(0) and frame() not in
                         (None, games_frame), "nonzero Quake PCM", timeout=30)
                gameplay = wait_for(frame, "gameplay")
                Image.frombytes("RGB", (WIDTH, HEIGHT), gameplay).save(
                    work / "gameplay.png")

                def snapshot(label):
                    time.sleep(0.5)
                    Image.frombytes("RGB", (WIDTH, HEIGHT), frame()).save(
                        work / f"{label}.png")

                # Menu, Select, Select: Quake menu, Single Player, New Game.
                tap("menu")
                snapshot("quake-menu")
                tap("select")
                snapshot("single-player")
                tap("select")
                time.sleep(4)
                snapshot("new-game")
                send("down", "play")
                send("down", "right")
                time.sleep(1.5)
                send("up", "right")
                send("up", "play")
                tap("select")
                snapshot("moved-and-fired")
                for _ in range(8):
                    tap("up", hold=0.03)
                snapshot("wheel-strafed")
                tap("menu", hold=0.75)
                text("menu-exit", "quake")
                assert (data / "config.cfg").stat().st_size > 0
                time.sleep(0.15)
                end = pcm.stat().st_size
                time.sleep(0.20)
                assert not audible_since(end), "PCM persisted after exit"
                print("PASS gameplay, PCM and Menu exit", flush=True)

                tap("select")
                wait_for(lambda: audible_since(end), "nonzero PCM on relaunch",
                         timeout=30)
                tap("usb")
                text("usb-consent", "select", "charge only")
                tap("select")
                text("usb-storage", "usb")
                tap("usb")
                open_games()
                time.sleep(0.15)
                end = pcm.stat().st_size
                time.sleep(0.20)
                assert not audible_since(end), "PCM persisted after USB"
                tap("select")
                wait_for(lambda: audible_since(end), "PCM after USB relaunch",
                         timeout=30)
                tap("menu", hold=0.75)
                text("final-exit", "quake")
                print("PASS USB takeover, silence and relaunch", flush=True)

                tap("menu")
                text("plugins-again", "games")
                tap("menu")
                text("home-again", "files")
                tap("down")
                tap("select")
                text("files", "tone")
                end = pcm.stat().st_size
                tap("select")
                wait_for(lambda: audible_since(end), "native playback PCM",
                         timeout=15)
                snapshot("native-playback")
                print("PASS native playback after exit", flush=True)
            finally:
                sim.terminate()
                try:
                    sim.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    sim.kill()
                    sim.wait()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path)
    parser.add_argument("external_pak", type=Path)
    parser.add_argument("work", type=Path)
    parser.add_argument("--fonts-from", type=Path, required=True,
                        help="AstraPod firmware ZIP with native fonts")
    args = parser.parse_args()
    exercise(args.build.resolve(), args.external_pak.resolve(),
             args.work.resolve(), args.fonts_from.resolve())
