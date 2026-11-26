#!/usr/bin/env python3
"""Check snapped list scrolling in SDL: Scrim's rows jump a whole row and
only the selection marker glides; Light matches an older build.

Arguments: NEW_ROCKBOXUI NEW_ZIP OLD_ROCKBOXUI OLD_ZIP OUTPUT_DIRECTORY
Xvfb is private to this process. Writes frame strips, result.json and each
run's ui-perf.txt to the output directory.
"""

import argparse
import json
import os
import subprocess
import tempfile
import threading
import time
from pathlib import Path
from zipfile import ZipFile

from PIL import Image, ImageChops

from ui_menu_sim import FPS, Screen, process


TOP = 46
PITCH = 30
VISIBLE = 6
BOTTOM = TOP + VISIBLE * PITCH
MARKER_X = 47
TEXT_X = (60, 290)
THUMB_X = 305
TRACK_COUNT = 20
SLOW_DETENT_MS = 400
SPIN_DETENT_MS = 60


class Recorder:
    """Grabs the window as fast as X allows and keeps each changed frame."""

    def __init__(self, screen):
        self.screen = screen
        self.frames, self.stamps = [], []
        self.running = False

    def __enter__(self):
        self.running = True
        self.start = time.monotonic()
        self.frames.append(self.screen.frame())
        self.stamps.append(0.0)
        self.thread = threading.Thread(target=self.loop)
        self.thread.start()
        return self

    def loop(self):
        while self.running:
            frame = self.screen.frame()
            if ImageChops.difference(self.frames[-1], frame).getbbox():
                self.frames.append(frame)
                self.stamps.append(round((time.monotonic() - self.start) * FPS, 2))

    def __exit__(self, *exc):
        self.running = False
        self.thread.join()


def click(screen, clicks, button, delay_ms):
    subprocess.run(["xdotool", "click", "--repeat", str(clicks), "--delay",
                    str(delay_ms), str(button)], env=screen.env, check=True)


def record(screen, clicks, button, delay_ms, settle_f=40):
    with Recorder(screen) as rec:
        click(screen, clicks, button, delay_ms)
        time.sleep(settle_f / FPS)
    return rec.frames, rec.stamps


