#!/usr/bin/env python3
"""Compare the local, no-art Scrim playing and paused WPS in actual SDL."""
import argparse
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import time
import wave
import zipfile

from PIL import ImageChops

from ui_menu_sim import FPS, Screen, image_diff, process


def add_wave_fixture(path):
    with wave.open(str(path), "wb") as audio:
        audio.setparams((1, 2, 8000, 0, "NONE", "not compressed"))
        audio.writeframes(bytes(8000 * 2 * 600))
    info = bytearray(b"INFO")
    for field, value in ((b"INAM", b"PodBox"), (b"IART", b"Test Ensemble")):
        value += b"\0"
        info.extend(struct.pack("<4sI", field, len(value)))
        info.extend(value)
        if len(value) % 2:
            info.append(0)
    with path.open("r+b") as wav:
        wav.seek(0, os.SEEK_END)
        wav.write(struct.pack("<4sI", b"LIST", len(info)) + info)
        size = wav.tell()
        wav.seek(4)
        wav.write(struct.pack("<I", size - 8))


def capture(binary, data, env, output):
    with output.with_suffix(".log").open("w") as log, process(
        [str(binary), "--root", str(data), "--nobackground", "--zoom", "1", "--screenshot"],
        log, env=env, cwd=data,
    ) as firmware:
        time.sleep(120 / FPS)
        screen = Screen(env, firmware)

        def tap(key):
            subprocess.run(
                ["xdotool", "keydown", "--window", screen.window, key, "sleep", str(2 / FPS),
                 "keyup", "--window", screen.window, key],
                env=env, check=True,
            )

        def wait_for_wps(transport=None):
            for _ in range(3 * FPS):
                assert firmware.poll() is None
                frame = screen.frame()
                if frame.getpixel((200, 142)) != (0, 0, 0) and (
                    transport is None or ImageChops.difference(
                        transport, frame.crop((0, 0, 30, 26))).getbbox()
                ):
                    return frame
                time.sleep(1 / FPS)
            raise AssertionError("WPS progress and transport did not settle")

        tap("Return")
        playing = wait_for_wps()
        tap("space")
        paused = wait_for_wps(playing.crop((0, 0, 30, 26)))
        playing.save(output.with_name(output.name + "-playing.png"))
        paused.save(output.with_name(output.name + "-paused.png"))
        return playing, paused


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("resources", type=Path)
    parser.add_argument("sim_codecs", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--capture-only", action="store_true")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)

    captures = {}
    with tempfile.TemporaryDirectory(prefix="native-wps-") as temp, \
         tempfile.TemporaryFile() as display, (args.output / "xvfb.log").open("w") as log:
        fixture = Path(temp)
        runtime = fixture / "runtime"
        runtime.mkdir(mode=0o700)
        with process(
            ["Xvfb", "-displayfd", str(display.fileno()), "-screen", "0", "800x600x24",
             "-nolisten", "tcp"],
            log, pass_fds=(display.fileno(),)
        ) as xvfb:
            for _ in range(FPS * 5):
                display.seek(0)
                number = display.read().decode().strip()
                if number:
                    break
                assert xvfb.poll() is None
                time.sleep(1 / FPS)
            else:
                raise RuntimeError("Xvfb did not report its display")
            env = dict(os.environ, DISPLAY=":" + number, XDG_RUNTIME_DIR=str(runtime),
                       SDL_VIDEODRIVER="x11", SDL_AUDIODRIVER="dummy",
                       SDL_RENDER_DRIVER="software", LIBGL_ALWAYS_SOFTWARE="1",
                       __GLX_VENDOR_LIBRARY_NAME="mesa")
            variants = (("old", args.baseline, False, "scrim"),)
            if not args.capture_only:
                variants += (("off", args.candidate, False, "scrim"),
                             ("native", args.candidate, True, "scrim"),
                             ("light", args.candidate, True, "light"))
            for mode, binary, native, theme in variants:
                root = fixture / mode
                with zipfile.ZipFile(args.sim_codecs) as archive:
                    archive.extract(".rockbox/codecs/wav.codec", root)
                with zipfile.ZipFile(args.resources) as archive:
                    for path in archive.namelist():
                        if path.startswith((".rockbox/fonts/", ".rockbox/themes/", ".rockbox/wps/",
                                            ".rockbox/icons/", ".rockbox/backdrops/")):
                            if native and path == ".rockbox/wps/scrim.wps":
                                continue
                            archive.extract(path, root)
                config = root / ".rockbox/config.cfg"
                config.write_text((root / ".rockbox/themes/scrim.cfg").read_text() +
                                  "\nstart in screen: files\nshow files: music\n"
                                  "backlight timeout: off\nvoice menus: off\n"
                                  "animations: off\nalbum art: off\n"
                                  f"native ui: {'on' if native else 'off'}\n"
                                  f"native theme: {theme}\n")
                add_wave_fixture(root / "Test.wav")
                captures[mode] = capture(binary.resolve(), root, env, args.output / mode)

    if not args.capture_only:
        result = {}
        for mode in ("off", "native"):
            result[mode] = {
                label: image_diff(old, new, f"{mode}-{label}", args.output)
                for label, old, new in zip(("playing", "paused"), captures["old"], captures[mode])
            }
        assert not any(count for state in result.values() for count in state.values()), result
        result["light"] = {}
        for label, image in zip(("playing", "paused"), captures["light"]):
            background = image.getpixel((319, 239))
            assert all(channel > 235 for channel in background), background
            result["light"][label] = {"background_rgb": background}
        (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
        print("PASS local Scrim native WPS parity, without the WPS skin file")


if __name__ == "__main__":
    main()
