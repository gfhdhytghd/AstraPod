#!/usr/bin/env python3
"""Convert the stock iPod Classic fonts into PodBox glyph sets.

python3 tools/fonts/stock.py FONT_DIR [OUTPUT_DIR]

FONT_DIR is the Fonts folder of the stock firmware's resource partition:
Helvetica.ttf, HelveticaBold.ttf, CJK11.ttf and CJK16.ttf. OUTPUT_DIR
defaults to apps-ipod/fonts/stock. compile.py merges each Helvetica glyph
set (SGL1) with its CJK strike and then with the fallback faces at the same
pixel size into a PBF1 font of the same basename.

The stock firmware renders the unhinted Helvetica outlines with FreeType
2.3.5 into 8-bit grey coverage at whole-pixel origins, advances each glyph by
FreeType's advance rounded to a whole pixel and applies no kerning: FreeType
does not read the fonts' AAT kern table. Its Helvetica glyphs are converted
the same way, with FreeType 2.3.5 built from a pinned release in
~/.cache/podbox-font on first use. CJK comes from the 4-bit bitmap strikes.
"""
import argparse
from contextlib import ExitStack
import ctypes as C
import hashlib
import io
import os
from pathlib import Path
import shlex
import struct
import subprocess
import tarfile
import tempfile
from urllib.request import urlopen

from fontTools.ttLib import TTFont

import compile as compiler
from fusion import FUSION_CACHE

STOCK_DIR = compiler.ROOT / "apps-ipod/fonts/stock"
HEADER = struct.Struct("<4sHHHHHI")
ENTRY = struct.Struct("<IHH")
FREETYPE = "freetype-2.3.5"
FREETYPE_URL = f"https://download.savannah.gnu.org/releases/freetype/freetype-old/{FREETYPE}.tar.gz"
FREETYPE_SHA256 = "f3e43b388eb706342a8b86d9fb8aa782bfc76d8eef30b070f85b55425ee3076f"
FREETYPE_SOURCES = ("src/base/ftsystem.c", "src/base/ftinit.c", "src/base/ftbase.c",
                    "src/truetype/truetype.c", "src/sfnt/sfnt.c", "src/smooth/smooth.c")
FREETYPE_MODULES = b"""FT_USE_MODULE(tt_driver_class)
FT_USE_MODULE(sfnt_module_class)
FT_USE_MODULE(ft_smooth_renderer_class)
"""

# basename: (stock file, size in 1/64 px, fallback weight, line height,
# ascent, CJK strike). Each size is the one whose glyphs match the stock
# screens pixel for pixel: list rows 16.265625 px, Now Playing title
# 16.984375 px, the side pane's title 17.40625 px, Now Playing's times
# 13.796875 px and the 13.125 px small text. The 13 px faces keep the
# firmware's own 11/4 line; the larger faces fit the Classic layout's cells.
STOCK = {
    "13-helvetica": ("Helvetica.ttf", 840, 400, 15, 11, 11),
    "13-helvetica-bold": ("HelveticaBold.ttf", 840, 700, 15, 11, 11),
    "14-helvetica-bold": ("HelveticaBold.ttf", 883, 700, 16, 12, 11),
    "16-helvetica-bold": ("HelveticaBold.ttf", 1041, 700, 20, 16, 16),
    "17-helvetica-bold": ("HelveticaBold.ttf", 1087, 700, 18, 14, 16),
    "17-helvetica-bold-pane": ("HelveticaBold.ttf", 1114, 700, 19, 14, 16),
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


def build_raster(directory, cache):
    """stock_raster.c linked with the pinned FreeType release."""
    path = cache / f"{FREETYPE}.tar.gz"
    data = path.read_bytes() if path.is_file() else None
    if data is None:
        with urlopen(FREETYPE_URL, timeout=30) as response:
            data = response.read()
    if hashlib.sha256(data).hexdigest() != FREETYPE_SHA256:
        raise ValueError(f"Unexpected checksum: {path.name}")
    if not path.is_file():
        cache.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    with tarfile.open(fileobj=io.BytesIO(data)) as archive:
        members = [m for m in archive.getmembers()
                   if m.name.startswith((f"{FREETYPE}/include/", f"{FREETYPE}/src/"))]
        archive.extractall(directory, members, filter="data")
    (directory / "stock_modules.h").write_bytes(FREETYPE_MODULES)
    library = directory / "stock_raster.so"
    source = directory / FREETYPE
    subprocess.run(
        [*shlex.split(os.environ.get("CC", "cc")), "-O2", "-w", "-std=gnu99", "-fPIC", "-shared",
         "-DFT2_BUILD_LIBRARY", '-DFT_CONFIG_MODULES_H="stock_modules.h"',
         f"-I{directory}", f"-I{source / 'include'}",
         *(str(source / name) for name in FREETYPE_SOURCES),
         str(Path(__file__).with_name("stock_raster.c")), "-o", str(library)], check=True
    )
    lib = C.CDLL(str(library))
    lib.stock_open.restype = C.c_void_p
    lib.stock_open.argtypes = (C.c_char_p, C.c_int)
    lib.stock_close.restype = None
    lib.stock_close.argtypes = (C.c_void_p,)
    lib.stock_glyph.restype = C.c_int
    lib.stock_glyph.argtypes = (C.c_void_p, C.c_uint32, C.POINTER(C.c_int),
                                C.POINTER(C.c_uint8), C.c_uint)
    return lib


def outlines(lib, source, size64, required):
    """8-bit coverage records: phase count 1 with RECORD_GRAY8, the bearings
    and size, then one coverage byte per pixel, 0 transparent."""
    font = TTFont(source)
    cmap = font.getBestCmap()
    font.close()
    handle = lib.stock_open(os.fsencode(source), size64)
    if not handle:
        raise ValueError(f"FreeType could not open {source}")
    glyphs = {}
    try:
        for cp in sorted(required & cmap.keys()):
            if compiler.invisible(cp):
                continue
            metrics = (C.c_int * 5)()
            pixels = (C.c_uint8 * (160 * 160))()
            size = lib.stock_glyph(handle, cp, metrics, pixels, len(pixels))
            if size < 0:
                raise ValueError(f"FreeType could not rasterize U+{cp:04X}")
            x, y, w, h, advance = metrics
            record = struct.pack("<BbbBB", 1 | compiler.RECORD_GRAY8, x, y, w, h)
            glyphs[cp] = (record + bytes(pixels[:size]), (advance + 32) & ~63)
    finally:
        lib.stock_close(handle)
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
    parser.add_argument("--cache", type=Path, default=FUSION_CACHE)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    required = compiler.required_codepoints(args.lang_dir)
    with ExitStack() as stack:
        lib = build_raster(Path(stack.enter_context(
            tempfile.TemporaryDirectory(prefix="podbox-raster-"))), args.cache)
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
