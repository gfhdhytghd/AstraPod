#!/usr/bin/env python3
"""Exercise paged Albums and Songs on the Chinese Fruit Company screen.

Run with ROCKBOXUI ZIP OUTPUT_DIRECTORY. SDL dummy video captures the LCD.
The row and overlay checks use Tesseract's English recognition on the Latin
parts of the fixture names.
"""

import argparse
import io
import json
import string
import subprocess
import time
from pathlib import Path
from zipfile import ZipFile

from PIL import Image, ImageOps

from ui_music_fixture import build_tagcache, database_count
from ui_stock_transitions_sim import H, W, session


def frame(tag, value):
    text = b"\x01" + value.encode("utf-16")
    return tag.encode("ascii") + len(text).to_bytes(4, "big") + b"\0\0" + text


def make_fixture(binary, resource, root):
    root.mkdir()
    music = root / "Music"
    music.mkdir()
    with ZipFile(resource) as archive:
        for name in archive.namelist():
            if name == ".rockbox/tagnavi.config" or name.startswith(
                (".rockbox/fonts/", ".rockbox/langs/", ".rockbox/themes/",
                 ".rockbox/native-themes/")
            ):
                archive.extract(name, root)
    (root / ".rockbox/config.cfg").write_text(
        "start in screen: root\nanimations: on\nbacklight timeout: off\n"
        "voice menus: off\nmax files in dir: 1000\n")
    seed = root.parent / "seed.mp3"
    subprocess.run([
        "ffmpeg", "-nostdin", "-loglevel", "error", "-f", "lavfi", "-i",
        "anullsrc=r=44100:cl=stereo", "-t", "0.15",
        "-id3v2_version", "0", "-write_xing", "0", str(seed),
    ], check=True)
    audio = seed.read_bytes()
    names = [f"{letter}{i:03d} Group"
             for letter in string.ascii_uppercase for i in range(40)]
    names += ["# Symbol", "007 Digits", "A 10 Apple", "The A 2 Apple",
              "The Blue Atlas", "Éclair Echo", "上海 OCEAN", "北京 FINAL"]
    fixtures = [(name, "Rock") for name in reversed(names)]
    fixtures.insert(0, ("Exclude This Audiobook", "Audiobook"))
    for number, (name, genre) in enumerate(fixtures):
        tags = (frame("TIT2", name) + frame("TALB", name) +
                frame("TPE1", "Test Artist") + frame("TCON", genre))
        length = len(tags)
        header = bytes((length >> shift) & 0x7f for shift in (21, 14, 7, 0))
        (music / f"{number:04d}.mp3").write_bytes(
            b"ID3\x03\x00\x00" + header + tags + audio)
    build_tagcache(binary, root, root.parent / "tagcache.log")
    assert database_count(root) == len(fixtures)


def visible_tile(image):
    for x in (130, 190):
        for y in (95, 120, 145):
            rgb = image.getpixel((x, y))
            if max(rgb) > 135 or max(rgb) - min(rgb) > 20:
                return False
    return True


def thumb_level(image):
    pixels = (image.getpixel((316, y)) for y in range(22, H))
    return min((g for r, g, b in pixels if max(r, g, b) - min(r, g, b) < 15),
               default=255)


def recognize(image, psm=7, whitelist=""):
    enlarged = image.resize((image.width * 3, image.height * 3))
    buffer = io.BytesIO()
    enlarged.save(buffer, format="PNG")
    return subprocess.run([
        "tesseract", "stdin", "stdout", "--psm", str(psm), "-l", "eng",
        "-c", "tessedit_char_whitelist=" + whitelist,
    ], input=buffer.getvalue(), capture_output=True, check=True
    ).stdout.decode().strip()


def tile_text(image):
    glyph = image.crop((134, 96, 187, 145)).convert("L")
    mask = glyph.point(lambda value: 0 if value > 215 else 255)
    return recognize(ImageOps.expand(mask, border=12, fill=255), 6,
                     string.ascii_uppercase + "123")


