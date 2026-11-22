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

from PIL import Image, ImageChops, ImageOps, ImageStat


WIDTH, HEIGHT = 320, 240
FRAME_BYTES = WIDTH * HEIGHT * 3
STATE_BYTES = 12 + 8 * (FRAME_BYTES + 4)


def image(pixels):
    return Image.frombytes("RGB", (WIDTH, HEIGHT), pixels)


def changed(before, after, box):
    """Returns the share of pixels in box that changed visibly."""
    delta = ImageChops.difference(image(before).crop(box),
                                  image(after).crop(box))
    mask = ImageOps.grayscale(delta).point(lambda value: value > 12 and 255)
    return ImageStat.Stat(mask).mean[0] / 255


def menu_row(pixels, plaque=None):
    """Returns the main or Single Player menu row under the spinning cursor.

    The menu fades the scene behind it to one pixel in four, never two side
    by side, so only the cursor has bright horizontal pairs. A frame of a
    menu seen earlier, as plaque, also requires the QUAKE plaque beside it."""
    if plaque and changed(plaque, pixels, PLAQUE) > 0.2:
        return None
    gray = ImageOps.grayscale(image(pixels))
    pairs = ImageChops.darker(gray.crop((54, 32, 72, 132)),
                              gray.crop((55, 32, 73, 132)))
    rows = [sum(value > 60 for value in
                pairs.crop((0, y, 18, y + 18)).get_flattened_data())
            for y in range(0, 100, 20)]
    lit = [row for row, count in enumerate(rows) if count >= 10]
    dark = all(count < 3 for row, count in enumerate(rows) if row not in lit)
    return lit[0] if len(lit) == 1 and dark else None


