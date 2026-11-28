#!/usr/bin/env python3
"""The USB Connected screen in each built-in theme, from a headless simulator.

Arguments: ROCKBOXUI SIMDISK OUTPUT_DIRECTORY [THEME...] [--language LANGUAGE]
SIMDISK is a simulator root with .rockbox in it, as build-sim.sh leaves; it
is copied, not changed. THEME defaults to all four built-in themes.
LANGUAGE is a bundled language name such as english or chinese-simp.

The simulator runs with SDL's dummy video driver: no window opens. For each
theme it plugs USB in, accepts Disk Mode and saves the screen as
usb-<theme>.png. Classic and both Fruit Company themes must show the stock disk-mode
screen: Background_LightBlue_Image behind it, the black status bar at the
top, the orange sphere and white text. Scrim must not.

Writes result.json beside the screens and exits non-zero on a failure.
"""

import argparse
import json
import mmap
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
BACKGROUND = (ROOT / "apps-ipod/bitmaps/source/stock" /
              "Background_LightBlue_Image.png")
STOCK_THEMES = ("classic", "fruit-company", "fruit-company-ultra")
W, H, SLOTS = 320, 240, 8
PIX = W * H * 3
HEADER = struct.Struct("=III")
WORD = struct.Struct("=I")
FPS = 60

# Points clear of the pictures, the text and the status bar.
BACKDROP_POINTS = [(20, 40), (300, 40), (20, 120), (300, 120), (40, 228),
                   (280, 228), (160, 160), (5, 235)]
SPHERE_POINT = (130, 100)       # orange, left of the connector
BAR_POINT = (100, 10)           # the status bar, left of the title


class Sim:
    def __init__(self, rockboxui, root, out):
        self.statefile = out / "state.bin"
        self.fh = self.statefile.open("w+b")
        size = HEADER.size + SLOTS * (WORD.size + PIX)
        self.fh.truncate(size)
        self.state = mmap.mmap(self.fh.fileno(), size, access=mmap.ACCESS_READ)
        env = dict(os.environ, SDL_VIDEODRIVER="dummy",
                   SDL_AUDIODRIVER="dummy", SDL_RENDER_DRIVER="software",
                   PODBOX_UI_WALK_STATE=str(self.statefile))
        env.pop("DISPLAY", None)
        env.pop("WAYLAND_DISPLAY", None)
        self.log = (out / "firmware.log").open("wb")
        self.proc = subprocess.Popen(
            [str(rockboxui), "--root", ".", "--nobackground", "--zoom", "1",
             "--screenshot", "--keys", "-"],
            cwd=root, env=env, stdin=subprocess.PIPE, stdout=self.log,
            stderr=subprocess.STDOUT, bufsize=0)

    def frame(self):
        _, frames, _ = HEADER.unpack_from(self.state)
        if not frames:
            return None
        off = HEADER.size + ((frames - 1) % SLOTS) * (WORD.size + PIX)
        px = bytes(self.state[off + WORD.size:off + WORD.size + PIX])
        return Image.frombytes("RGB", (W, H), px)

    def send(self, line):
        self.proc.stdin.write((line + "\n").encode())
        self.proc.stdin.flush()

    def wait(self, frames):
        time.sleep(frames / FPS)
        if self.proc.poll() is not None:
            raise RuntimeError("simulator exited")

    def tap(self, key):
        self.send("down " + key)
        self.wait(4)
        self.send("up " + key)

    def close(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(4)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        self.state.close()
        self.fh.close()
        self.log.close()


def prepare(simdisk, root, theme, language):
    shutil.copytree(simdisk, root)
    cfg = root / ".rockbox/config.cfg"
    if not cfg.exists():
        cfg = root / ".rockbox/default-config.cfg"
    lines = cfg.read_text().splitlines() if cfg.exists() else []
    cfg = root / ".rockbox/config.cfg"
    lines = [line for line in lines if not line.startswith("native theme:")]
    lines += ["native theme: " + theme, "idle poweroff: off",
              "sleep timer on startup: off"]
    if language:
        lines = [line for line in lines if not line.startswith("lang:")]
        lines.append(f"lang: /.rockbox/langs/{language}.lng")
    cfg.write_text("\n".join(lines) + "\n")


def capture(rockboxui, simdisk, out, theme, language):
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp) / "disk"
        prepare(simdisk, root, theme, language)
        work = out / ("run-" + theme)
        work.mkdir(parents=True, exist_ok=True)
        sim = Sim(rockboxui, root, work)
        try:
            sim.wait(FPS * 4)
            sim.tap("usb")              # plug in: the Disk Mode question
            sim.wait(FPS)
            sim.tap("select")           # its first choice connects
            sim.wait(FPS * 2)
            frame = sim.frame()
            sim.tap("usb")              # unplug
            sim.wait(FPS)
        finally:
            sim.close()
    if frame is None:
        raise RuntimeError(theme + ": no frame")
    frame.save(out / ("usb-%s.png" % theme))
    return frame


def to565(rgb):
    r, g, b = rgb[:3]
    return (r >> 3, g >> 2, b >> 3)


def near(a, b, slack=1):
    return all(abs(x - y) <= slack for x, y in zip(to565(a), to565(b)))


def check(theme, frame, backdrop):
    backdrop_ok = all(near(frame.getpixel(p), backdrop.getpixel(p))
                      for p in BACKDROP_POINTS)
    r, g, b = frame.getpixel(SPHERE_POINT)
    sphere_ok = r > 200 and 60 < g < 200 and b < 60
    bar = frame.getpixel(BAR_POINT)
    bar_ok = max(bar) < 60 and max(bar) < min(backdrop.getpixel(BAR_POINT))
    if theme in STOCK_THEMES:
        ok = backdrop_ok and sphere_ok and bar_ok
    else:
        ok = not backdrop_ok and not sphere_ok
    return {"ok": ok, "backdrop": backdrop_ok, "sphere": sphere_ok,
            "bar": bar_ok}


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("rockboxui", type=Path)
    parser.add_argument("simdisk", type=Path)
    parser.add_argument("output_directory", type=Path)
    parser.add_argument("themes", nargs="*")
    parser.add_argument("--language", help="bundled language name")
    args = parser.parse_intermixed_args()
    rockboxui = args.rockboxui.resolve()
    simdisk = args.simdisk.resolve()
    out = args.output_directory.resolve()
    themes = args.themes or (*STOCK_THEMES, "scrim")
    if args.language:
        languages = {p.stem for p in (simdisk / ".rockbox/langs").glob("*.lng")}
        if args.language not in languages:
            parser.error(f"language {args.language!r} is not bundled in {simdisk}")
    out.mkdir(parents=True, exist_ok=True)
    backdrop = Image.open(BACKGROUND).convert("RGB")
    result = {}
    for theme in themes:
        frame = capture(rockboxui, simdisk, out, theme, args.language)
        result[theme] = check(theme, frame, backdrop)
        print(theme, result[theme])
    (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    sys.exit(0 if all(r["ok"] for r in result.values()) else 1)


if __name__ == "__main__":
    main()
