#!/usr/bin/env python3
"""Compare Scrim frames and check menu motion in two real SDL firmware builds.

Arguments: OLD_ROCKBOXUI NEW_ROCKBOXUI RESOURCE_ZIP EVIDENCE_DIRECTORY
Xvfb is private to this process. Only the regular simulated click-wheel is used.
"""
import argparse
from contextlib import contextmanager
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
import zipfile

from PIL import ImageChops, ImageGrab

FPS = 60
SIZE = (320, 240)


def invoke(env, *args):
    return subprocess.check_output(args, env=env, text=True).strip()


@contextmanager
def process(command, log, **kwargs):
    proc = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, **kwargs)
    try:
        yield proc
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=4)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()


class Screen:
    def __init__(self, env, build):
        self.env = env
        for _ in range(FPS * 5):
            result = subprocess.run(["xdotool", "search", "--onlyvisible", "--name",
                                     "iPod 6G"], env=env, capture_output=True, text=True)
            if result.returncode == 0:
                self.window = result.stdout.splitlines()[-1]
                break
            assert build.poll() is None
            time.sleep(1 / FPS)
        else:
            raise RuntimeError("No simulator window")

        geom = dict(line.split("=", 1) for line in
                    invoke(env, "xdotool", "getwindowgeometry", "--shell",
                           self.window).splitlines() if "=" in line)
        assert (int(geom["WIDTH"]), int(geom["HEIGHT"])) == SIZE
        x, y = int(geom["X"]), int(geom["Y"])
        self.bbox = (x, y, x + SIZE[0], y + SIZE[1])

    def key(self, kind, key):
        invoke(self.env, "xdotool", kind, "--window", self.window, key)

    def frame(self):
        return ImageGrab.grab(bbox=self.bbox, xdisplay=self.env["DISPLAY"]).convert("RGB")

    def tap(self, key):
        self.key("keydown", key)
        time.sleep(3 / FPS)
        self.key("keyup", key)

    def wait_full_charge(self):
        # The 6G simulator starts plugged in. Scrim uses different glyphs
        # for 99% and 100%, even in screenshot mode.
        for _ in range(180):
            frame = self.frame()
            if (frame.getpixel((295, 9)) == (206, 207, 206) and
                    frame.getpixel((300, 15)) == frame.getpixel((306, 13)) == (0, 0, 0)):
                return
            time.sleep(3 / FPS)
        raise RuntimeError("Scrim screenshot battery did not reach 100% charging")

    def sequence(self, actions, output=None):
        frames = []
        stamps = []
        start = time.monotonic()
        events = iter(actions)
        edge = next(events, None)
        for sample in range(122):
            sample_frame = sample / 2
            time.sleep(max(0, start + sample_frame / FPS - time.monotonic()))
            while edge and edge[0] <= sample_frame:
                self.key(edge[1], edge[2])
                edge = next(events, None)

            if output is None:
                continue
            frame = self.frame()
            if not frames or ImageChops.difference(frames[-1], frame).getbbox():
                frames.append(frame)
                stamps.append(round((time.monotonic() - start) * FPS, 2))

        if output is None:
            return None
        holds = [b - a for a, b in zip(stamps, stamps[1:])] + [9]
        frames[0].save(output, save_all=True, append_images=frames[1:],
                       duration=[round(hold * 1000 / FPS) for hold in holds], loop=0)
        return frames, stamps


def image_diff(old, new, name, out):
    diff = ImageChops.difference(old, new)
    red, green, blue = diff.split()
    identical = ImageChops.lighter(ImageChops.lighter(red, green), blue).histogram()[0]
    pixels = SIZE[0] * SIZE[1] - identical
    if pixels:
        diff.save(out / (name + "-diff.png"))
    return pixels


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("old", "new", "resources", "out"):
        parser.add_argument(name, type=lambda s: Path(s).resolve())
    parser.add_argument("--home", action="store_true", help="exercise Home and Settings")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    result = {}
    with tempfile.TemporaryDirectory(prefix="podbox-ui-sim-") as temp, \
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
            static = {}
            for name, binary in [("old", args.old), ("off", args.new), ("on", args.new)]:
                root = fixture / name
                with zipfile.ZipFile(args.resources) as archive:
                    for entry in archive.namelist():
                        if entry.startswith((".rockbox/fonts/", ".rockbox/themes/",
                                             ".rockbox/wps/", ".rockbox/icons/",
                                             ".rockbox/backdrops/")):
                            archive.extract(entry, root)
                src = root / ".rockbox"
                config = (src / "themes/scrim.cfg").read_text()
                startup = ("\nstart in screen: root\nroot menu order: settings,system_menu\n"
                           if args.home else "\nstart in screen: menu\n")
                (src / "config.cfg").write_text(config + startup +
                       "backlight timeout: off\nvoice menus: off\n"
                       f"animations: {'on' if name == 'on' else 'off'}\n")

                with (args.out / (name + ".log")).open("w") as slog, process(
                    [str(binary), "--root", str(root), "--nobackground", "--zoom", "1",
                     "--screenshot"],
                    slog, env=env, cwd=root,
                ) as firmware:
                    # SDL can recreate its window while loading settings.
                    time.sleep(120 / FPS)
                    s = Screen(env, firmware)
                    s.wait_full_charge()
                    if not args.home:
                        s.tap("Down")
                    time.sleep(12 / FPS)
                    initial = s.frame()
                    push = [(4, "keydown", "Right"), (7, "keyup", "Right")]
                    pop = [(4, "keydown", "Left"), (7, "keyup", "Left")]
                    reverse = push + [(11, "keydown", "Left"), (14, "keyup", "Left")]

                    def run_motion(motion, edges):
                        output = args.out / (motion + ".gif") if name == "on" else None
                        capture = s.sequence(edges, output)
                        if capture:
                            frames, stamps = capture
                            result[motion] = {"visually_distinct_frames": len(frames),
                                              "capture_frame_times": stamps}
                        return s.frame()

                    entered = run_motion("push", push)
                    assert ImageChops.difference(initial, entered).getbbox(), name

                    returned = run_motion("pop", pop)
                    reversed_ = run_motion("reverse", reverse)
                    endpoints = ("home", "settings") if args.home else ("settings", "playback")
                    static[name] = {endpoints[0]: initial, endpoints[1]: entered,
                                    "returned": returned, "reversed": reversed_}
                    for key, image in static[name].items():
                        image.save(args.out / f"{name}-{key}.png")
                    assert not ImageChops.difference(returned, reversed_).getbbox(), name
                    assert firmware.poll() is None
            result["pixel_diff"] = {
                name: {key: image_diff(static["old"][key], image, f"{name}-{key}", args.out)
                       for key, image in static[name].items()}
                for name in ("off", "on")
            }
    (args.out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    assert not any(result["pixel_diff"][mode][state] for mode in ("off", "on")
                   for state in result["pixel_diff"][mode]), result["pixel_diff"]
    assert all(result[name]["visually_distinct_frames"] >= 5
               for name in ("push", "pop", "reverse")), result
    print("PASS pixel-identical Scrim menu endpoints and three live transitions")


if __name__ == "__main__":
    main()
