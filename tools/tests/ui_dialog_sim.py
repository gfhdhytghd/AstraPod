#!/usr/bin/env python3
"""Compare Scrim confirmation frames and their SDL transitions."""
import argparse
import json
import os
from pathlib import Path
import tempfile
import time
import zipfile

from PIL import ImageChops

from ui_menu_sim import FPS, Screen, image_diff, process


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("old", "new", "resources", "out"):
        parser.add_argument(name, type=lambda entry: Path(entry).resolve())
    parser.add_argument("--kind", choices=("usb", "yesno"), default="usb")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    result = {"motion": {}, "pixel_diff": {}}
    states = {}

    with tempfile.TemporaryDirectory(prefix="podbox-dialog-") as temp, \
            tempfile.TemporaryFile() as display, (args.out / "xvfb.log").open("w") as log:
        fixture = Path(temp)
        runtime = fixture / "runtime"
        runtime.mkdir(mode=0o700)
        with process(["Xvfb", "-displayfd", str(display.fileno()), "-screen", "0",
                      "800x600x24"], log, pass_fds=(display.fileno(),)) as xv:
            for _ in range(FPS * 5):
                display.seek(0)
                number = display.read().decode().strip()
                if number:
                    break
                assert xv.poll() is None
                time.sleep(1 / FPS)
            else:
                raise RuntimeError("Xvfb did not start")

            env = dict(os.environ, DISPLAY=":" + number, XDG_RUNTIME_DIR=str(runtime),
                       SDL_VIDEODRIVER="x11", SDL_AUDIODRIVER="dummy",
                       SDL_RENDER_DRIVER="software")
            for name, binary in [("old", args.old), ("off", args.new), ("on", args.new)]:
                root = fixture / name
                with zipfile.ZipFile(args.resources) as archive:
                    for entry in archive.namelist():
                        if entry.startswith((".rockbox/fonts/", ".rockbox/themes/",
                                             ".rockbox/wps/", ".rockbox/icons/",
                                             ".rockbox/backdrops/")):
                            archive.extract(entry, root)
                assets = root / ".rockbox"
                config = (assets / "themes/scrim.cfg").read_text()
                (assets / "config.cfg").write_text(config + "\nstart in screen: menu\n"
                     "backlight timeout: off\nvoice menus: off\nusb mode: mass storage\n"
                     f"animations: {'on' if name == 'on' else 'off'}\n")

                with (args.out / f"{name}.log").open("w") as slog, process(
                    [str(binary), "--root", str(root), "--nobackground", "--zoom", "1",
                     "--screenshot"], slog, env=env, cwd=root,
                ) as firmware:
                    time.sleep(120 / FPS)
                    screen = Screen(env, firmware)
                    screen.wait_full_charge()

                    def capture(motion, edges):
                        output = args.out / f"{motion}.gif" if name == "on" else None
                        data = screen.sequence(edges, output)
                        if data:
                            frames, stamps = data
                            result["motion"][motion] = {"frames": len(frames),
                                                        "capture_times_f": stamps}
                        return screen.frame()

                    if args.kind == "yesno":
                        # Settings > Library > Maintenance.
                        for key in ("Down", "Down", "Right", "Up", "Right"):
                            screen.tap(key)
                            time.sleep(24 / FPS)
                        open_key, close_key = "Right", "space"
                    else:
                        open_key, close_key = "u", "Escape"
                    page = screen.frame()
                    confirmation = capture("open", [(4, "keydown", open_key),
                                                    (7, "keyup", open_key)])
                    assert ImageChops.difference(page, confirmation).getbbox(), name
                    # Both close keys are accepted on press. Hold the key
                    # past the animation so its release cannot interrupt it.
                    capture("close", [(4, "keydown", close_key),
                                      (26, "keyup", close_key)])
                    if args.kind == "yesno":
                        # The maintenance caller presents a timed Cancelled
                        # message before it returns to the page.
                        time.sleep(40 / FPS)
                    returned = screen.frame()
                    assert firmware.poll() is None
                    states[name] = dict(page=page, confirmation=confirmation, returned=returned)
                    for state, image in states[name].items():
                        image.save(args.out / f"{name}-{state}.png")

            for mode in ("off", "on"):
                result["pixel_diff"][mode] = {
                    state: image_diff(states["old"][state], image, f"{mode}-{state}", args.out)
                    for state, image in states[mode].items()
                }
    (args.out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    assert all(not pixels for mode in result["pixel_diff"].values()
               for pixels in mode.values()), result["pixel_diff"]
    assert all(step["frames"] >= 4 for step in result["motion"].values()), result["motion"]
    print(f"PASS pixel-identical Scrim {args.kind} confirmation and two live dialog transitions")


if __name__ == "__main__":
    main()
