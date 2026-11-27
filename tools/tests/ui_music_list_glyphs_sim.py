#!/usr/bin/env python3
"""Check Fruit Company Songs and Albums for blank rows with cold glyphs.

Arguments: ROCKBOXUI ZIP OUTPUT_DIRECTORY [--fixture DIRECTORY]
Runs with SDL dummy video and captures the submitted panel frames.
"""

import argparse
import json
import os
import subprocess
import time
from pathlib import Path
from zipfile import ZipFile

from PIL import Image

from ui_stock_transitions_sim import FPS, H, W, session
from ui_music_fixture import build_tagcache, database_count

TRACKS = 96
DELAY_MS = 400


def make_fixture(binary, resource, root):
    music = root / "Music"
    music.mkdir(parents=True)
    with ZipFile(resource) as archive:
        for name in archive.namelist():
            if name == ".rockbox/tagnavi.config" or name.startswith(
                (".rockbox/fonts/", ".rockbox/langs/",
                 ".rockbox/native-themes/")
            ):
                archive.extract(name, root)
    config = root / ".rockbox/config.cfg"
    config.write_text("start in screen: root\nanimations: on\n"
                      "backlight timeout: off\nvoice menus: off\n",
                      encoding="utf-8")
    seed = root.parent / "seed.mp3"
    ffmpeg = ["ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error"]
    subprocess.run(ffmpeg + ["-y", "-f", "lavfi", "-i",
                             "anullsrc=r=44100:cl=stereo", "-t", "0.15",
                             str(seed)], check=True)
    # Use disjoint glyphs per title to keep later pages cold during the spin.
    for i in range(TRACKS):
        title = "山海" + "".join(chr(0x7000 + 8 * i + j) for j in range(8))
        album = "風雨" + "".join(chr(0x7500 + 8 * i + j) for j in range(8))
        subprocess.run(ffmpeg + ["-i", str(seed), "-c", "copy",
                                 "-metadata", f"title={title}",
                                 "-metadata", f"album={album}",
                                 "-metadata", "artist=群山",
                                 str(music / f"{i:03d}.mp3")], check=True)
    build_tagcache(binary, root, root.parent / "tagcache.log")
    assert database_count(root) == TRACKS


def spin(sim, direction):
    seen, _ = sim.frames_since(1 << 30)
    start = time.monotonic()
    shots = []
    previous = None
    # Notches every 33 ms trigger the simulator wheel's fast path.
    pending = [((2 * i + release) / FPS, f"{action} {direction}")
               for i in range(60)
               for release, action in enumerate(("down", "up"))]
    while time.monotonic() - start < 2.5:
        now = time.monotonic() - start
        while pending and now >= pending[0][0]:
            sim.send(pending.pop(0)[1])
        seen, frames = sim.frames_since(seen)
        for pixels in frames:
            if pixels != previous:
                shots.append((now * 1000,
                              Image.frombytes("RGB", (W, H), pixels)))
                previous = pixels
        time.sleep(0.002)
    assert shots, "No scroll frames reached the panel"
    return shots


def capture(sim, row):
    sim.tap("select", 60)  # Home → Music
    if row == "songs":
        sim.tap("down", 30)
    return {"entry": sim.record(180, "select"),
            "down": spin(sim, "down"), "up": spin(sim, "up")}


def measure(row, shots):
    # Use only the text columns left of the stock quickscroll tile, and
    # exclude the covers, title bar, and scroll indicator.
    if row == "albums":
        x0, bands = 62, [(38 + 55 * i, 56 + 55 * i) for i in range(4)]
    else:
        x0 = 8
        bands = [(y, y + 16) for y in (24, 49, 74, 99, 123, 147, 171, 195, 219)]
    x1 = 108
    reference = shots["entry"][-1][1]

    def ink(image, offset=0, width=W):
        pixels = image.load()
        return [sum(pixels[offset + x, y] != pixels[offset + x0 - 1, y]
                    for y in range(top, bottom)
                    for x in range(x0, min(x1, width)))
                for top, bottom in bands]

    def signature(image, x):
        return tuple(image.getpixel((x, y)) for y in (21, 22, 23))

    # The top of the first selection band is flat across the incoming page.
    # Its left edge gives the offset even if all titles are missing.
    signature_at_rest = signature(reference, 0)
    partial, entry_blank = [], []
    readable_frames = 0
    landed = None
    for ms, image in shots["entry"]:
        edge = W
        while edge > 0 and signature(image, edge - 1) == signature_at_rest:
            edge -= 1
        if edge == W:
            continue
        if edge > 0:
            partial.append(round(ms))
        elif landed is None:
            landed = round(ms)
        expected = ink(reference, width=W - edge)
        actual = ink(image, offset=edge, width=W - edge)
        if edge and any(expected):
            readable_frames += 1
        if any(count < target * 0.85
               for count, target in zip(actual, expected)):
            entry_blank.append(round(ms))

    slide_ms = landed - partial[0] if partial and landed is not None else None
    result = {"entry_slide_frames": len(partial),
              "entry_readable_frames": readable_frames,
              "entry_blank_ms": entry_blank, "slide_ms": slide_ms}
    failures = []
    if min(ink(reference)) < 80:
        failures.append("settled entry text missing")
    if readable_frames < 3 or slide_ms is None:
        failures.append("no incoming slide")
    elif not 40 <= slide_ms <= 330:
        failures.append("incoming slide timing")
    if entry_blank:
        failures.append("blank rows during entry")
    for direction in ("down", "up"):
        motion = shots[direction]
        blank = [round(ms) for ms, image in motion if min(ink(image)) < 45]
        changed = len({image.crop((x0, bands[0][0], x1, bands[0][1])).tobytes()
                       for _, image in motion})
        result[direction] = {"frames": len(motion),
                             "distinct_top_rows": changed,
                             "blank_ms": blank}
        if changed < 5:
            failures.append(f"too few {direction} scroll poses")
        if blank:
            failures.append(f"blank rows scrolling {direction}")
    return result, failures


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("binary", "resource", "output"):
        parser.add_argument(name, type=lambda path: Path(path).resolve())
    parser.add_argument("--fixture", type=lambda path: Path(path).resolve())
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    fixture = args.fixture or args.output / "fixture"
    if not args.fixture:
        make_fixture(args.binary, args.resource, fixture)

    os.environ["PODBOX_GLYPH_DELAY_MS"] = str(DELAY_MS)
    results = {}
    failures = []
    for row in ("albums", "songs"):
        shots = session(args.binary, fixture, args.output / row,
                        "fruit-company",
                        "fruit music menu: 40\n"  # Albums and Songs
                        "lang: /.rockbox/langs/chinese-simp.lng\n"
                        "database album art: on\n"
                        "list wraparound: off\n",
                        lambda sim: capture(sim, row))
        results[row], failed = measure(row, shots)
        failures.extend(f"{row}: {failure}" for failure in failed)
    (args.output / "result.json").write_text(
        json.dumps(results, indent=2) + "\n")
    for failure in failures:
        print("FAIL", failure)
    if failures:
        raise SystemExit(1)
    print("PASS Fruit Company Songs and Albums entry and fast wheel scrolling")


if __name__ == "__main__":
    main()