PLAQUE = (16, 4, 48, 140)
VIEW = (0, 0, WIDTH, 192)
AMMO = (224, 216, WIDTH, HEIGHT)
QUIT_BOX = (72, 84, 248, 116)


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
                """Presses key, with a gap the button debounce sees."""
                send("down", key)
                time.sleep(hold)
                send("up", key)
                time.sleep(0.05)

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

            def check_rejection(label, contents, words=("invalid", "pak"),
                                menu=False):
                if contents is not None:
                    pak.write_bytes(contents)
                tap("select")
                text(label, *words)
                if menu:
                    start = time.monotonic()
                    tap("menu")
                text(f"{label}-return", "quake", exclude=words)
                if menu:
                    took = time.monotonic() - start
                    assert took < 2, f"Menu took {took:.1f} s to return"
                assert not (data / "config.cfg").exists()
                print(f"PASS {label}{' with Menu' if menu else ''}", flush=True)

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
                check_rejection("missing-pack-menu", None, ("found", "pak"),
                                menu=True)
                check_rejection("wrong-magic", b"WAD2" + b"\0" * 8)
                check_rejection("wrong-magic-menu", None, menu=True)
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
                    pixels = frame()
                    image(pixels).save(work / f"{label}.png")
                    return pixels

                plaque = None

                def shown(test):
                    pixels = frame()
                    return pixels if pixels and test(pixels) else None

                def at_row(label, row):
                    pixels = wait_for(lambda: shown(
                        lambda pixels: menu_row(pixels, plaque) == row),
                        label, 5)
                    image(pixels).save(work / f"{label}.png")
                    return pixels

                def escape(label):
                    """Hold on presses Escape, which opens the menu; Hold off
                    releases it and leaves the menu as it is."""
                    tap("hold")
                    pixels = wait_for(lambda: shown(
                        lambda pixels: menu_row(pixels, plaque) is not None),
                        label)
                    image(pixels).save(work / f"{label}.png")
                    row = menu_row(pixels, plaque)
                    tap("hold")
                    rows = set()
                    for _ in range(6):
                        time.sleep(0.1)
                        rows.add(menu_row(frame(), plaque))
                    assert rows - {None} == {row}, f"{label}: Hold off {rows}"
                    return row

                def quit_prompt(label, menu):
                    tap("select")
                    wait_for(lambda: changed(menu, frame(), QUIT_BOX) > 0.15,
                             label)
                    image(frame()).save(work / f"{label}.png")

                def quit_quake(label, cancel=False):
                    """Leaves through Quit in the Quake main menu."""
                    escape(f"{label}-menu")
                    for _ in range(5):
                        if menu_row(frame(), plaque) == 4:
                            break
                        tap("play")
                        time.sleep(0.3)
                    menu = at_row(f"{label}-quit-row", 4)
                    quit_prompt(f"{label}-prompt", menu)
                    if cancel:
                        # Escape answers No and returns to the main menu.
                        tap("hold")
                        wait_for(lambda: changed(menu, frame(), QUIT_BOX) < 0.05
                                 and menu_row(frame(), plaque) == 4,
                                 f"{label}-prompt-cancelled")
                        tap("hold")
                        time.sleep(0.3)
                        quit_prompt(f"{label}-prompt-again", menu)
                    # Select answers Yes.
                    tap("select")

                def held(key, seconds):
                    send("down", key)
                    time.sleep(seconds)
                    send("up", key)

                def moved(label, action, floor=0.08):
                    time.sleep(0.6)
                    still = frame()
                    time.sleep(0.6)
                    idle = changed(still, frame(), VIEW)
                    before = frame()
                    action()
                    time.sleep(0.4)
                    after = snapshot(label)
                    share = changed(before, after, VIEW)
                    print(f"{label}: view changed {share:.3f}, idle {idle:.3f}",
                          flush=True)
                    assert share > max(floor, 3 * idle), label

                def ammo_changed(label, action):
                    time.sleep(0.6)
                    before = frame()
                    action()
                    after = snapshot(label)
                    share = changed(before, after, AMMO)
                    print(f"{label}: ammo changed {share:.3f}", flush=True)
                    assert share > 0.02, label

                def settled():
                    before = frame()
                    time.sleep(0.5)
                    return changed(before, frame(), (0, 0, WIDTH, 216)) < 0.01

                # The demo: Hold opens the main menu on Single Player.
                assert escape("main-menu") == 0
                # Play moves down, Menu moves up, Select confirms.
                tap("play")
                at_row("menu-play-down", 1)
                tap("menu")
                plaque = at_row("menu-menu-up", 0)
                tap("select")
                at_row("single-player", 0)
                single = frame()
                tap("select")
                wait_for(lambda: changed(single, frame(), VIEW) > 0.3,
                         "new game")

                wait_for(settled, "level loaded", timeout=40)
                snapshot("new-game")

                ammo_changed("select-fires", lambda: tap("select"))
                moved("menu-forward", lambda: held("menu", 1.5))
                moved("right-turns", lambda: held("right", 0.6))
                moved("left-turns", lambda: held("back", 0.6))
                moved("wheel-strafes",
                      lambda: [tap("up", hold=0.03) for _ in range(10)])
                ammo_changed("play-cycles-weapon", lambda: tap("play"))
                ammo_changed("play-cycles-back", lambda: tap("play"))
                print("PASS level controls", flush=True)

                quit_quake("level-quit", cancel=True)
                text("quit-exit", "quake")
                assert (data / "config.cfg").stat().st_size > 0
                time.sleep(0.15)
                end = pcm.stat().st_size
                time.sleep(0.20)
                assert not audible_since(end), "PCM persisted after exit"
                print("PASS Quit menu exit and silence", flush=True)

                tap("select")
                wait_for(lambda: audible_since(end), "nonzero PCM on relaunch",
                         timeout=30)
                tap("usb")
                text("usb-consent", "select", "charge only")
                tap("select")
                text("usb-storage", "usb")
                tap("usb")
                text("usb-return", "plugins", "games")
                tap("select")
                text("games-after-usb", "quake")
                time.sleep(0.15)
                end = pcm.stat().st_size
                time.sleep(0.20)
                assert not audible_since(end), "PCM persisted after USB"
                tap("select")
                wait_for(lambda: audible_since(end), "PCM after USB relaunch",
                         timeout=30)
                quit_quake("final")
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
