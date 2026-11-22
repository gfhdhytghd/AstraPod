#!/usr/bin/env python3
"""Run the MPEGPlayer module against synthetic MPEG and SDL disk audio."""
import argparse
from array import array
from contextlib import contextmanager
import mmap
import os
from pathlib import Path
import struct
import subprocess
import time

from PIL import Image, ImageOps

LCD = (320, 240)
FRAME_BYTES = LCD[0] * LCD[1] * 3
STATE_BYTES = 12 + 8 * (4 + FRAME_BYTES)


def fixtures(path):
    path.mkdir(parents=True, exist_ok=True)
    good = path / "colors.mpg"
    subprocess.run([
        "ffmpeg", "-v", "error", "-y", "-filter_complex_threads", "1",
        "-f", "lavfi", "-i", "color=red:s=320x240:r=25:d=4",
        "-f", "lavfi", "-i", "color=blue:s=320x240:r=25:d=5",
        "-f", "lavfi", "-i", "color=green:s=320x240:r=25:d=5",
        "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=32000:duration=14",
        "-filter_complex", "[0:v][1:v][2:v]concat=n=3:v=1:a=0[v]",
        "-map", "[v]", "-map", "3:a", "-c:v", "mpeg2video", "-g", "12",
        "-b:v", "1600k", "-minrate", "1600k", "-maxrate", "1600k",
        "-bufsize", "320k", "-c:a", "mp2", "-b:a", "128k", "-ac", "2",
        "-f", "mpeg",
        "-threads", "1", str(good),
    ], check=True)
    (path / "invalid.mpg").write_bytes(b"not an MPEG stream\n")
    (path / "truncated.mpg").write_bytes(good.read_bytes()[:31])
    return good


class Sim:
    def __init__(self, proc, state, case):
        self.proc = proc
        self.state = state
        self.case = case

    def tap(self, key):
        for action in ("down", "up"):
            self.proc.stdin.write(f"{action} {key}\n".encode())
            self.proc.stdin.flush()
            time.sleep(0.16 if action == "down" else 0.30)

    def image(self):
        sequence, = struct.unpack_from("=I", self.state, 4)
        if not sequence:
            return None
        offset = 12 + ((sequence - 1) % 8) * (FRAME_BYTES + 4)
        data = self.state[offset + 4:offset + 4 + FRAME_BYTES]
        if struct.unpack_from("=I", self.state, offset)[0] != sequence:
            return None
        return Image.frombytes("RGB", LCD, data)

    def wait(self, label, check, timeout=15):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            assert self.proc.poll() is None, self.case / "firmware.log"
            image = self.image()
            if image is not None and check(image):
                image.save(self.case / f"{label}.png")
                return
            time.sleep(0.06)
        if image is not None:
            image.save(self.case / f"failed-{label}.png")
        raise AssertionError(f"{self.case}: {label}")

    def color(self, label, channel):
        def matches(image):
            r, g, b = image.getpixel((160, 120))
            return (r > 180 and g < 60 and b < 60 if channel == "red" else
                    b > 180 and g < 60 and r < 60)
        self.wait(label, matches)

    def text(self, label, *words):
        last = None

        def matches(image):
            nonlocal last
            data = image.tobytes()
            if data == last:
                return False
            last = data
            png = self.case / "ocr.png"
            gray = ImageOps.autocontrast(ImageOps.grayscale(image))
            if gray.resize((1, 1)).getpixel((0, 0)) < 127:
                gray = ImageOps.invert(gray)
            gray.resize((960, 720)).save(png)
            value = subprocess.check_output(
                ["tesseract", png, "stdout", "--psm", "11"],
                stderr=subprocess.DEVNULL, text=True).lower()
            return all(word in value for word in words)

        self.wait(label, matches)

    def launch(self, label):
        self.tap("select")
        self.text(f"{label}-picker", "fixture")
        self.tap("down")
        self.tap("select")

    def audio(self, first=0):
        path = self.case / "pcm.raw"
        if not path.exists():
            return first, False
        data = path.read_bytes()
        samples = array("h")
        samples.frombytes(data[first:len(data) & ~1])
        return len(data) & ~1, sum(abs(sample) > 128 for sample in samples) > 400


