#!/usr/bin/env python3
"""Shared harness for the Fruit Company Ultra recordings, in SDL.

Each Ultra check runs one key script twice in a simulator with no window,
once under Fruit Company and once under Fruit Company Ultra, records every
distinct frame the panel received with its time, and measures the pacing
of the Ultra motion. It writes, per animation, both themes' frames, a
contact sheet with Fruit Company on the left and Ultra on the right, and
a side-by-side MP4 at 60 fps for review.

Import it from a check in tools/tests:

    from ultra_sim import compare, pacing, press, record, write_review

`compare()` returns {theme: {name: shots}}; a shot is (ms, PIL image).
"""

import shutil
import subprocess
import sys
from pathlib import Path

from PIL import Image, ImageDraw

sys.dont_write_bytecode = True
from ui_stock_transitions_sim import (  # noqa: E402
    FPS, H, W, Sim, difference, make_fixture, session)

FRUIT, ULTRA = "fruit-company", "fruit-company-ultra"
THEMES = (FRUIT, ULTRA)
# The panel refreshes at about 67 Hz and full-screen motion presents every
# second refresh; the simulator's clock runs the same pacing.
REFRESH_MS = 1000 / 66.96
# A frame interval longer than this inside a motion is a visible hitch:
# more than one presentation missed at the half rate.
HITCH_MS = 4.5 * REFRESH_MS

__all__ = ["FPS", "H", "W", "Sim", "THEMES", "FRUIT", "ULTRA", "compare",
           "difference", "make_fixture", "pacing", "press", "record",
           "session", "write_review", "perf_routes"]


def press(key):
    """A record() trigger that taps KEY."""
    return key


def record(sim, frames, key, **kwargs):
    """Sim.record with the same arguments, for symmetry with press()."""
    return sim.record(frames, key, **kwargs)


def compare(binary, fixture, out, extra, script, themes=THEMES, prepare=None):
    """Run SCRIPT under each theme. SCRIPT(sim, theme) returns
    {name: shots}. Each theme's ui-perf.txt is kept beside its frames."""
    results = {}
    for theme in themes:
        folder = out / theme

        def perf_prepare(root, prepare=prepare):
            (root / ".rockbox/ui-perf.enable").touch()
            if prepare:
                prepare(root)

        def run(sim, theme=theme, folder=folder):
            try:
                return script(sim, theme)
            finally:
                perf = sim.root / ".rockbox/ui-perf.txt"
                if perf.exists():
                    shutil.copyfile(perf, folder / "ui-perf.txt")
        results[theme] = session(binary, fixture, folder, theme, extra, run,
                                 perf_prepare)
    return results


def perf_routes(folder, route=None):
    """The summary lines of FOLDER/ui-perf.txt as dicts, optionally only
    ROUTE's."""
    path = Path(folder) / "ui-perf.txt"
    if not path.exists():
        return []
    out = []
    for line in path.read_text().splitlines():
        if not line.startswith("route="):
            continue
        fields = dict(f.split("=", 1) for f in line.split() if "=" in f)
        if route is None or fields.get("route") == route:
            out.append(fields)
    return out


def motion_span(shots, settle_ms=150, box=None):
    """The shots from the first that differs from the one before the key
    to the last change, ignoring a lone change SETTLE_MS after the rest.
    BOX limits the comparison to a region, to leave out an ambient motion
    such as the menu pane's pan."""
    if len(shots) < 2:
        return shots
    first = next((i for i in range(1, len(shots))
                  if difference(shots[i][1], shots[0][1], box) > 0), None)
    if first is None:
        return shots[:1]
    last = first
    for i in range(first + 1, len(shots)):
        if shots[i][0] - shots[last][0] > settle_ms:
            break
        if box is None or difference(shots[i][1], shots[i - 1][1], box) > 0:
            last = i
    return shots[first - 1:last + 1]


