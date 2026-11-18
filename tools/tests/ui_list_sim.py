#!/usr/bin/env python3
"""Check Scrim single-column wheel motion in baseline and candidate SDL builds."""
import argparse
import json
import os
from pathlib import Path
import tempfile
import time
import zipfile

from PIL import ImageChops
from ui_menu_sim import FPS, Screen, image_diff, process


def wheel(key, start=4):
    return [(start, "keydown", key), (start + 3, "keyup", key)]


def check_capture(old, off, new, out):
    states = list(old)
    if not all(ImageChops.difference(old[a], old[b]).getbbox()
               for a, b in [("initial", "selection"), ("before-scroll", "scroll"),
                            ("scroll", "reverse"), ("reverse", "burst")]):
        raise AssertionError("Fixture did not reach the intended file rows")
    return {mode: {state: image_diff(old[state], screenshots[state],
                                    f"{mode}-{state}", out) for state in states}
            for mode, screenshots in [("off", off), ("on", new)]}


def fixture(root, resources, mode, scrollbar):
    with zipfile.ZipFile(resources) as archive:
        for name in archive.namelist():
            if name.startswith((".rockbox/fonts/", ".rockbox/themes/",
                                ".rockbox/wps/", ".rockbox/icons/", ".rockbox/backdrops/")):
                archive.extract(name, root)
    rb = root / ".rockbox"
    config = (rb / "themes/scrim.cfg").read_text()
    (rb / "config.cfg").write_text(config + "\nstart in screen: files\n"
                                  "backlight timeout: off\nvoice menus: off\n"
                                  f"animations: {'on' if mode == 'on' else 'off'}\n")
    if scrollbar:
        with (rb / "wps/scrim.sbs").open("a") as skin:
            skin.write("\n%V(312,46,8,184,-)%Vf(ffffff)%Vb(000000)\n"
                       "%?LB<%LB(0,0,8,184,vertical)|>\n")
    for i in range(1, 25):
        folder = root / f"Folder {i:02d}"
        folder.mkdir()
        (folder / f"Track {i:02d}.mp3").touch()


def capture(screen, output, motion):
    endpoints = {"initial": screen.frame()}

    def measure(name, edges):
        frames, stamps = screen.sequence(edges, output / (name + ".gif"))
        rows = {frame.crop((10, 46, 310, 230)).tobytes() for frame in frames}
        chrome = {frame.crop((0, 0, 320, 35)).tobytes() for frame in frames}
        text = {frame.crop((57, 46, 310, 230)).tobytes() for frame in frames}
        bar = {frame.crop((312, 46, 320, 230)).tobytes() for frame in frames}
        motion[name] = {"distinct_list_frames": len(rows),
                        "distinct_chrome_frames": len(chrome),
                        "distinct_content_frames": len(text),
                        "distinct_scrollbar_frames": len(bar),
                        "capture_frame_times": stamps}
        endpoints[name] = screen.frame()

    measure("selection", wheel("Down"))
    for _ in range(3):
        screen.tap("Down")
        time.sleep(15 / FPS)
    endpoints["before-scroll"] = screen.frame()
    measure("scroll", wheel("Down"))
    measure("reverse", wheel("Down") + wheel("Up", 8) + wheel("Up", 12))
    measure("burst", [edge for i in range(9) for edge in wheel("Down", 4 + 4 * i)])
    measure("select-during-motion",
            wheel("Down") + [(6, "keydown", "Right"), (10, "keyup", "Right")])
    return endpoints


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("old", "new", "resources", "out"):
        parser.add_argument(name, type=lambda s: Path(s).resolve())
    parser.add_argument("--scrollbar", action="store_true",
                        help="exercise a visible scrollbar in a test-only Scrim fixture")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    static, motion = {}, {}
    with tempfile.TemporaryDirectory(prefix="podbox-list-sim-") as temp, \
            tempfile.TemporaryFile() as display, (args.out / "xvfb.log").open("w") as log:
        root = Path(temp)
        runtime = root / "runtime"
        runtime.mkdir(mode=0o700)
        with process(["Xvfb", "-displayfd", str(display.fileno()), "-screen", "0",
                      "800x600x24"], log, pass_fds=(display.fileno(),)) as xvfb:
            for _ in range(FPS * 5):
                display.seek(0)
                number = display.read().decode().strip()
                if number:
                    break
                assert xvfb.poll() is None
                time.sleep(1 / FPS)
            else:
                raise RuntimeError("Xvfb did not start")
            env = dict(os.environ, DISPLAY=":" + number, XDG_RUNTIME_DIR=str(runtime),
                       SDL_VIDEODRIVER="x11", SDL_AUDIODRIVER="dummy",
                       SDL_RENDER_DRIVER="software")
            for mode, binary in [("old", args.old), ("off", args.new), ("on", args.new)]:
                target = root / mode
                output = args.out / mode
                output.mkdir(exist_ok=True)
                fixture(target, args.resources, mode, args.scrollbar)
                command = [str(binary), "--root", str(target), "--nobackground",
                           "--zoom", "1", "--screenshot"]
                with (output / "sim.log").open("w") as slog, \
                        process(command, slog, env=env, cwd=target) as firmware:
                    time.sleep(120 / FPS)
                    screen = Screen(env, firmware)
                    screen.wait_full_charge()
                    motion[mode] = {}
                    static[mode] = capture(screen, output, motion[mode])
                    for state, image in static[mode].items():
                        image.save(output / (state + ".png"))
                    assert firmware.poll() is None

    diff = check_capture(static["old"], static["off"], static["on"], args.out)
    results = {"pixel_diff": diff, "motion": motion, "scrollbar_fixture": args.scrollbar}
    (args.out / "result.json").write_text(json.dumps(results, indent=2) + "\n")
    assert not any(count for states in diff.values() for count in states.values()), diff
    for name in ("selection", "scroll", "reverse"):
        assert (motion["on"][name]["distinct_list_frames"] >
                motion["old"][name]["distinct_list_frames"]), motion
    assert motion["on"]["scroll"]["distinct_content_frames"] > 2, motion
    if args.scrollbar:
        assert motion["on"]["scroll"]["distinct_scrollbar_frames"] > 2, motion
    assert ImageChops.difference(static["old"]["burst"],
                                 static["old"]["select-during-motion"]).getbbox()
    assert motion["on"]["scroll"]["distinct_chrome_frames"] == 1, motion
    print("PASS settled Scrim File Browser pixels and opt-in single-column wheel motion")


if __name__ == "__main__":
    main()
