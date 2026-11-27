#!/usr/bin/env python3
"""Capture native menu navigation with fonts as the only UI assets."""
import argparse
import json
import os
from pathlib import Path
import tempfile
import time
from zipfile import ZipFile

from PIL import ImageChops

from ui_menu_sim import FPS, Screen, image_diff, process


def capture(binary, resources, root, display, runtime, output, theme, home, legacy=False):
    with ZipFile(resources) as archive:
        for path in archive.namelist():
            if path.startswith(".rockbox/fonts/") or (
                legacy and path.startswith((".rockbox/themes/", ".rockbox/wps/",
                                            ".rockbox/icons/", ".rockbox/backdrops/"))
            ):
                archive.extract(path, root)

    rockbox = root / ".rockbox"
    config = (rockbox / "themes/scrim.cfg").read_text() if legacy else ""
    if home:
        config += ("\nstart in screen: root\nlist wraparound: on\n"
                   "root menu order: settings,system_menu,wps,database,pictureflow,"
                   "playlists,files,currentplaylist,books,shortcuts\n")
    else:
        config += "\nstart in screen: menu\n"
    config += ("backlight timeout: off\nvoice menus: off\nanimations: off\n"
               "settings mode: everything\nbeep: moderate\nscrollbar: off\n")
    config += f"native theme: {theme}\n"
    (rockbox / "config.cfg").write_text(config)
    env = dict(os.environ, DISPLAY=display, XDG_RUNTIME_DIR=str(runtime),
               SDL_VIDEODRIVER="x11", SDL_AUDIODRIVER="dummy", SDL_RENDER_DRIVER="software",
               LIBGL_ALWAYS_SOFTWARE="1", __GLX_VENDOR_LIBRARY_NAME="mesa")
    with (output / f"{root.name}.log").open("w") as log, process(
        [str(binary), "--root", str(root), "--nobackground", "--zoom", "1", "--screenshot"],
        log, env=env, cwd=root,
    ) as firmware:
        time.sleep(120 / FPS)
        screen = Screen(env, firmware)

        def step(key):
            screen.tap(key)
            time.sleep(15 / FPS)
            assert firmware.poll() is None
            return screen.frame()

        if theme == "scrim":
            screen.wait_full_charge()
        start, end = ("home", "settings") if home else ("settings", "playback")
        initial = screen.frame() if home else step("Down")
        entered = step("Right")
        wheel = step("Down")
        returned = step("Left")
        assert ImageChops.difference(initial, entered).getbbox()
        assert ImageChops.difference(entered, wheel).getbbox()
        assert ImageChops.difference(wheel, returned).getbbox()

        frames = {start: initial, end: entered, "wheel": wheel, "returned": returned}
        for name, frame in frames.items():
            frame.save(output / f"{root.name}-{name}.png")
        return frames


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("old", "new", "resources", "out"):
        parser.add_argument(name, type=lambda arg: Path(arg).resolve())
    parser.add_argument("--home", action="store_true")
    args = parser.parse_args()
    args.out.mkdir(parents=True)
    with tempfile.TemporaryDirectory(prefix="podbox-native-menu-") as folder, \
            tempfile.TemporaryFile() as display, (args.out / "xvfb.log").open("w") as log:
        root = Path(folder)
        runtime = root / "runtime"
        runtime.mkdir(mode=0o700)
        with process(["Xvfb", "-displayfd", str(display.fileno()), "-screen", "0",
                      "800x600x24", "-nolisten", "tcp"], log,
                     pass_fds=(display.fileno(),)):
            for _ in range(300):
                display.seek(0)
                number = display.read().decode().strip()
                if number:
                    break
                time.sleep(1 / FPS)
            else:
                raise RuntimeError("Xvfb did not start")

            display_name = ":" + number
            legacy = capture(args.old, args.resources, root / "legacy", display_name,
                             runtime, args.out, "scrim", args.home, legacy=True)
            native = capture(args.new, args.resources, root / "scrim", display_name,
                             runtime, args.out, "scrim", args.home)
    pixels = {name: image_diff(legacy[name], frame, f"scrim-{name}", args.out)
              for name, frame in native.items()}
    result = {"scrim_pixels_changed": pixels,
              "no_legacy_assets": True, "captures": list(native)}
    (args.out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print("PASS native menu entry, wheel selection and return with Scrim")


if __name__ == "__main__":
    main()
