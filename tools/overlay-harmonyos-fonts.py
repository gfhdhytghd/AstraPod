#!/usr/bin/env python3
"""Render Chinese glyphs for a local Rockbox build from user-supplied fonts.

The base package supplies its existing Latin glyphs, advance widths, heights,
baselines, and RB12 glyph slots. This tool fills the existing Han slots by
rendering the HarmonyOS fonts locally. It does not ship or alter either TTF.
Requires Pillow with FreeType, and fontTools.
"""
import argparse
from collections import Counter
from functools import cache
from hashlib import sha256
import json
from pathlib import Path
from struct import Struct, iter_unpack
from unicodedata import normalize
from zipfile import ZipFile

from fontTools.ttLib import TTFont
from fontTools.unicodedata import script
from PIL import Image, ImageFont, features


FONT_DIR = ".rockbox/fonts/"
FACES = {
    "12-noto-sans.fnt": (12, 400),
    "18-noto-sans.fnt": (18, 400),
    "18-noto-sans-bold.fnt": (18, 706),
    "22-noto-sans-medium.fnt": (22, 500),
    "26-noto-sans-medium.fnt": (26, 500),
    "24-seven-fifteen.fnt": (16, 400),
    **{f"{size}-noto-serif.fnt": (size, 400) for size in (18, 20, 22, 24, 28)},
}
HEADER = Struct("<4s4H6I")
TRANSPARENCY = bytes((255 - alpha + 8) // 17 for alpha in range(256))
HIGH_NIBBLE = bytes((value & 15) << 4 for value in range(256))


def rb12_layout(data):
    magic, maxwidth, height, ascent, depth, first, default, count, bits, offsets, widths = (
        HEADER.unpack_from(data)
    )
    if (magic != b"RB12" or depth != 1 or not 0 < ascent <= height or
            widths != count or offsets != count or not first <= default < first + count):
        raise ValueError("Expected a proportional 4-bit RB12 font with complete tables")
    offset_bytes = 4 if bits >= 0xFFDB else 2
    table_start = (HEADER.size + bits + offset_bytes - 1) & -offset_bytes
    table_end = table_start + count * offset_bytes
    advances = data[table_end:]
    if len(advances) != count or not advances or max(advances) > maxwidth:
        raise ValueError("Invalid RB12 width table")
    fmt = "<I" if offset_bytes == 4 else "<H"
    addresses = [address for address, in iter_unpack(fmt, data[table_start:table_end])]
    intervals = sorted({(address, address + (width * height + 1) // 2)
                        for address, width in zip(addresses, advances)})
    if not intervals or intervals[-1][1] > bits or any(
        end > next_start for (_, end), (next_start, _) in zip(intervals, intervals[1:])
    ):
        raise ValueError("Overlapping or out-of-bounds RB12 glyph slots")
    return height, ascent, first, default - first, addresses, advances


@cache
def font_face(path, size, weight):
    face = ImageFont.truetype(path, size, layout_engine=ImageFont.Layout.BASIC)
    face.set_variation_by_axes([weight])
    return face


def render_glyph(path, size, weight, ch, width, height, ascent):
    face = font_face(path, min(size, width + 2), weight)
    mask, (_, y) = face.getmask2(ch, "L", anchor="ls")
    ink = Image.frombytes("L", mask.size, bytes(mask))
    bounds = ink.getbbox()
    if bounds is None:
        raise ValueError(f"No ink rendered for U+{ord(ch):04X}")
    ink = ink.crop(bounds)
    top = ascent + y + bounds[1]
    if ink.width > width or ink.height > height:
        scale = min(width / ink.width, height / ink.height)
        new_size = (max(1, round(ink.width * scale)), max(1, round(ink.height * scale)))
        top += (ink.height - new_size[1]) // 2
        ink = ink.resize(new_size, Image.Resampling.LANCZOS)
    cell = Image.new("L", (width, height))
    cell.paste(ink, ((width - ink.width) // 2, min(max(0, top), height - ink.height)))
    samples = cell.tobytes().translate(TRANSPARENCY)
    if len(samples) % 2:
        samples += bytes(1)
    return bytes(a | b for a, b in zip(samples[0::2], samples[1::2].translate(HIGH_NIBBLE)))


def overlay(data, source_faces, size, weight):
    height, ascent, first, default, addresses, advances = rb12_layout(data)
    aliases = Counter(addresses)
    output = bytearray(data)
    covered = dict.fromkeys(source_faces, 0)
    changed = 0
    compatibility_sources = 0
    missing = []
    for glyph, address in enumerate(addresses):
        ch = chr(first + glyph)
        if address == addresses[default] or script(ch) != "Hani":
            continue
        if aliases[address] != 1:
            raise ValueError(f"U+{ord(ch):04X} shares its original bitmap slot")
        # HarmonyOS uses the equivalent ideograph for a Kangxi radical.
        # Prefer an exact codepoint in either face before using decomposition.
        compatible = normalize("NFKC", ch)
        choices = (ch, compatible) if len(compatible) == 1 and compatible != ch else (ch,)
        source = next(((name, path, char) for char in choices
                       for name, (path, cmap) in source_faces.items() if ord(char) in cmap), None)
        if source is None:
            missing.append(f"U+{ord(ch):04X}")
            continue
        name, path, char = source
        width = advances[glyph]
        bitmap = render_glyph(path, size, weight, char, width, height, ascent)
        start_byte = HEADER.size + address
        stop_byte = start_byte + (width * height + 1) // 2
        if len(bitmap) != stop_byte - start_byte:
            raise ValueError("Rasterizer exceeded original glyph slot")
        changed += output[start_byte:stop_byte] != bitmap
        output[start_byte:stop_byte] = bitmap
        covered[name] += 1
        compatibility_sources += char != ch
    return bytes(output), {"height": height, "ascent": ascent, "covered": covered,
                           "compatibility_sources": compatibility_sources,
                           "changed": changed, "missing": missing}


def read_source(path, variant):
    with TTFont(path, lazy=True) as source:
        if source["name"].getDebugName(1) != f"HarmonyOS Sans {variant}":
            raise ValueError(f"Expected HarmonyOS Sans {variant} at {path}")
        cmap = set(source.getBestCmap())
        version = source["name"].getDebugName(5)
    return cmap, {"version": version, "sha256": sha256(path.read_bytes()).hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("base_zip", type=Path)
    parser.add_argument("output", type=Path, help="Directory for the local resource overlay")
    parser.add_argument("--sc", required=True, type=Path, help="HarmonyOS Sans SC TTF")
    parser.add_argument("--tc", required=True, type=Path, help="HarmonyOS Sans TC TTF")
    parser.add_argument("--variant", default="SC", choices=("SC", "TC"),
                        help="Primary Han face where both variants cover a character")
    args = parser.parse_args()

    source_faces = {}
    report = {"primary_variant": args.variant, "freetype": features.version_module("freetype2"),
              "fonts": {}, "sources": {}}
    for variant in (args.variant, "TC" if args.variant == "SC" else "SC"):
        path = getattr(args, variant.lower())
        cmap, metadata = read_source(path, variant)
        report["sources"][variant] = metadata
        source_faces[variant] = (path, cmap)

    with ZipFile(args.base_zip) as package:
        for filename, (size, weight) in FACES.items():
            name = FONT_DIR + filename
            data = package.read(name)
            converted, details = overlay(data, source_faces, size, weight)
            if details["missing"]:
                raise ValueError(f"{name} has {len(details['missing'])} missing Han glyphs")
            target = args.output / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(converted)
            details.update(bytes=len(converted), pixel_size=size, weight=weight,
                           base_sha256=sha256(data).hexdigest(),
                           overlay_sha256=sha256(converted).hexdigest())
            report["fonts"][name] = details
            print(filename, details["covered"], flush=True)

    (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
