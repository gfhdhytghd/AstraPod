#!/usr/bin/env python3
"""Check retained Settings and Playback Settings against legacy Scrim.

Pass OLD NEW PHASE4_ZIP EVIDENCE. Both executables are normal iPod 6G SDL
builds. The no-skin fixture has no .wps or .sbs files to fall back to.
"""
import argparse
import json
import os
from pathlib import Path
import tempfile
import time
import zipfile

from PIL import ImageChops

from ui_menu_sim import FPS, Screen, image_diff, process


def capture(binary, resources, root, display, runtime, output, mode, no_skin=False):
    with zipfile.ZipFile(resources) as archive:
        for path in archive.namelist():
            if path.startswith((".rockbox/fonts/", ".rockbox/themes/", ".rockbox/wps/",
                                ".rockbox/icons/", ".rockbox/backdrops/")):
                if no_skin and path.startswith(".rockbox/wps/"):
                    continue
                archive.extract(path, root)
    rockbox = root / ".rockbox"
    config = (rockbox / "themes/scrim.cfg").read_text()
    config += "\nstart in screen: menu\nbacklight timeout: off\nvoice menus: off\n"
    config += f"animations: off\nnative ui: {mode}\nsettings mode: everything\nbeep: moderate\n"
    if root.name == "light":
        config += "native theme: light\n"
    (rockbox / "config.cfg").write_text(config)
    env = dict(os.environ, DISPLAY=display, XDG_RUNTIME_DIR=str(runtime),
               SDL_VIDEODRIVER="x11", SDL_AUDIODRIVER="dummy", SDL_RENDER_DRIVER="software")
    with (output / f"{root.name}.log").open("w") as slog, process(
        [str(binary), "--root", str(root), "--nobackground", "--zoom", "1", "--screenshot"],
        slog, env=env, cwd=root,
    ) as firmware:
        time.sleep(120 / FPS)
        screen = Screen(env, firmware)
        if root.name != "light":
            screen.wait_full_charge()

        def tap(key):
            screen.tap(key)
            time.sleep(15 / FPS)

        tap("Down")
        frames = {"settings": screen.frame()}
        tap("Right")
        frames["playback"] = screen.frame()
        assert ImageChops.difference(frames["settings"], frames["playback"]).getbbox()
        for number in range(22):
            tap("Down")
            frames[f"playback-{number}"] = screen.frame()
        tap("Left")
        frames["returned"] = screen.frame()
        assert ImageChops.difference(frames["returned"], frames["playback"]).getbbox()
        for key, frame in frames.items():
            frame.save(output / f"{root.name}-{key}.png")
        return frames


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("old", "new", "resources", "out"):
        parser.add_argument(name, type=lambda arg: Path(arg).resolve())
    args = parser.parse_args()
    args.out.mkdir(parents=True)
    with tempfile.TemporaryDirectory(prefix="podbox-native-menu-") as folder, \
            tempfile.TemporaryFile() as display, (args.out / "xvfb.log").open("w") as xlog:
        root = Path(folder)
        runtime = root / "runtime"
        runtime.mkdir(mode=0o700)
        with process(["Xvfb", "-displayfd", str(display.fileno()), "-screen", "0",
                      "800x600x24", "-nolisten", "tcp"], xlog,
                     pass_fds=(display.fileno(),)):
            for _ in range(300):
                display.seek(0)
                number = display.read().decode().strip()
                if number:
                    break
                time.sleep(1 / FPS)
            else:
                raise RuntimeError("Xvfb did not start")
            captures = {
                name: capture(binary, args.resources, root / name, ":" + number,
                              runtime, args.out, mode, no_skin)
                for name, binary, mode, no_skin in (
                    ("legacy", args.old, "off", False),
                    ("disabled", args.new, "off", False),
                    ("native", args.new, "on", False),
                    ("no-skin", args.new, "on", True),
                    ("light", args.new, "on", True),
                )
            }
    result = {
        name: {
            state: image_diff(captures["legacy"][state], frame, f"{name}-{state}", args.out)
            for state, frame in frames.items()
        }
        for name, frames in captures.items() if name not in ("legacy", "light")
    }
    (args.out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    assert all(not value for frames in result.values() for value in frames.values()), result
    for state in ("settings", "playback", "returned"):
        frame = captures["light"][state]
        assert frame.getpixel((0, 0)) == (222, 223, 214), state
        assert frame.getpixel((0, 239)) == (247, 247, 247), state
    print("PASS native Settings and Playback Settings with Scrim and Light, including no-skin mode")


if __name__ == "__main__":
    main()
