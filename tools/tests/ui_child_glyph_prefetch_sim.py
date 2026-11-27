#!/usr/bin/env python3
"""Compare a CJK folder push with and without delayed glyph batches.

Arguments: ROCKBOXUI ZIP OUTPUT_DIRECTORY [--baseline ROCKBOXUI]
The child is opened after 850 ms of idle time. A private Xvfb receives input.
Panel records preserve the first frames even if the host misses a refresh.
"""

import argparse
import json
import os
from pathlib import Path
import tempfile
import time
from zipfile import ZipFile

from PIL import Image

from ui_list_hwscroll_sim import RECORD, RECORD_SIZE, click, tap
from ui_list_snap_sim import strip
from ui_menu_sim import Screen, invoke, process


IDLE_SEC = 0.85
DELAY_MS = 400


def captures(record, first):
    data = record.read_bytes()
    frames = []
    for i in range(first, len(data) // RECORD_SIZE):
        offset = i * RECORD_SIZE + RECORD.size
        frames.append(Image.frombytes(
            "RGB", (320, 240), data[offset:offset + 320 * 240 * 2],
            "raw", "BGR;16"))
    return frames


def push(binary, resource, root, output, env, delay):
    root.mkdir()
    with ZipFile(resource) as archive:
        for name in archive.namelist():
            if name.startswith(".rockbox/fonts/"):
                archive.extract(name, root)
    (root / ".rockbox/config.cfg").write_text(
        "start in screen: files\nstart directory: /Parent\n"
        "show files: supported\nlist wraparound: off\nbacklight timeout: off\n"
        "voice menus: off\nkeyclick: off\nanimations: on\n"
        "settings mode: everything\nbeep: off\nnative theme: light\n")
    for folder, prefix in (("A", "日月星辰"), ("B", "春夏秋冬")):
        for i in range(8):
            # Four distinct CJK glyphs at the start of each name. The ASCII
            # suffix gives deterministic ordering without another glyph batch.
            (root / "Parent" / folder / f"{prefix} {i:02d}").mkdir(
                parents=True)
    record = root / "panel.rec"
    with (output / f"{root.name}.log").open("w") as log, process(
        [str(binary), "--root", str(root), "--nobackground", "--zoom", "1",
         "--screenshot"], log, cwd=root,
        env=dict(env, PODBOX_PANEL_RECORD=str(record),
                 PODBOX_GLYPH_DELAY_MS=str(delay)),
    ) as firmware:
        time.sleep(3)
        screen = Screen(env, firmware)
        x, y, _, _ = screen.bbox
        invoke(env, "xdotool", "mousemove", str(x + 120), str(y + 120))
        click(screen, 1, 5, 0)
        time.sleep(0.25)
        tap(env, "Return")
        time.sleep(0.35)
        click(screen, 1, 5, 0)
        time.sleep(IDLE_SEC)
        start = record.stat().st_size // RECORD_SIZE
        screen.frame().save(output / f"{root.name}-parent.png")
        tap(env, "Return")
        time.sleep(1.25)
        frames = captures(record, start)
        assert firmware.poll() is None and frames
        strip(frames[:24], output / f"{root.name}.png")
        frames[-1].save(output / f"{root.name}-settled.png")
        return frames



def marker_x(image):
    # The incoming Light selection is blue. The parent selection is on
    # row two, so only the incoming row has blue at this height.
    for x in range(320):
        r, g, b = image.getpixel((x, 56))
        if b - r > 12 and b - g > 5:
            return x
    return None


def first_row_ink(frames):
    """Match the settled CJK glyph strokes at the first moving row's offset."""
    final = frames[-1]
    settled_x = marker_x(final)
    assert settled_x == 52, ("child did not settle on row one", settled_x)
    # Both the screen and the panel records use RGB565. Match dark stroke
    # interiors and allow the first frame's opacity to be as low as 1/6.
    ink = []
    for y in range(49, 73):
        bg = final.getpixel((200, y))[0]
        for dx in range(10, 80):
            if bg - final.getpixel((settled_x + dx, y))[0] > 100:
                ink.append((dx, y))
    assert len(ink) > 80, ("missing CJK reference", len(ink))
    for i, frame in enumerate(frames):
        x = marker_x(frame)
        if x is None or x <= settled_x + 4 or x > 265:
            continue
        points = [(x + dx, y) for dx, y in ink if x + dx < 319]
        # The background of the incoming band is uniform on each scanline.
        bg = {y: max(frame.getpixel((px, y))[0]
                     for px in range(x + 8, min(319, x + 105)))
              for _, y in points}
        matched = sum(bg[y] - frame.getpixel((px, y))[0] >= 8
                      for px, y in points)
        return {"frame": i, "marker_x": x, "matched": matched,
                "reference": len(points)}
    raise AssertionError("No incoming first row during the push")

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("binary", "resource", "output"):
        parser.add_argument(name, type=lambda arg: Path(arg).resolve())
    parser.add_argument("--baseline", type=lambda arg: Path(arg).resolve())
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="podbox-child-glyph-") as tmp, \
            tempfile.TemporaryFile() as display, \
            (args.output / "xvfb.log").open("w") as log:
        root = Path(tmp)
        runtime = root / "runtime"
        runtime.mkdir(mode=0o700)
        with process(["Xvfb", "-displayfd", str(display.fileno()), "-screen",
                      "0", "800x600x24", "-nolisten", "tcp"], log,
                     pass_fds=(display.fileno(),)):
            for _ in range(120):
                display.seek(0)
                number = display.read().decode().strip()
                if number:
                    break
                time.sleep(0.02)
            else:
                raise RuntimeError("Xvfb did not start")
            env = dict(os.environ, DISPLAY=":" + number,
                       XDG_RUNTIME_DIR=str(runtime), SDL_VIDEODRIVER="x11",
                       SDL_AUDIODRIVER="dummy", SDL_RENDER_DRIVER="software")
            env.pop("WAYLAND_DISPLAY", None)
            results = {}
            runs = [("after", args.binary)]
            if args.baseline:
                runs.insert(0, ("before", args.baseline))
            for name, binary in runs:
                frames = push(binary, args.resource, root / name,
                              args.output, env, DELAY_MS)
                results[name] = first_row_ink(frames)

    (args.output / "result.json").write_text(json.dumps(results, indent=2) + "\n")
    if args.baseline:
        before = results["before"]
        assert before["matched"] * 5 < before["reference"], before
    after = results["after"]
    assert after["matched"] * 4 >= after["reference"] * 3, after
    print("PASS CJK first push frame after 850 ms idle with 400 ms glyph delay")


if __name__ == "__main__":
    main()
