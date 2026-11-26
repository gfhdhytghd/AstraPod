#!/usr/bin/env python3
"""Check the list's letter jump and its overlay in SDL.

A folder list of 1040 names, 40 to a letter, is spun slowly, then fast
until the letter jump starts, then a detent at a time, then left alone.
Under Classic the overlay is stock's tile; under Scrim and Light a square
twice the letter face's line height. Checks that the jump starts, that every
frame while it runs shows the overlay at one size, that letters follow one
another with no frame between them, and that the overlay goes with the
finger. The simulator's wheel has no touch sensor: a notch counts as a touch
for half a second.

Arguments: ROCKBOXUI ZIP OUTPUT_DIRECTORY
Xvfb is private to this process. Writes a frame strip per theme and
result.json to the output directory.
"""

import argparse
import json
import os
import subprocess
import tempfile
import time
from pathlib import Path
from zipfile import ZipFile

from ui_list_snap_sim import Recorder, click, strip
from ui_menu_sim import FPS, Screen, process

LETTERS = 26
PER_LETTER = 40
# Stock's tile is 74 x 70, centred on the list.
TILE = (123, 74, 70)


def lum(frame, x, y):
    r, g, b = frame.getpixel((x, y))
    return (r * 3 + g * 6 + b) / 10


def edge(frame, outer, inner, span):
    """The share of `span` along which the line `outer` is brighter than its
    neighbour `inner`; each is a function from position along it to pixel.
    Text gives a step on some rows only, the border on most."""
    return sum(lum(frame, *outer(t)) - lum(frame, *inner(t)) > 4 for t in span) / len(span)


def overlay_box(frame, theme, list_box):
    """The overlay's bounding box, centred on the list, or None. Scrim's and
    Light's square has a one-pixel border brighter than the dimmed inside:
    each side is where that step runs unbroken, away from the letter. Stock's
    tile is a grey box over a light list."""
    x0, y0, x1, y1 = list_box
    cx, cy = (x0 + x1) // 2, (y0 + y1) // 2
    if theme != "classic":
        rows, cols = range(cy - 20, cy + 20, 2), range(cx - 20, cx + 20, 2)

        def best(scores):
            """The edge nearest the centre; the list outside a dimmed
            square can be brighter than its border, a step of its own."""
            near = [value for score, value in scores if score >= 0.7]
            return near[-1] if near else None

        left = best((edge(frame, lambda y: (x, y), lambda y: (x + 1, y), rows), x)
                    for x in range(x0 + 1, cx - 30))
        right = best((edge(frame, lambda y: (x, y), lambda y: (x - 1, y), rows), x)
                     for x in range(x1 - 2, cx + 30, -1))
        top = best((edge(frame, lambda x: (x, y), lambda x: (x, y + 1), cols), y)
                   for y in range(y0 + 1, cy - 30))
        bottom = best((edge(frame, lambda x: (x, y), lambda x: (x, y - 1), cols), y)
                      for y in range(y1 - 2, cy + 30, -1))
        if None in (left, right, top, bottom) or \
                abs(left + right - 2 * cx) > 4 or abs(top + bottom - 2 * cy) > 8:
            return None
        return (left, top, right - left + 1, bottom - top + 1)

    def grey(x, y):
        r, g, b = frame.getpixel((x, y))
        return max(r, g, b) - min(r, g, b) < 12 and max(r, g, b) < 150

    probe = cx - 28
    if not grey(probe, cy):
        return None
    left = probe
    while left > x0 and grey(left - 1, cy):
        left -= 1
    right = probe
    while right < x1 - 1 and grey(right + 1, cy):
        right += 1
    top = cy
    while top > y0 and grey(probe, top - 1):
        top -= 1
    bottom = cy
    while bottom < y1 - 1 and grey(probe, bottom + 1):
        bottom += 1
    return (left, top, right - left + 1, bottom - top + 1)