def pacing(shots, settle_ms=150, box=None):
    """Frame intervals of the motion in SHOTS, in ms."""
    span = motion_span(shots, settle_ms, box)
    gaps = [b[0] - a[0] for a, b in zip(span, span[1:])]
    if not gaps:
        return {"frames": len(span), "duration_ms": 0, "p50_ms": 0,
                "p95_ms": 0, "max_ms": 0, "hitches": 0}
    ordered = sorted(gaps)
    return {"frames": len(span),
            "duration_ms": round(span[-1][0] - span[0][0], 1),
            "p50_ms": round(ordered[len(ordered) // 2], 1),
            "p95_ms": round(ordered[min(len(ordered) - 1,
                                        len(ordered) * 95 // 100)], 1),
            "max_ms": round(ordered[-1], 1),
            "hitches": sum(g > HITCH_MS for g in gaps)}


def _label(image, text):
    draw = ImageDraw.Draw(image)
    draw.rectangle((0, 0, len(text) * 6 + 6, 12), fill=(0, 0, 0))
    draw.text((3, 1), text, fill=(255, 255, 0))
    return image


def _resample(shots, fps=FPS, tail_ms=250):
    """SHOTS as a constant-rate sequence, each frame held until the next."""
    if not shots:
        return []
    start, end = shots[0][0], shots[-1][0] + tail_ms
    frames, j = [], 0
    t = start
    while t <= end:
        while j + 1 < len(shots) and shots[j + 1][0] <= t:
            j += 1
        frames.append(shots[j][1])
        t += 1000 / fps
    return frames


def contact_sheet(fruit, ultra, path, columns=6, limit=36, box=None):
    """Fruit Company's motion frames left, Ultra's right, each labelled
    with its time from the key."""
    halves = []
    for name, shots in (("Fruit Company", fruit), ("Ultra", ultra)):
        span = motion_span(shots, box=box) if shots else []
        if len(span) > limit:
            step = len(span) / limit
            span = [span[int(i * step)] for i in range(limit)]
        rows = max(1, (len(span) + columns - 1) // columns)
        thumb_w, thumb_h = W // 2, H // 2
        sheet = Image.new("RGB", (columns * thumb_w, rows * thumb_h + 14),
                          (40, 40, 40))
        _label(sheet, name)
        t0 = shots[0][0] if shots else 0
        for i, (ms, image) in enumerate(span):
            thumb = _label(image.resize((thumb_w, thumb_h)),
                           f"{ms - t0:.0f}ms")
            sheet.paste(thumb, ((i % columns) * thumb_w,
                                14 + (i // columns) * thumb_h))
        halves.append(sheet)
    out = Image.new("RGB", (halves[0].width + halves[1].width + 8,
                            max(h.height for h in halves)), (0, 0, 0))
    out.paste(halves[0], (0, 0))
    out.paste(halves[1], (halves[0].width + 8, 0))
    out.save(path)


def clip(fruit, ultra, path, scale=2):
    """Side-by-side MP4 at 60 fps, Fruit Company left and Ultra right,
    each from its own key press."""
    a, b = _resample(fruit), _resample(ultra)
    count = max(len(a), len(b))
    if not count:
        return
    a += [a[-1]] * (count - len(a)) if a else []
    b += [b[-1]] * (count - len(b)) if b else []
    size = (2 * W * scale + 8 * scale, H * scale)
    process = subprocess.Popen(
        ["ffmpeg", "-loglevel", "error", "-y", "-f", "rawvideo",
         "-pix_fmt", "rgb24", "-s", f"{size[0]}x{size[1]}", "-r", str(FPS),
         "-i", "-", "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "16",
         str(path)], stdin=subprocess.PIPE)
    for left, right in zip(a, b):
        frame = Image.new("RGB", size, (0, 0, 0))
        frame.paste(_label(left.resize((W * scale, H * scale),
                                       Image.NEAREST), "Fruit Company"),
                    (0, 0))
        frame.paste(_label(right.resize((W * scale, H * scale),
                                        Image.NEAREST), "Ultra"),
                    (W * scale + 8 * scale, 0))
        process.stdin.write(frame.tobytes())
    process.stdin.close()
    process.wait()


def write_review(results, review, prefix="", boxes=None):
    """Contact sheets and clips of every animation both themes recorded,
    into REVIEW. BOXES maps an animation to the region its motion is
    measured in. Returns {name: {theme: pacing}}."""
    boxes = boxes or {}
    review = Path(review)
    review.mkdir(parents=True, exist_ok=True)
    report = {}
    fruit, ultra = results.get(FRUIT, {}), results.get(ULTRA, {})
    for name in sorted(set(fruit) | set(ultra)):
        a, b = fruit.get(name, []), ultra.get(name, [])
        stem = review / f"{prefix}{name}"
        box = boxes.get(name)
        contact_sheet(a, b, stem.with_suffix(".png"), box=box)
        clip(a, b, stem.with_suffix(".mp4"))
        report[name] = {FRUIT: pacing(a, box=box) if a else None,
                        ULTRA: pacing(b, box=box) if b else None}
    return report
