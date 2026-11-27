#!/usr/bin/env python3
"""Check the first readable frame of Fruit Company's album back.

Use SDL dummy video and delay foreground font batches by 400 ms. The
fixture's newest album has CJK album, artist and track names.
Arguments: ROCKBOXUI ZIP OUTPUT_DIRECTORY [--fixture DIRECTORY]
"""

import argparse
import json
import os
from pathlib import Path

from ui_stock_transitions_sim import make_fixture, session


def open_album(sim):
    sim.tap("select", 30)   # Home → Music
    sim.tap("select", 145)  # Music → Cover Flow
    return {"album-open": sim.record(144, "select")}


def glyph_ink(image):
    """Find the tilted bands column by column, then count their light ink."""
    pixels = image.load()
    header, selected = {}, {}
    for x in range(image.width):
        blues, selection = [], []
        for y in range(17, 135):
            r, g, b = pixels[x, y]
            if 40 <= r <= 150 and 22 <= g - r <= 63 and 35 <= b - r <= 84:
                blues.append(y)
            if r < 45 and g > 80 and b > 165:
                selection.append(y)
        if len(blues) >= 10 and blues[-1] - blues[0] <= 56:
            header[x] = (blues[0], blues[-1])
            if len(selection) >= 8:
                selected[x] = (selection[0], selection[-1])

    # A nearly edge-on back can project several glyphs onto one pixel.
    if len(header) < 25:
        return len(header), (0, 0, 0)
    left, right = min(header), max(header)
    width = right - left

    def light(x, y):
        r, g, b = pixels[x, y]
        return r >= 185 and b - r < 30 and g - r < 23

    title = artist = track = 0
    for x, (top, bottom) in header.items():
        if not left + width * 0.07 <= x <= right - width * 0.08:
            continue
        middle = (top + bottom) // 2
        title += sum(light(x, y) for y in range(top + 3, middle))
        artist += sum(light(x, y) for y in range(middle, bottom - 3))
        # Ignore the ASCII track number and duration at the sides.
        if x in selected and left + width * 0.23 < x < right - width * 0.13:
            a, b = selected[x]
            track += sum(light(x, y) for y in range(a + 2, b - 1))
    return len(header), (title, artist, track)


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
    os.environ["PODBOX_GLYPH_DELAY_MS"] = "400"
    shots = session(args.binary, fixture, args.output / "fruit-company",
                    "fruit-company", "album covers sort albums by: year descending\n",
                    open_album)["album-open"]

    ref_width, reference = glyph_ink(shots[-1][1])
    assert ref_width > 240 and min(reference) > 150, (ref_width, reference)
    for ms, image in shots:
        width, first = glyph_ink(image)
        if width >= 25:
            break
    assert 25 <= width < 240, "No readable frame of the turning album back"
    result = {"first_ms": round(ms), "columns": width, "ink": first,
              "settled_columns": ref_width, "settled_ink": reference}
    (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    # Allow for perspective and rasterization on the narrow turning card.
    assert all(10 * count * ref_width >= 3 * settled * width
               for count, settled in zip(first, reference)), result
    print("PASS first readable album back has title, artist and track glyphs",
          result)


if __name__ == "__main__":
    main()
