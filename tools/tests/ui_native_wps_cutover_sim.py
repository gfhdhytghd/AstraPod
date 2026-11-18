#!/usr/bin/env python3
"""Exercise retained Now Playing with real local audio and no skin files."""
import argparse
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import time
import wave
import zipfile

from PIL import Image, ImageChops, ImageDraw

from ui_menu_sim import FPS, Screen, process

TITLE = (5, 85, 235, 113)
TRANSPORT = (0, 0, 78, 35)
VOLUME = (40, 15, 280, 21)
PROGRESS = (0, 140, 240, 145)
COVER_POINT = (305, 225)


def audio_fixture(root):
    path = root / "Track.wav"
    sample_rate = 8000
    sample_count = sample_rate * 600
    tone = struct.pack("<8000h", *(
        int(9500 * math.sin(2 * math.pi * 160 * i / sample_rate) +
            9500 * math.sin(2 * math.pi * 680 * i / sample_rate))
        for i in range(sample_rate)
    ))
    with wave.open(str(path), "wb") as audio:
        audio.setparams((1, 2, sample_rate, 0, "NONE", "not compressed"))
        audio.writeframes(tone * (sample_count // sample_rate))
    title = b"Very long demonstration title for the retained Now Playing screen"
    artist = b"PodBox examples"
    fields = b"INFO"
    for field, value in ((b"INAM", title), (b"IART", artist)):
        value += b"\0"
        fields += struct.pack("<4sI", field, len(value)) + value
        if len(value) % 2:
            fields += b"\0"
    with path.open("r+b") as audio:
        wave_data = 44 + sample_count * 2
        audio.seek(wave_data)
        audio.write(struct.pack("<4sI", b"LIST", len(fields)) + fields)
        end = audio.tell()
        audio.truncate()
        audio.seek(4)
        audio.write(struct.pack("<I", end - 8))

    cover = Image.new("RGB", (320, 320))
    draw = ImageDraw.Draw(cover)
    for box, color in (((0, 0, 159, 159), "#336a99"),
                       ((160, 0, 319, 159), "#b0444d"),
                       ((0, 160, 159, 319), "#384f6a"),
                       ((160, 160, 319, 319), "#d29746")):
        draw.rectangle(box, fill=color)
    cover.save(root / "cover.png")
    (root / "Track.lrc").write_text("[00:00.00]First retained lyric\n[00:03.00]Second retained lyric\n")


def prepare(root, assets, codec, theme):
    root.mkdir()
    with zipfile.ZipFile(assets) as archive:
        for name in archive.namelist():
            if name.startswith(".rockbox/fonts/") and not name.endswith("/"):
                archive.extract(name, root)
    rockbox = root / ".rockbox"
    codecs = rockbox / "codecs"
    codecs.mkdir()
    (codecs / "wav.codec").write_bytes(codec.read_bytes())
    (rockbox / "config.cfg").write_text(
        "start in screen: files\nshow files: music\n"
        "font: 18-noto-sans\nbacklight timeout: off\nvoice menus: off\n"
        "animations: off\nalbum art: prefer image file\n"
        f"native theme: {theme}\n"
    )
    if theme == "blur":
        themes = rockbox / "native-themes"
        themes.mkdir()
        (themes / "blur.podtheme").write_text("podbox-theme=1\nbackground.wps=blurred_cover\n")
    audio_fixture(root)


def capture(binary, root, env, out, theme):
    def save(name, image):
        image.save(out / f"{theme}-{name}.png")
        return image

    with (out / f"{theme}-firmware.log").open("w") as log, process(
        [str(binary), "--root", str(root), "--nobackground", "--zoom", "1", "--screenshot"],
        log, env=env, cwd=root,
    ) as firmware:
        time.sleep(115 / FPS)
        screen = Screen(env, firmware)

        def key(action, code):
            subprocess.run(["xdotool", action, "--window", screen.window, code],
                           env=env, check=True)

        def tap(code):
            key("keydown", code)
            time.sleep(2 / FPS)
            key("keyup", code)

        def wait(predicate, label, timeout=4 * FPS):
            for _ in range(timeout):
                assert firmware.poll() is None, label
                image = screen.frame()
                if predicate(image):
                    return image
                time.sleep(1 / FPS)
            save(f"failed-{label}", screen.frame())
            raise AssertionError(label)

        tap("Return")
        wps = save("playing", wait(
            lambda frame: max(frame.getpixel(COVER_POINT)) > 70 and frame.getpixel((70, 142)) != (0, 0, 0),
            "cover and progress"))
        outside = wps.getpixel((22, 225))
        if theme == "blur":
            assert max(outside) > 15
            return {"outside_cover": outside}

        assert outside == (0, 0, 0)
        r, g, b = wps.getpixel(COVER_POINT)
        assert r > g > b and r - b > 60, (r, g, b)

        save("spectrum", wait(lambda frame: frame.crop((0, 201, 79, 235)).getbbox(),
                              "live spectrum"))
        scroll = save("scroll", wait(
            lambda frame: ImageChops.difference(wps.crop(TITLE), frame.crop(TITLE)).getbbox(),
            "long title scroll"))
        tap("space")
        paused = save("pause", wait(
            lambda frame: ImageChops.difference(scroll.crop(TRANSPORT),
                                                frame.crop(TRANSPORT)).getbbox(),
            "pause transport"))
        key("keydown", "Right")
        seeking = save("seek", wait(
            lambda frame: ImageChops.difference(paused.crop(TRANSPORT),
                                                frame.crop(TRANSPORT)).getbbox()
                       and ImageChops.difference(paused.crop(PROGRESS),
                                                frame.crop(PROGRESS)).getbbox(),
            "seek preview"))
        key("keyup", "Right")
        time.sleep(17 / FPS)
        tap("Down")
        volume = save("volume", wait(
            lambda frame: ImageChops.difference(seeking.crop(VOLUME), frame.crop(VOLUME)).getbbox()
                       and frame.getpixel((41, 16)) != seeking.getpixel((41, 16)),
            "volume overlay"))
        assert volume.getpixel(COVER_POINT) == wps.getpixel(COVER_POINT)
        tap("Return")
        wait(lambda frame: frame.getpixel(COVER_POINT) != wps.getpixel(COVER_POINT),
             "lyrics launch")
        time.sleep(26 / FPS)
        save("lyrics", screen.frame())
        tap("Escape")
        save("lyrics-return", wait(lambda frame: frame.getpixel(COVER_POINT) ==
                                                      wps.getpixel(COVER_POINT), "lyrics return"))
        return {"cover": wps.getpixel(COVER_POINT), "scroll": True, "spectrum": True,
                "seek_preview": True, "volume_overlay": True, "lyrics": True}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for key in ("simulator", "resources", "codec", "output"):
        parser.add_argument(key, type=lambda x: Path(x).resolve())
    args = parser.parse_args()
    args.output.mkdir(parents=True)
    result = {}
    with tempfile.TemporaryDirectory(prefix="podbox-wps-cutover-") as directory, \
         tempfile.TemporaryFile() as display, (args.output / "xvfb.log").open("w") as log:
        tmp = Path(directory)
        runtime = tmp / "runtime"
        runtime.mkdir(mode=0o700)
        with process(["Xvfb", "-displayfd", str(display.fileno()), "-screen", "0", "800x600x24",
                      "-nolisten", "tcp"], log, pass_fds=(display.fileno(),)) as server:
            for _ in range(FPS * 5):
                display.seek(0)
                number = display.read().decode().strip()
                if number:
                    break
                assert server.poll() is None
                time.sleep(1 / FPS)
            else:
                raise RuntimeError("Xvfb did not report its display")
            env = dict(os.environ, DISPLAY=":" + number, XDG_RUNTIME_DIR=str(runtime),
                       SDL_VIDEODRIVER="x11", SDL_AUDIODRIVER="dummy",
                       SDL_RENDER_DRIVER="software", LIBGL_ALWAYS_SOFTWARE="1",
                       __GLX_VENDOR_LIBRARY_NAME="mesa")
            for theme in ("scrim", "blur"):
                root = tmp / theme
                prepare(root, args.resources, args.codec, theme)
                result[theme] = capture(args.simulator, root, env, args.output, theme)
    (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    main()