def row_text(image, row, albums):
    top = (21 + row * 55) if albums else (21 + row * 24)
    height = 54 if albums else 23
    row_image = image.crop((59 if albums else 5, top, 200, top + height))
    gray = ImageOps.grayscale(row_image)
    if gray.getpixel((1, gray.height // 2)) < 140:
        gray = ImageOps.invert(gray)
    return recognize(gray)


def frame_now(sim):
    _, frames = sim.frames_since(0)
    assert frames
    return Image.frombytes("RGB", (W, H), frames[-1])


def wheel(sim, direction, notches, interval, rest):
    count, _ = sim.frames_since(1 << 30)
    start = time.monotonic()
    pending = [(interval * i + delay, f"{action} {direction}")
               for i in range(notches)
               for delay, action in ((0, "down"), (.006, "up"))]
    shots = []
    while time.monotonic() - start < notches * interval + rest:
        elapsed = time.monotonic() - start
        while pending and elapsed >= pending[0][0]:
            sim.send(pending.pop(0)[1])
        count, frames = sim.frames_since(count)
        shots.extend((elapsed, Image.frombytes("RGB", (W, H), rgb))
                     for rgb in frames)
        time.sleep(.002)
    return shots


def check(sim, albums):
    # The boot scan may still own the UI while the 1,049-track cache opens.
    sim.wait(120)
    sim.tap("select", 60)
    if not albums:
        sim.tap("down", 26)
    sim.tap("select", 90)
    entry = frame_now(sim)
    entry.save(sim.out / "entry.png")
    first = [row_text(entry, i, albums) for i in range(4)]
    assert [text.split()[-1] for text in first] == [
        "Symbol", "Digits", "Apple", "Apple",
    ], first
    assert "10" in first[2] and first[3].startswith("The A 2"), first

    slow = wheel(sim, "down", 4, .27, .6)
    assert slow and not any(visible_tile(image) for _, image in slow)

    interval, notches = .033, 69
    fast = wheel(sim, "down", notches, interval, 1.2)
    tiles = [(t, image) for t, image in fast if visible_tile(image)]
    assert tiles, "No visible tile on the fast spin"
    last_motion = (notches - 1) * interval
    first_on, _ = tiles[0]
    assert .65 < first_on < 2.2, first_on
    after = next(t for t, image in fast
                 if t > tiles[-1][0] and not visible_tile(image))
    assert .40 < after - last_motion < .70, (after, last_motion)
    assert not any(not visible_tile(img) for t, img in fast
                   if first_on < t < after), "Tile disappeared mid-spin"
    assert len({img.getpixel((190, 108)) for _, img in tiles}) == 1

    _, tile = tiles[-1]
    tile.save(sim.out / "tile.png")
    letter = tile_text(tile)
    row = row_text(tile, 0, albums)
    assert len(letter) == 1 and letter in string.ascii_uppercase, letter
    # All four-character fixture IDs begin with a letter. OCR can read an
    # O, I or Z next to its three digits as a digit too.
    first_letter = row[:1].translate(str.maketrans("012", "OIZ"))
    assert "Group" in row and first_letter == letter, (letter, row)
    assert any(t > last_motion and visible_tile(img) for t, img in fast)
    assert thumb_level(tile) < 190, "Scrollbar dimmed while the tile showed"
    assert thumb_level(frame_now(sim)) > 240, "Scrollbar stayed after release"
    assert not visible_tile(frame_now(sim))

    reverse = wheel(sim, "up", 150, interval, .8)
    numbers = [image for _, image in reverse if visible_tile(image)]
    assert numbers, "Reverse spin never entered letter jump"
    digits = numbers[-1]
    digits.save(sim.out / "digits.png")
    digit_label = tile_text(digits)
    assert digit_label == "123", digit_label
    assert "Symbol" in row_text(digits, 0, albums)

    # Spin from the first group across the page boundary and into CJK.
    tail = wheel(sim, "down", 150, interval, .9)
    shown = [t for t, image in tail if visible_tile(image)]
    assert shown, "Forward spin never entered letter jump"
    assert any(shown[-1] < t < 149 * interval and not visible_tile(image)
               for t, image in tail), "Tile stayed in the CJK group"
    end = frame_now(sim)
    end.save(sim.out / "tail.png")
    last_rows = [row_text(end, i, albums) for i in
                 (range(4) if albums else range(9))]
    assert "OCEAN" in last_rows[-2] and "FINAL" in last_rows[-1], last_rows
    return {"first_rows": first, "overlay_label": letter, "overlay_row": row,
            "on_seconds": first_on, "off_after_motion": after - last_motion,
            "last_rows": last_rows}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("binary", "resource", "output"):
        parser.add_argument(name, type=lambda p: Path(p).resolve())
    parser.add_argument("--fixture", type=lambda p: Path(p).resolve())
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    fixture = args.fixture or args.output / "fixture"
    if not args.fixture:
        make_fixture(args.binary, args.resource, fixture)
    result = {}
    for view in ("albums", "songs"):
        def run(sim):
            result[view] = check(sim, albums=view == "albums")
            return {}

        session(
            args.binary, fixture, args.output / view, "fruit-company",
            "fruit music menu: 40\nlang: /.rockbox/langs/chinese-simp.lng\n"
            "database album art: on\nalbum show year: off\n"
            "show all tracks row: off\nshow random row: off\n"
            "show shuffle albums row: off\nlist wraparound: off\n"
            "sort ignoring articles: on\nsegregate audiobooks: on\n", run)
    (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print("PASS Albums and Songs name order, overlay key and timing")


if __name__ == "__main__":
    main()
