#!/usr/bin/env python3
"""Photos against stock 2.0.5's reference captures, pixel by pixel.

    python3 tools/tests/photos_stock_compare_sim.py build-sim-ipod6g \\
        CAPTURES EVIDENCE_DIR [--theme classic]

CAPTURES is the reference set (capture-205, with its MANIFEST.md). The
reference library -- p001-p100 at 640 x 480 and the aspect tests a1-a8,
drawn as its generator draws them -- is copied as loose files into folders
"Aspect Test", "Thirty Three" (p001-p033) and "Weekly" (p034-p100), so All
Photos holds them in stock's order. With animations on, the script records
every frame of:

  - the Photos menu, and its pane panning;
  - Thirty Three opening (placeholders, the title bar sliding down) and
    MENU leaving it (the list sliding in);
  - Aspect Test's grid, SELECT into a1 (the push), each aspect test full
    screen, and MENU back (the push back);

and writes EVIDENCE_DIR/<theme>/compare.json: for each state the RGB565
pixels that differ from the reference frame, and the motion it measured.
Frames go to the same directory.
"""
import argparse
import colorsys
import json
import os
from pathlib import Path
import shutil
import sys
import time

import numpy as np
from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, str(Path(__file__).resolve().parent))
from photos_loose_sim import session, open_photos  # noqa: E402
from videos_sim import player  # noqa: E402

FONT = "/usr/share/fonts/liberation/LiberationSans-Regular.ttf"
ASPECTS = [("a1_4x3", 640, 480), ("a2_3x4", 480, 640),
           ("a3_1x1", 600, 600), ("a4_pano", 1200, 300),
           ("a5_tall", 300, 1200), ("a6_16x9", 1280, 720),
           ("a7_small", 160, 120), ("a8_320x240", 320, 240)]
FS_NAMES = ["fs-a1-4x3", "fs-a2-3x4", "fs-a3-1x1", "fs-a4-pano",
            "fs-a5-tall", "fs-a6-16x9", "fs-a7-160x120", "fs-a8-320x240"]


def font(size):
    return ImageFont.truetype(FONT, size)


