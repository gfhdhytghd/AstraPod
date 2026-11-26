#!/usr/bin/env python3
"""Convert the stock iPod Classic fonts into PodBox glyph sets.

python3 tools/fonts/stock.py FONT_DIR [OUTPUT_DIR]

FONT_DIR is the Fonts folder of the stock firmware's resource partition:
Helvetica.ttf, HelveticaBold.ttf, CJK11.ttf and CJK16.ttf. OUTPUT_DIR
defaults to apps-ipod/fonts/stock. compile.py merges each Helvetica glyph
set (SGL1) with its CJK strike and then with the fallback faces at the same
pixel size into a PBF1 font of the same basename.

The stock firmware draws unhinted Helvetica outlines with linear coverage at
whole-pixel origins, advances each glyph by its linear advance rounded to a
whole pixel and applies no kerning. CJK comes from the 4-bit bitmap strikes.
"""
import argparse
from contextlib import ExitStack
import os
from pathlib import Path
import struct
import tempfile

from fontTools.ttLib import TTFont

import compile as compiler

STOCK_DIR = compiler.ROOT / "apps-ipod/fonts/stock"
HEADER = struct.Struct("<4sHHHHHI")
ENTRY = struct.Struct("<IHH")
LINEAR = tuple(15 - round(15 * v / 255) for v in range(256))

# basename: (stock file, scale in 1/64 px, fallback weight, line height,
# ascent, CJK strike). The scale reproduces the stock text widths; FreeType
# rounds it to the whole ppem the firmware reports. The 13 px faces keep the
# firmware's own 11/4 line; the larger faces fit the Classic layout's cells.
STOCK = {
    "13-helvetica": ("Helvetica.ttf", 840, 400, 15, 11, 11),
    "13-helvetica-bold": ("HelveticaBold.ttf", 840, 700, 15, 11, 11),
    "16-helvetica-bold": ("HelveticaBold.ttf", 1040, 700, 20, 16, 16),
    "17-helvetica-bold": ("HelveticaBold.ttf", 1088, 700, 18, 14, 16),
    "17-helvetica-bold-pane": ("HelveticaBold.ttf", 1116, 700, 19, 14, 16),
}
CJK_FILES = {11: "CJK11.ttf", 16: "CJK16.ttf"}


class GlyphSet:
    """One SGL1 file: stock glyph records keyed by codepoint."""

    def __init__(self, path):
        path = Path(path)
        data = path.read_bytes()
        (magic, self.size64, self.weight, self.height, self.ascent, cjk,
         count) = HEADER.unpack_from(data)
        if magic != b"SGL1":
            raise ValueError(f"{path} is not an SGL1 glyph set")
        at = HEADER.size + count * ENTRY.size
        self.glyphs = {}
        for cp, advance, length in ENTRY.iter_unpack(data[HEADER.size:at]):
            self.glyphs[cp] = (data[at:at + length], advance)
            at += length
        if at != len(data):
            raise ValueError(f"{path} has trailing bytes")
        self.own = len(self.glyphs)
        if cjk:
            strike = GlyphSet(path.with_name(f"cjk{cjk}.sgl"))
            self.glyphs = {**strike.glyphs, **self.glyphs}

    @property
    def size(self):
        """The fallback faces' pixel size."""
        return (self.size64 + 32) // 64


def write(path, size64, weight, height, ascent, cjk, glyphs):
    entries = b"".join(ENTRY.pack(cp, advance, len(record))
                       for cp, (record, advance) in sorted(glyphs.items()))
    records = b"".join(record for _, (record, _) in sorted(glyphs.items()))
    path.write_bytes(HEADER.pack(b"SGL1", size64, weight, height, ascent, cjk,
                                 len(glyphs)) + entries + records)


def outlines(lib, source, size64, required):
    font = TTFont(source)
    cmap = font.getBestCmap()
    font.close()
    handle = lib.source_open_scaled(os.fsencode(source), 0, size64, 400)
    if not handle:
        raise ValueError(f"FreeType could not open {source}")
    glyphs = {}
    try:
        for cp in sorted(required & cmap.keys()):
            if compiler.invisible(cp):
                continue
            metrics = (compiler.C.c_int * 4)()
            pixels = (compiler.C.c_uint8 * (160 * 160))()
            size = lib.source_raster_unhinted(handle, cp, 0, metrics, pixels, len(pixels))
            advance = lib.source_advance(handle, cp)
            if size < 0 or advance < 0:
                raise ValueError(f"FreeType could not rasterize U+{cp:04X}")
            x, y, w, h = metrics
            record = bytearray(struct.pack("<BbbBB", 1, x, y, w, h))
            record += compiler.pack_alpha(bytes(pixels[:size]), LINEAR)
            glyphs[cp] = (bytes(record), (advance + 32) & ~63)
    finally:
        lib.source_close(handle)
    return glyphs


def strike(source, required):
    """The 4-bit EBDT strike: rows are byte aligned, high nibble first,
    0 transparent."""
    font = TTFont(source)
    cmap = font.getBestCmap()
    line = font["EBLC"].strikes[0].bitmapSizeTable.hori
    bitmaps = font["EBDT"].strikeData[0]
    glyphs = {}
    for cp in sorted(required & cmap.keys()):
        if compiler.invisible(cp) or cmap[cp] not in bitmaps:
            continue
        glyph = bitmaps[cmap[cp]]
        m = glyph.metrics
        pitch = (m.width + 1) // 2
        coverage = bytearray()
        for row in range(m.height):
            data = glyph.imageData[row * pitch:(row + 1) * pitch]
            for col in range(m.width):
                coverage.append((data[col // 2] >> (0 if col % 2 else 4)) & 15)
        record = bytearray(struct.pack("<BbbBB", 1, m.BearingX, m.BearingY,
                                       m.width, m.height))
        record += compiler.pack_alpha(coverage, tuple(15 - v for v in range(16)))
        glyphs[cp] = (bytes(record), m.Advance * 64)
    font.close()
    return line.ascender - line.descender, line.ascender, glyphs


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("fonts", type=Path)
    parser.add_argument("output", type=Path, nargs="?", default=STOCK_DIR)
    parser.add_argument("--lang-dir", type=Path, default=compiler.ROOT / "apps-ipod/lang")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    required = compiler.required_codepoints(args.lang_dir)
    with ExitStack() as stack:
        lib = compiler.build_raster(Path(stack.enter_context(
            tempfile.TemporaryDirectory(prefix="podbox-raster-"))))
        for ppem, file in CJK_FILES.items():
            height, ascent, glyphs = strike(args.fonts / file, required)
            path = args.output / f"cjk{ppem}.sgl"
            write(path, ppem * 64, 0, height, ascent, 0, glyphs)
            print(path.stem, len(glyphs), "glyphs", path.stat().st_size, "bytes")
        for name, (file, size64, weight, height, ascent, cjk) in STOCK.items():
            glyphs = outlines(lib, args.fonts / file, size64, required)
            path = args.output / (name + ".sgl")
            write(path, size64, weight, height, ascent, cjk, glyphs)
            print(name, len(glyphs), "glyphs", path.stat().st_size, "bytes")


if __name__ == "__main__":
    main()
