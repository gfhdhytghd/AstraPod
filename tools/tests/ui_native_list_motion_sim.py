#!/usr/bin/env python3
"""Exercise native list glide, edge margins, and spring-back in SDL."""

import argparse
import json
import os
import shutil
import subprocess
import tempfile
import time
from pathlib import Path
from zipfile import ZipFile

from PIL import ImageChops

from ui_menu_sim import FPS, Screen, process


TOP = 46
PITCH = 30
VISIBLE = 6
BOTTOM = TOP + VISIBLE * PITCH
HALF_ROW = PITCH // 2
MARKER_X = 47
THUMB_X = 305
EMPTY_X = 314
TRACK_COUNT = 20


def driver_delay(frames):
    # xdotool and Pillow expose durations in thousandths of a second.
    return round(frames * 1000 / FPS)


def content_span(frame, x, test):
    rows = [y for y in range(TOP, BOTTOM) if test(frame, x, y)]
    assert rows
    return [rows[0], rows[-1]]


def capture_spin(screen, output, name, clicks, direction, marker):
    t0 = time.monotonic()
    command = ["xdotool", "click", "--repeat", str(clicks), "--delay",
               str(driver_delay(0.9)), str(direction)]
    clicker = subprocess.Popen(command, env=screen.env)
    frames, stamps = [screen.frame()], [0]
    for sample in range(1, 2 * (clicks + 25)):
        target = t0 + sample / (2 * FPS)
        time.sleep(max(0, target - time.monotonic()))
        frame = screen.frame()
        if ImageChops.difference(frames[-1], frame).getbbox():
            frames.append(frame)
            stamps.append(round((time.monotonic() - t0) * FPS, 2))
    assert clicker.wait() == 0

    intervals = [b - a for a, b in zip(stamps, stamps[1:])] + [12]
    frames[0].save(output / f"{name}.gif", save_all=True, append_images=frames[1:],
                   duration=[driver_delay(interval) for interval in intervals], loop=0)
    samples = []
    for stamp, frame in zip(stamps, frames):
        cursor = content_span(frame, MARKER_X, lambda im, x, y: im.getpixel((x, y)) == marker)
        thumb = content_span(frame, THUMB_X, lambda im, x, y: TOP < y < BOTTOM - 1
                             and im.getpixel((x, y)) != im.getpixel((EMPTY_X, y)))
        samples.append({"at_f": stamp, "cursor_px": cursor, "thumb_px": thumb})
    (output / f"{name}.json").write_text(json.dumps(samples, indent=2) + "\n")
    return samples


def selection_samples(samples):
    return [sample["cursor_px"] for sample in samples]


def verify_margin(samples, top):
    cursors = selection_samples(samples)
    if top:
        half = [TOP, TOP + HALF_ROW - 1]
        settled = [TOP + PITCH, TOP + 2 * PITCH - 1]
    else:
        half = [BOTTOM - HALF_ROW, BOTTOM - 1]
        settled = [BOTTOM - 2 * PITCH, BOTTOM - PITCH - 1]
    assert half in cursors
    assert cursors[-1] == settled
    assert all(end - start + 1 >= HALF_ROW for start, end in cursors)
    peak = next(sample for sample in reversed(samples) if sample["cursor_px"] == half)
    assert peak["thumb_px"] != samples[-1]["thumb_px"]
    return {"half_row_cursor_px": half, "rest_cursor_px": settled,
            "peak_to_rest_observed_f": round(samples[-1]["at_f"] - peak["at_f"], 2)}


def verify_end(samples, top):
    cursors = selection_samples(samples)
    rested = TOP if top else BOTTOM - PITCH
    assert cursors[-1] == [rested, rested + PITCH - 1]
    # Find a rubber-band pose by its shortened thumb at the physical end.
    final_thumb = samples[-1]["thumb_px"]
    edge = 0 if top else 1
    stretched = [
        sample for sample in samples
        if sample["thumb_px"][edge] == final_thumb[edge]
        and sample["thumb_px"][1] - sample["thumb_px"][0] < final_thumb[1] - final_thumb[0]
    ]
    assert stretched
    shifts = [abs(sample["cursor_px"][0] - rested) for sample in stretched]
    maximum = max(shifts)
    assert 8 <= maximum <= HALF_ROW
    return {"max_pull_observed_px": maximum, "rest_cursor_first_px": rested,
            "thumb_shortened": True}


def run(binary, resource, root, output, env, animations):
    mode = "motion" if animations else "static"
    root.mkdir()
    with ZipFile(resource) as archive:
        for path in archive.namelist():
            if path.startswith(".rockbox/fonts/"):
                archive.extract(path, root)
    rockbox = root / ".rockbox"
    (rockbox / "config.cfg").write_text(
        "start in screen: files\nshow files: supported\nlist wraparound: off\n"
        "backlight timeout: off\nvoice menus: off\nkeyclick: off\n"
        f"animations: {'on' if animations else 'off'}\n"
        "settings mode: everything\nbeep: off\nscrollbar: right\n"
        "scrollbar width: 8\nnative theme: scrim\n")
    (rockbox / "ui-perf.enable").touch()
    for i in range(TRACK_COUNT):
        (root / f"Track {i + 1:02d}").mkdir()

    with (output / f"{mode}-sim.log").open("w") as log, process(
        [str(binary), "--root", str(root), "--nobackground", "--zoom", "1", "--screenshot"],
        log, env=env, cwd=root,
    ) as firmware:
        time.sleep(120 / FPS)
        screen = Screen(env, firmware)
        x, y, _, _ = screen.bbox
        subprocess.run(["xdotool", "mousemove", str(x + 120), str(y + 120)],
                       env=env, check=True)
        frame = screen.frame()
        marker = frame.getpixel((MARKER_X, TOP + HALF_ROW))
        frame.save(output / f"{mode}-initial.png")
        result = {}
        bottom = capture_spin(screen, output, f"{mode}-bottom-margin", 13, 5, marker)
        if animations:
            result["bottom_margin"] = verify_margin(bottom, top=False)
            end = capture_spin(screen, output, "motion-bottom-end", 12, 5, marker)
            result["bottom_end"] = verify_end(end, top=False)
            margin = capture_spin(screen, output, "motion-top-margin", 14, 4, marker)
            result["top_margin"] = verify_margin(margin, top=True)
            end = capture_spin(screen, output, "motion-top-end", 12, 4, marker)
            result["top_end"] = verify_end(end, top=True)
            trace = rockbox / "ui-perf.txt"
            assert trace.read_text().count("route=list-glide") == 4
            shutil.copy2(trace, output / "motion-perf.txt")
        else:
            assert all(end - start + 1 == PITCH for start, end in selection_samples(bottom))
            assert not (rockbox / "ui-perf.txt").exists()
            result["switch_off"] = True
        assert firmware.poll() is None
        return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("binary", "resource", "output"):
        parser.add_argument(name, type=lambda arg: Path(arg).resolve())
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="podbox-list-motion-") as folder, \
            tempfile.TemporaryFile() as display, (args.output / "xvfb.log").open("w") as log:
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
            motion = run(args.binary, args.resource, root / "motion", args.output, env, True)
            static = run(args.binary, args.resource, root / "static", args.output, env, False)
    result = {"row_height_px": PITCH, **motion, **static}
    (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print("PASS native list half-row edges, physical springs, scrollbar and switch")


if __name__ == "__main__":
    main()