def strip(frames, path, columns=8):
    """Every captured frame, left to right, in reading order."""
    columns = min(columns, len(frames))
    rows = (len(frames) + columns - 1) // columns
    sheet = Image.new("RGB", (columns * 322, rows * 242), (255, 0, 255))
    for i, frame in enumerate(frames):
        sheet.paste(frame, ((i % columns) * 322 + 1, (i // columns) * 242 + 1))
    sheet.save(path)


def span(frame, x, test):
    rows = [y for y in range(TOP, BOTTOM) if test(frame, x, y)]
    return [rows[0], rows[-1]] if rows else None


def text_phase(frame, background):
    """Where the first row of text ink sits, modulo the row pitch."""
    left, right = TEXT_X
    for y in range(TOP, BOTTOM):
        if any(frame.getpixel((x, y)) != background for x in range(left, right, 2)):
            return (y - TOP) % PITCH
    return None


def text_area(frame):
    return frame.crop((TEXT_X[0], TOP, TEXT_X[1], BOTTOM)).tobytes()


def analyse(frames, stamps, marker, background):
    samples = []
    for stamp, frame in zip(stamps, frames):
        samples.append({
            "at_f": stamp,
            "marker_px": span(frame, MARKER_X,
                              lambda im, x, y: im.getpixel((x, y)) == marker),
            "thumb_px": span(frame, THUMB_X,
                             lambda im, x, y: im.getpixel((x, y)) != background),
            "text_phase_px": text_phase(frame, background),
        })
    return samples


def run(binary, resource, root, theme, output, env, scenarios):
    root.mkdir()
    with ZipFile(resource) as archive:
        for path in archive.namelist():
            if path.startswith(".rockbox/fonts/"):
                archive.extract(path, root)
    rockbox = root / ".rockbox"
    (rockbox / "config.cfg").write_text(
        "start in screen: files\nshow files: supported\nlist wraparound: off\n"
        "backlight timeout: off\nvoice menus: off\nkeyclick: off\n"
        "animations: on\nsettings mode: everything\nbeep: off\n"
        "scrollbar: right\nscrollbar width: 8\n"
        f"native theme: {theme}\n")
    (rockbox / "ui-perf.enable").touch()
    for i in range(TRACK_COUNT):
        (root / f"Track {i + 1:02d}").mkdir()

    results = {}
    with (output / f"{root.name}-sim.log").open("w") as log, process(
        [str(binary), "--root", str(root), "--nobackground", "--zoom", "1",
         "--screenshot"], log, env=env, cwd=root,
    ) as firmware:
        time.sleep(120 / FPS)
        screen = Screen(env, firmware)
        x, y, _, _ = screen.bbox
        subprocess.run(["xdotool", "mousemove", str(x + 120), str(y + 120)],
                       env=env, check=True)
        time.sleep(30 / FPS)
        first = screen.frame()
        marker = first.getpixel((MARKER_X, TOP + PITCH // 2))
        background = first.getpixel((THUMB_X - 20, BOTTOM - 4))
        for name, clicks, button, delay in scenarios:
            frames, stamps = record(screen, clicks, button, delay)
            strip(frames, output / f"{root.name}-{name}.png")
            results[name] = analyse(frames, stamps, marker, background)
            results[name + "_final"] = frames[-1]
        assert firmware.poll() is None
    perf = rockbox / "ui-perf.txt"
    text = perf.read_text() if perf.is_file() else ""
    (output / f"{root.name}-ui-perf.txt").write_text(text)
    return results, text


SCENARIOS = [
    ("in-page", 1, 5, SLOW_DETENT_MS),
    ("in-page-3", 3, 5, SLOW_DETENT_MS),
    ("scroll", 1, 5, SLOW_DETENT_MS),
    ("bottom-end", 20, 5, SPIN_DETENT_MS),
    ("top-end", 24, 4, SPIN_DETENT_MS),
    ("fast", 14, 5, 17),
]


def check_scrim(result):
    summary = {}
    for name, *_ in SCENARIOS:
        samples = result[name]
        phases = {s["text_phase_px"] for s in samples}
        thumbs = {s["thumb_px"][1] - s["thumb_px"][0] for s in samples if s["thumb_px"]}
        # Snapped: the text never sits between rows and the thumb never
        # shortens at an end.
        assert len(phases) == 1, (name, phases)
        assert len(thumbs) == 1, (name, thumbs)
        markers = [s["marker_px"] for s in samples if s["marker_px"]]
        summary[name] = {
            "captured_frames": len(samples),
            "marker_positions": len({tuple(m) for m in markers}),
            "marker_tops_px": [m[0] for m in markers],
            "text_phase_px": phases.pop(),
        }
    # A move inside the page glides the marker through intermediate rows.
    inpage = summary["in-page"]["marker_tops_px"]
    assert inpage[0] == TOP and inpage[-1] == TOP + PITCH, inpage
    assert len(set(inpage)) >= 4, inpage
    # A move that scrolls keeps the marker on its screen row.
    assert set(summary["scroll"]["marker_tops_px"]) == {TOP + 4 * PITCH}
    return summary


def perf_captures(text):
    captures = []
    for line in text.splitlines():
        if line.startswith("route=list-glide"):
            fields = dict(item.split("=", 1) for item in line.split())
            captures.append({key: fields[key] for key in (
                "seen", "work_p99_f", "work_worst_f", "interval_worst_f",
                "skipped_slots", "interval_misses", "rate")})
    return captures


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("new", "new_zip", "old", "old_zip", "output"):
        parser.add_argument(name, type=lambda arg: Path(arg).resolve())
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="podbox-list-snap-") as folder, \
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

            result = {}
            scrim, perf = run(args.new, args.new_zip, root / "scrim", "scrim",
                              args.output, env, SCENARIOS)
            result["scrim"] = check_scrim(scrim)
            result["scrim"]["perf"] = perf_captures(perf)
            for theme in ("light",):
                new, _ = run(args.new, args.new_zip, root / f"{theme}-after", theme,
                             args.output, env, SCENARIOS)
                old, _ = run(args.old, args.old_zip, root / f"{theme}-before", theme,
                             args.output, env, SCENARIOS)
                compare = {}
                for name, *_ in SCENARIOS:
                    same_end = not ImageChops.difference(
                        new[name + "_final"], old[name + "_final"]).getbbox()
                    assert same_end, (theme, name)
                    compare[name] = {
                        "final_frame_identical": same_end,
                        "text_phases_before": sorted({s["text_phase_px"] for s in old[name]}),
                        "text_phases_after": sorted({s["text_phase_px"] for s in new[name]}),
                        "thumb_lengths_before": sorted({s["thumb_px"][1] - s["thumb_px"][0]
                                                        for s in old[name] if s["thumb_px"]}),
                        "thumb_lengths_after": sorted({s["thumb_px"][1] - s["thumb_px"][0]
                                                       for s in new[name] if s["thumb_px"]}),
                    }
                result[theme] = compare
    (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print("PASS Scrim snapped list scrolling; Light end frames unchanged")


if __name__ == "__main__":
    main()