@contextmanager
def session(build, case, fixture):
    disk = case / "disk"
    rockbox = disk / ".rockbox"
    module = rockbox / "rocks/apps/mpegplayer.rock"
    module.parent.mkdir(parents=True)
    module.write_bytes((build / "rock-compat/modules/apps/mpegplayer.rock").read_bytes())
    (disk / "fixture.mpg").write_bytes(fixture.read_bytes())
    (rockbox / "config.cfg").write_text(
        "start in screen: root\nroot menu order: plugins,files,settings\n"
        "animations: off\nbacklight timeout: off\nvoice menus: off\n"
        "volume: -24\n"
    )
    state_path = case / "state.bin"
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="disk",
               SDL_DISKAUDIOFILE=str(case / "pcm.raw"),
               SDL_RENDER_DRIVER="software", PODBOX_UI_WALK_STATE=str(state_path))
    with state_path.open("w+b") as handle, (case / "firmware.log").open("wb") as log:
        handle.truncate(STATE_BYTES)
        with mmap.mmap(handle.fileno(), 0, access=mmap.ACCESS_READ) as state, \
                subprocess.Popen([
                    build / "rockboxui", "--root", ".", "--nobackground",
                    "--zoom", "1", "--screenshot", "--keys", "-",
                ], stdin=subprocess.PIPE, env=env, cwd=disk, stdout=log,
                    stderr=subprocess.STDOUT) as proc:
            try:
                sim = Sim(proc, state, case)
                sim.text("home", "plugins")
                time.sleep(0.4)
                sim.tap("select")
                sim.text("categories", "applications")
                sim.tap("select")
                sim.text("applications", "mpegpl")
                yield sim
            finally:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait(timeout=5)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=lambda p: Path(p).resolve())
    parser.add_argument("evidence", type=lambda p: Path(p).resolve())
    parser.add_argument("--fixtures-only", action="store_true")
    args = parser.parse_args()
    good = fixtures(args.evidence / "fixtures")
    if args.fixtures_only:
        return

    with session(args.build, args.evidence / "playback", good) as sim:
        sim.launch("play")
        sim.color("red", "red")
        first, _ = sim.audio()
        time.sleep(0.8)
        assert sim.audio(first)[1], "decoded MPEG audio was silent"

        sim.tap("play")
        time.sleep(0.25)
        first, _ = sim.audio()
        time.sleep(0.45)
        assert not sim.audio(first)[1], "decoded PCM continued while paused"
        sim.tap("right")
        sim.color("paused-seek", "blue")
        first, _ = sim.audio()
        sim.tap("play")
        time.sleep(0.65)
        assert sim.audio(first)[1], "decoded PCM did not resume after seek"

        sim.tap("menu")
        sim.text("menu-return", "mpegpl")
        first, _ = sim.audio()
        time.sleep(0.3)
        assert not sim.audio(first)[1], "decoded PCM survived module return"
        sim.launch("reopen")
        sim.color("reopened", "red")
        sim.tap("usb")
        sim.text("usb-consent", "computer", "connected")
        first, _ = sim.audio()
        time.sleep(0.3)
        assert not sim.audio(first)[1], "decoded PCM survived USB consent"
        sim.tap("menu")
        sim.text("charge-only-return", "mpegpl")
        sim.tap("usb")
        sim.launch("charge-reopen")
        sim.color("charge-reopened", "red")
        sim.tap("usb")
        sim.text("storage-consent", "computer", "connected")
        sim.tap("select")
        sim.wait("usb-storage", lambda _: "All threads have acknowledged the connect."
                 in (sim.case / "firmware.log").read_text())
        sim.tap("usb")
        sim.text("usb-return", "applications", "demos")
        sim.tap("select")
        sim.text("usb-applications", "mpegpl")
        sim.launch("after-usb")
        sim.color("after-usb", "red")
        first, _ = sim.audio()
        time.sleep(0.55)
        assert sim.audio(first)[1], "MPEG PCM did not recover after USB"
        sim.tap("menu")
        sim.text("final-return", "mpegpl")

    for name in ("invalid", "truncated"):
        with session(args.build, args.evidence / name,
                     good.with_name(f"{name}.mpg")) as sim:
            sim.launch(name)
            sim.text("returned", "mpegpl")
            assert not sim.audio()[1], "invalid input produced PCM"
    print("PASS MPEGPlayer SDL decode, seek, pause, return, USB, and rejection")


if __name__ == "__main__":
    main()