def library(disk):
    """The reference library's pictures, with its dates"""
    base = time.mktime((2026, 3, 1, 12, 0, 0, 0, 0, -1))
    for i in range(1, 101):
        r, g, b = [int(255 * c) for c in
                   colorsys.hsv_to_rgb((i * 0.137) % 1.0, 0.6, 0.8)]
        image = Image.new("RGB", (640, 480), (r, g, b))
        draw = ImageDraw.Draw(image)
        draw.rectangle((40, 40, 599, 439), outline=(255, 255, 255), width=4)
        draw.text((320, 240), "%03d" % i, font=font(150),
                  fill=(255, 255, 255), anchor="mm")
        folder = disk / ("Thirty Three" if i <= 33 else "Weekly")
        folder.mkdir(parents=True, exist_ok=True)
        path = folder / ("p%03d.jpg" % i)
        image.save(path, quality=92)
        t = base + ((i - 1) // 10) * 86400 * 7
        os.utime(path, (t, t))
    for name, w, h in ASPECTS:
        image = Image.new("RGB", (w, h), (40, 80, 160))
        draw = ImageDraw.Draw(image)
        for x in range(0, w, 40):
            for y in range(0, h, 40):
                if (x // 40 + y // 40) % 2:
                    draw.rectangle((x, y, x + 39, y + 39),
                                   fill=(60, 110, 200))
        draw.rectangle((0, 0, w - 1, h - 1), outline=(255, 0, 0),
                       width=max(2, w // 160))
        m = max(10, w // 32)
        draw.rectangle((m, m, w - 1 - m, h - 1 - m), outline=(255, 255, 0),
                       width=max(1, w // 320))
        draw.line((w // 2, 0, w // 2, h), fill=(0, 255, 0),
                  width=max(1, w // 320))
        draw.line((0, h // 2, w, h // 2), fill=(0, 255, 0),
                  width=max(1, w // 320))
        draw.text((w // 2, h // 4), name, font=font(max(12, min(w, h) // 6)),
                  fill=(255, 255, 255), anchor="mm")
        folder = disk / "Aspect Test"
        folder.mkdir(parents=True, exist_ok=True)
        path = folder / (name + ".jpg")
        image.save(path, quality=92)
        t = base - 86400 * 30
        os.utime(path, (t, t))


def rgb565(image):
    a = np.asarray(image.convert("RGB")).astype(np.int32)
    return (a[..., 0] >> 3) << 11 | (a[..., 1] >> 2) << 5 | (a[..., 2] >> 3)


def differ(ours, stock, box=(0, 0, 320, 240)):
    x0, y0, x1, y1 = box
    a, b = rgb565(ours)[y0:y1, x0:x1], rgb565(stock)[y0:y1, x0:x1]
    return int((a != b).sum())


def record(sim, seconds, until=None):
    """Every frame for `seconds`: (time, image)"""
    frames, last = [], sim.frame()[0]
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        sequence, pixels = sim.frame()
        if sequence != last and pixels is not None:
            frames.append((time.monotonic(),
                           Image.frombytes("RGB", (320, 240), pixels)))
            last = sequence
        time.sleep(0.001)
    return frames


def shift_of(frame, final, rows, limit):
    """How far `frame`'s `rows` sit from `final`'s, by best match"""
    a = rgb565(frame)[rows[0]:rows[1]]
    b = rgb565(final)[rows[0]:rows[1]]
    best = None
    for k in range(limit + 1):
        if k:
            d = float((a[:, k:] != b[:, :-k]).mean())
        else:
            d = float((a != b).mean())
        if best is None or d < best[0]:
            best = (d, k)
    return best[1]


def title_hidden(frame, final):
    """Rows of the final title bar still above the top"""
    a, b = rgb565(frame)[:24], rgb565(final)[:24]
    best = None
    for k in range(25):
        d = float((a[:24 - k] != b[k:]).mean()) if k < 24 else 1.0
        if best is None or d < best[0]:
            best = (d, k)
    return best[1]


def panel_edge(frame):
    """The menu list's right edge: its last white column, plus one"""
    row = np.asarray(frame.convert("RGB"))[230]
    white = [x for x in range(320) if (row[x] >= 250).all()]
    return max(white) + 1 if white else 0


def steady(sim, quiet, box=(0, 0, 160, 240), timeout=20):
    """Wait until `box` has not changed for `quiet` seconds: the menu's pane
    pans for as long as the menu is up"""
    x0, y0, x1, y1 = box
    crop = lambda: np.asarray(sim.image())[y0:y1, x0:x1].tobytes()
    deadline = time.monotonic() + timeout
    last, since = crop(), time.monotonic()
    while time.monotonic() < deadline:
        time.sleep(0.05)
        now = crop()
        if now != last:
            last, since = now, time.monotonic()
        elif time.monotonic() - since >= quiet:
            return
    raise AssertionError("the list never settled")


def pane_shift(frame, final):
    """How far the pane's picture past its shadow moved between frames, by
    best match: positive when it moved left"""
    a = rgb565(frame)[30:230, 186:320]
    b = rgb565(final)[30:230, 186:320]
    best = None
    for k in range(-40, 41):
        if k > 0:
            d = float((a[:, k:] != b[:, :-k]).mean())
        elif k < 0:
            d = float((a[:, :k] != b[:, -k:]).mean())
        else:
            d = float((a != b).mean())
        if best is None or d < best[0]:
            best = (d, k)
    return best[1]


def save_frames(frames, case, label):
    folder = case / label
    folder.mkdir(exist_ok=True)
    t0 = frames[0][0] if frames else 0
    for i, (t, image) in enumerate(frames):
        image.save(folder / f"f{i:03d}_{(t - t0) * 1000:06.0f}ms.png")


def pane_window(pane, renditions):
    """Which rendition, at which window column, the pane shows past its
    shadow, and how many pixels of that differ"""
    p = rgb565(pane)[:, 186:320]
    best = None
    for name, image in renditions.items():
        r = rgb565(image)
        for x in range(0, 161):
            d = int((r[:, x + 26:x + 160] != p).sum())
            if best is None or d < best[2]:
                best = (name, x, d)
    return best


def run(build, captures, evidence, theme):
    case = evidence / theme
    if case.exists():
        shutil.rmtree(case)
    case.mkdir(parents=True)
    disk = case / "disk"
    disk.mkdir()
    player(build, disk, theme)
    with (disk / ".rockbox/config.cfg").open("a") as cfg:
        cfg.write("root menu order: images,files,settings\n"
                  "animations: on\n")
    library(disk)
    stock = lambda name: Image.open(captures / f"{name}.png")
    entry_ref = sorted((captures / "series-grid-entry/png").glob("*.png"))
    out = {"theme": theme, "states": {}, "motion": {}}
    states, motion = out["states"], out["motion"]

    def compare(label, image, ref, box=(0, 0, 320, 240)):
        image.save(case / f"{label}.png")
        states[label] = {"reference": str(ref.relative_to(captures)) if
                         isinstance(ref, Path) else ref,
                         "differing": differ(image, Image.open(ref)
                                             if isinstance(ref, Path) else
                                             stock(ref), box),
                         "box": box}

    with session(build, case, disk, theme) as sim:
        open_photos(sim, theme)
        steady(sim, 1.5)
        menu = sim.save("menu")
        compare("menu-list", menu, "pm-all-photos", (0, 0, 160, 240))
        # the pane over three seconds: its pan speed and direction
        pane = record(sim, 3.0)
        if len(pane) > 2:
            first, last = pane[0], pane[-1]
            px = pane_shift(first[1], last[1])
            motion["pane_px_per_s"] = round(px / (last[0] - first[0]), 2)

        # Thirty Three: entry and exit
        for _ in range(3):
            sim.tap("down")
        steady(sim, 1.5)
        thirty = sim.save("menu-thirty-three")
        compare("menu-thirty-three-list", thirty, "pm-album-thirty-three",
                (0, 0, 160, 240))
        sim.quick("select", 0.05)
        frames = record(sim, 1.5)
        save_frames(frames, case, "grid-entry")
        settled = frames[-1][1]
        compare("grid-entry-final", settled, entry_ref[9])
        grey = rgb565(Image.new("RGB", (1, 1), (33, 32, 33)))[0, 0]
        empty = [f for t, f in frames if (rgb565(f)[:24] == grey).all() and
                 (rgb565(f)[24:46] == rgb565(settled)[24:46]).all()]
        if empty:
            compare("grid-entry-placeholders", empty[0], entry_ref[2])
        motion["title_hidden_rows"] = [title_hidden(f, settled)
                                       for t, f in frames]
        sim.quick("menu", 0.05)
        frames = record(sim, 1.0)
        save_frames(frames, case, "grid-exit")
        motion["menu_panel_edges"] = [panel_edge(f) for t, f in frames]
        mids = [f for t, f in frames if 0 < panel_edge(f) < 160]
        if mids:
            mids[len(mids) // 2].save(case / "grid-exit-mid.png")

        # Aspect Test: the grid, the push into a1, each aspect, the return
        sim.tap("up")
        steady(sim, 1.5)
        sim.quick("select", 0.05)
        time.sleep(1.5)
        sim.settle(1.0)
        compare("grid-aspect", sim.image(), "grid-aspect")
        grid = sim.image()
        sim.quick("select", 0.05)
        frames = record(sim, 1.2)
        save_frames(frames, case, "push-in")
        motion["push_in_offsets"] = [shift_of(grid, f, (60, 230), 320)
                                     for t, f in frames]
        sim.settle(0.8)
        renditions = {}
        for i, name in enumerate(FS_NAMES):
            if i:
                sim.tap("right")
                sim.settle(0.8)
            image = sim.image()
            renditions[name] = image
            compare(name, image, name)
        sim.quick("menu", 0.05)
        frames = record(sim, 1.2)
        save_frames(frames, case, "push-back")
        sim.settle(1.0)
        back = sim.image()
        compare("grid-aspect-return", back, "grid-aspect-return")
        motion["push_back_offsets"] = [320 - shift_of(back, f, (60, 230),
                                                      320)
                                       for t, f in frames]
        # the pane on the Aspect Test row shows one of its renditions 1:1
        sim.quick("menu", 0.05)
        steady(sim, 2.0)
        pane_shot = sim.save("menu-aspect")
        compare("menu-aspect-list", pane_shot, "pm-album-aspect",
                (0, 0, 160, 240))
        name, x, d = pane_window(pane_shot, renditions)
        motion["aspect_pane"] = {"rendition": name, "window_x": x,
                                 "differing_past_shadow": d}
        sim.quick("select", 0.05)
        time.sleep(1.5)
        sim.settle(1.0)
        compare("grid-aspect-reopen", sim.image(), "grid-aspect-reopen")

    (case / "compare.json").write_text(json.dumps(out, indent=2))
    shutil.rmtree(disk, ignore_errors=True)
    return out


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("build", type=Path)
    parser.add_argument("captures", type=Path)
    parser.add_argument("evidence", type=Path)
    parser.add_argument("--theme", default="classic")
    args = parser.parse_args()
    out = run(args.build.resolve(), args.captures.resolve(),
              args.evidence.resolve(), args.theme)
    print(json.dumps(out, indent=2))


if __name__ == "__main__":
    main()