def run(binary, resource, root, theme, output, env):
    root.mkdir()
    with ZipFile(resource) as archive:
        for path in archive.namelist():
            if path.startswith((".rockbox/fonts/", ".rockbox/themes/")):
                archive.extract(path, root)
    rockbox = root / ".rockbox"
    (rockbox / "config.cfg").write_text(
        "start in screen: files\nshow files: supported\nlist wraparound: off\n"
        "backlight timeout: off\nvoice menus: off\nkeyclick: off\n"
        "animations: on\nsettings mode: everything\nbeep: off\n"
        f"native theme: {theme}\n")
    for g in range(LETTERS):
        for i in range(PER_LETTER):
            (root / f"{chr(65 + g)}{chr(97 + g)} {i:03d}").mkdir()

    with (output / f"{theme}-sim.log").open("w") as log, process(
        [str(binary), "--root", str(root), "--nobackground", "--zoom", "1",
         "--screenshot"], log, env=env, cwd=root,
    ) as firmware:
        screen = Screen(env, firmware)
        time.sleep(240 / FPS)
        x, y, _, _ = screen.bbox
        subprocess.run(["xdotool", "mousemove", str(x + 120), str(y + 120)],
                       env=env, check=True)
        time.sleep(30 / FPS)
        with Recorder(screen) as rec:
            click(screen, 4, 5, 250)        # slow
            click(screen, 170, 5, 10)       # fast, into the jump
            click(screen, 6, 5, 200)        # a letter a detent
            time.sleep(1.2)                 # finger off
        assert firmware.poll() is None
    return rec.frames, rec.stamps


def check(theme, frames, stamps, output):
    list_box = (0, 40, 320, 240)
    boxes = [overlay_box(f, theme, list_box) for f in frames]
    shown = [i for i, b in enumerate(boxes) if b]
    strip(frames[::max(1, len(frames) // 48)], output / f"{theme}-all.png")
    strip(frames[-16:], output / f"{theme}-end.png")
    assert shown, f"{theme}: the letter jump never started"
    first, last = shown[0], shown[-1]
    # No frame inside the jump goes without the overlay.
    gaps = [i for i in range(first, last + 1) if not boxes[i]]
    if gaps:
        strip([frames[i] for i in gaps[:16]], output / f"{theme}-gaps.png")
    assert not gaps, f"{theme}: frames {gaps} lost the overlay"
    sizes = {b[2:] for b in boxes[first:last + 1]}
    assert len(sizes) == 1, f"{theme}: overlay sizes {sizes}"
    width, height = sizes.pop()
    if theme == "classic":
        assert (width, height) == TILE[1:], (width, height)
    else:
        assert abs(width - height) <= 1, (width, height)
    assert boxes[-1] is None, f"{theme}: the overlay stayed after release"
    picks = sorted(set([0, first - 1, first, first + 1, *range(first, last + 1, 4),
                        last, last + 1, len(frames) - 1]))
    strip([frames[i] for i in picks if 0 <= i < len(frames)],
          output / f"{theme}-quickscroll.png")
    # Rows whose glyphs are still loading draw nothing; count those frames.
    blank = sum(1 for f in frames[first:last + 1]
                if len({f.getpixel((x, y)) for x in range(4, 60, 3)
                        for y in range(list_box[1] + 4, list_box[3] - 4, 3)}) <= 2)
    return {"frames": len(frames), "jump_frames": last - first + 1,
            "blank_row_frames": blank,
            "jump_started_f": stamps[first], "overlay_wh": [width, height],
            "overlay_xy": list(boxes[first][:2])}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("binary", "resource", "output"):
        parser.add_argument(name, type=lambda arg: Path(arg).resolve())
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    result = {}
    with tempfile.TemporaryDirectory(prefix="podbox-quickscroll-") as folder, \
            tempfile.TemporaryFile() as display, \
            (args.output / "xvfb.log").open("w") as log:
        root = Path(folder)
        runtime = root / "runtime"
        runtime.mkdir(mode=0o700)
        with process(["Xvfb", "-displayfd", str(display.fileno()), "-screen", "0",
                      "800x600x24", "-nolisten", "tcp"], log,
                     pass_fds=(display.fileno(),)):
            for _ in range(120):
                display.seek(0)
                number = display.read().decode().strip()
                if number:
                    break
                time.sleep(1 / FPS)
            else:
                raise RuntimeError("Xvfb did not start")
            env = dict(os.environ, DISPLAY=":" + number, XDG_RUNTIME_DIR=str(runtime),
                       SDL_VIDEODRIVER="x11", SDL_AUDIODRIVER="dummy",
                       SDL_RENDER_DRIVER="software", LIBGL_ALWAYS_SOFTWARE="1")
            for theme in ("classic", "scrim", "light"):
                frames, stamps = run(args.binary, args.resource, root / theme,
                                     theme, args.output, env)
                try:
                    result[theme] = check(theme, frames, stamps, args.output)
                except AssertionError as error:
                    result[theme] = {"error": str(error)}
    (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    failed = {t: r["error"] for t, r in result.items() if "error" in r}
    assert not failed, failed
    print("PASS letter jump: " + ", ".join(
        f"{t} {r['overlay_wh'][0]}x{r['overlay_wh'][1]} over {r['jump_frames']} frames"
        for t, r in result.items()))


if __name__ == "__main__":
    main()
