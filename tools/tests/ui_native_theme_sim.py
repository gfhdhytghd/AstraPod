#!/usr/bin/env python3
"""Verify theme selection and invalid-file recovery without restarting SDL."""
import argparse
import json
import os
from pathlib import Path
import tempfile
import time
from zipfile import ZipFile

from PIL import ImageChops

from ui_menu_sim import FPS, Screen, process


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for key in ("simulator", "resources", "evidence"):
        parser.add_argument(key, type=lambda v: Path(v).resolve())
    parser.add_argument("--locale", choices=("chinese-simp", "chinese-trad"))
    parser.add_argument("--lang-dir", type=Path)
    args = parser.parse_args()
    if args.locale and not args.lang_dir:
        parser.error("--locale requires --lang-dir with simulator language files")
    args.evidence.mkdir(parents=True)
    with tempfile.TemporaryDirectory(prefix="native-theme-") as folder, \
            tempfile.TemporaryFile() as display, (args.evidence / "xvfb.log").open("w") as log:
        root = Path(folder)
        runtime = root / "runtime"
        runtime.mkdir(mode=0o700)
        with ZipFile(args.resources) as z:
            for name in z.namelist():
                if name.startswith(".rockbox/fonts/"):
                    z.extract(name, root)
        rockbox = root / ".rockbox"
        options = ""
        if args.locale:
            name = args.locale + ".lng"
            (rockbox / "langs").mkdir()
            (rockbox / "langs" / name).write_bytes((args.lang_dir / name).read_bytes())
            options += f"\nlang: {args.locale}\n"
        (rockbox / "config.cfg").write_text(options + "\nstart in screen: menu\n"
            "backlight timeout: off\nvoice menus: off\nlist wraparound: on\n"
            "animations: off\nnative theme: scrim\n")
        (rockbox / "native-themes").mkdir()
        path = rockbox / "native-themes/fixture.podtheme"
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
            env = dict(os.environ, DISPLAY=":" + number, XDG_RUNTIME_DIR=str(runtime),
                       SDL_VIDEODRIVER="x11", SDL_AUDIODRIVER="dummy",
                       SDL_RENDER_DRIVER="software", LIBGL_ALWAYS_SOFTWARE="1",
                       __GLX_VENDOR_LIBRARY_NAME="mesa")
            with (args.evidence / "firmware.log").open("w") as slog, process(
                [str(args.simulator), "--root", str(root), "--nobackground",
                 "--zoom", "1", "--screenshot"], slog, env=env, cwd=root,
            ) as firmware:
                time.sleep(120 / FPS)
                device = Screen(env, firmware)
                device.wait_full_charge()
                counter = 0

                def snap(name):
                    image = device.frame()
                    image.save(args.evidence / (name + ".png"))
                    return image

                def click(key):
                    device.tap(key)
                    time.sleep(18 / FPS)

                def choose(index, expected, first=False):
                    nonlocal counter
                    counter += 1
                    if first:
                        for _ in range(3):
                            click("Down")
                    click("Right")  # Appearance from Settings
                    snap(f"{counter}-appearance")
                    click("Right")  # Native theme is the first row
                    snap(f"{counter}-selector")
                    for _ in range(index):
                        click("Down")
                    click("Right")
                    click("Left")  # Settings
                    image = snap(f"{counter}-settings")
                    assert image.getpixel((0, 230)) == expected, (counter, image.getpixel((0, 230)))
                    return image

                black = (0, 0, 0)
                # Built-in Light, followed by an external theme and a rejected file.
                before = snap("scrim")
                light = choose(1, (247, 247, 247), first=True)
                assert ImageChops.difference(before, light).getbbox()
                path.write_text("podbox-theme=1\ncolor.background=204060\n")
                custom = choose(2, (33, 65, 99))
                assert ImageChops.difference(light, custom).getbbox()
                path.write_text("podbox-theme=1\nmetric.row_height=99999\n")
                scrim = choose(2, black)
                assert ImageChops.difference(custom, scrim).getbbox()
                assert "Native theme rejected, using Scrim (fixture)" in (
                    rockbox / "native-theme.log").read_text()
                result = {"same_process": firmware.poll() is None,
                          "light": light.getpixel((0, 230)),
                          "custom": custom.getpixel((0, 230)),
                          "invalid_reverted_to_scrim": scrim.getpixel((0, 230)) == black}
    (args.evidence / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print("PASS runtime native Light, custom theme, and invalid-file Scrim fallback")


if __name__ == "__main__":
    main()
