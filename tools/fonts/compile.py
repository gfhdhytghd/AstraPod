#!/usr/bin/env python3
"""Compile deterministic, pixel-native PodBox fonts (PBF1)."""
import argparse
import collections
import ctypes as C
from contextlib import ExitStack
import json
import os
from pathlib import Path
import re
import shlex
import struct
from statistics import median
import subprocess
import tempfile
import unicodedata as ud

from fontTools.ttLib import TTFont

from fusion import FUSION_CACHE, glyph_bitmap, load_fusion

ROOT = Path(__file__).resolve().parents[2]
HEADER = struct.Struct("<4s6H5I")
INDEX = struct.Struct("<IIHHBB")
PAIR = struct.Struct("<Bh")
PAGE = 64
RECORD_RGB = 0x80
RECORD_GRAY8 = 0x40
GAMMA = 0.90

# Nominal pixel sizes come from the existing basenames and theme roles.
# Preserve the old basenames to read settings and themes from previous releases.
FONTS = {
    "8-noto-sans": (8, 400), "10-noto-sans": (10, 400),
    "12-noto-sans": (12, 400), "14-noto-sans": (14, 400),
    "18-noto-sans": (18, 400), "18-noto-sans-bold": (18, 700),
    "20-noto-sans": (20, 400), "20-noto-serif": (20, 400),
    "22-noto-sans-medium": (22, 500), "22-noto-serif": (22, 400),
    "24-noto-serif": (24, 400), "26-noto-sans-medium": (26, 500),
    "28-noto-serif": (28, 400), "30-noto-serif-figures": (30, 400),
    "18-noto-serif": (18, 400),
    "56-podbox-index": (56, 500),
}
FACES = (
    "HarmonyOS Sans", "HarmonyOS Sans SC", "Noto Sans CJK SC", "Noto Sans",
    "Noto Sans Arabic", "Noto Sans Hebrew", "Noto Sans Devanagari", "Noto Sans Thai",
    "Noto Sans Symbols", "Noto Sans Symbols 2",
    "HarmonyOS Sans TC", "Noto Serif CJK SC",
)
REPLACEMENT = 0xFFFD
COVERAGE = "\u00ad‐‑‒–—―‘’‚“”„†‡•‧…‰′″‹›※、。・「」『』【】《》！（），．：；？～·￥✓✗←↑→↓▶⏸♫♡⌘␣□"


def fontconfig():
    output = subprocess.check_output(
        ["fc-list", "-f", "%{family}\t%{style}\t%{file}\t%{index}\n"], text=True
    )
    catalog = collections.defaultdict(list)
    for line in output.splitlines():
        names, styles, file, index = line.split("\t")
        if int(index) >> 16 or styles.split(",")[0] not in ("Regular", "Normal"):
            continue
        for name in names.split(","):
            catalog[name].append((file, int(index)))
    return catalog


def locate_fonts(overrides):
    catalog = fontconfig()
    for name in FACES:
        if name in overrides:
            path, index = overrides[name]
        elif catalog[name]:
            path, index = min(catalog[name])
        else:
            raise SystemExit(f"QUESTION: Where is the regular {name} font file?")
        yield name, path, index


def lang_strings(lang_dir):
    for file in sorted(lang_dir.glob("*.lang")):
        for line in file.read_text(encoding="utf-8-sig").splitlines():
            match = re.search(r':\s*"((?:[^"\\]|\\.)*)"\s*$', line)
            if match:
                yield file.name, re.sub(r'\\(["\\])', r"\1", match[1])


def required_codepoints(lang_dir, full=True):
    result = set(range(32, 127)) | set(range(0xA0, 0x180))
    result.update(map(ord, COVERAGE))
    result.add(REPLACEMENT)
    for first in range(0xA1, 0xF8):
        for second in range(0xA1, 0xFF):
            try:
                result.add(ord(bytes((first, second)).decode("gb2312")))
            except UnicodeDecodeError:
                pass
    for file in lang_dir.glob("*.lang"):
        result.update(map(ord, file.read_text(encoding="utf-8-sig")))
    # The firmware bidi path emits Arabic presentation forms.
    result.update(range(0xFE80, 0xFEFD))
    if full:
        result.update(range(0x202a, 0x202f))  # invisible directional formatting
        ranges = (
            (0x0370, 0x03e1), (0x03f0, 0x04ff),  # Greek and Cyrillic
            (0x3000, 0x303f), (0x3040, 0x30ff), (0x3100, 0x312f),
            (0x31a0, 0x31bb), (0x31f0, 0x31ff),  # Bopomofo and kana extensions
            (0x3400, 0x4dbf), (0x4e00, 0x9fff), (0xac00, 0xd7a3),
            (0xff00, 0xffef),
        )
        result.update(cp for start, end in ranges for cp in range(start, end + 1)
                      if ud.category(chr(cp)) != "Cn")
    return result


def ideograph(cp):
    return 0x3400 <= cp <= 0x9FFF or 0xF900 <= cp <= 0xFAFF or 0x20000 <= cp <= 0x323AF


def cjk(cp):
    return (ideograph(cp) or 0x3040 <= cp <= 0x312F or 0x31A0 <= cp <= 0x31FF or
            0xAC00 <= cp <= 0xD7AF or 0xFF65 <= cp <= 0xFF9F)


def punct(cp):
    return ud.category(chr(cp)).startswith("P")


def invisible(cp):
    return ud.category(chr(cp)) in {"Cc", "Cf", "Zl", "Zp"}


def resolve(fonts, required):
    sources = {}
    missing = []
    tc = FACES.index("HarmonyOS Sans TC")
    serif = FACES.index("Noto Serif CJK SC")
    for cp in sorted(required):
        if invisible(cp):
            sources[cp] = None
            continue
        order = ((0, 1, 3, tc, 2, serif) if cp < 0x300 and not cjk(cp) else
                 (1, 0, tc, 2, serif, 3))
        source = next((i for i in (*order, *range(4, tc)) if cp in fonts[i].cmap), None)
        if source is None:
            missing.append(f"U+{cp:04X} {chr(cp)}")
        else:
            sources[cp] = source
    if missing:
        raise ValueError("Missing required glyphs: " + ", ".join(missing))
    return sources


def build_raster(directory):
    library = directory / "raster.so"
    flags = subprocess.check_output(
        ["pkg-config", "--cflags", "--libs", "freetype2", "harfbuzz"], text=True
    )
    subprocess.run(
        [*shlex.split(os.environ.get("CC", "cc")), "-O2", "-Wall", "-Wextra", "-Werror",
         "-fPIC", "-shared", str(Path(__file__).with_name("raster.c")),
         "-o", str(library), *shlex.split(flags)], check=True
    )
    lib = C.CDLL(str(library))
    args = {
        "source_open": (C.c_void_p, C.c_char_p, C.c_int, C.c_int, C.c_int),
        "source_open_scaled": (C.c_void_p, C.c_char_p, C.c_int, C.c_int, C.c_int),
        "source_close": (None, C.c_void_p),
        "source_metrics": (None, C.c_void_p, C.POINTER(C.c_int)),
        "source_advance": (C.c_int, C.c_void_p, C.c_uint32),
        "source_raster": (C.c_int, C.c_void_p, C.c_uint32, C.c_int,
                          C.POINTER(C.c_int), C.POINTER(C.c_uint8), C.c_uint),
        "source_layout": (C.c_int, C.c_void_p, C.POINTER(C.c_uint32), C.c_uint,
                          C.c_int, C.c_int, C.POINTER(C.c_int)),
    }
    args["source_raster_lcd"] = (*args["source_raster"], C.c_int)
    args["source_raster_unhinted"] = args["source_raster"]
    for name, (result, *argtypes) in args.items():
        fn = getattr(lib, name)
        fn.restype, fn.argtypes = result, argtypes
    return lib


class Face:
    def __init__(self, name, path, index, lib):
        self.name, self.path, self.index, self.lib = name, path, index, lib
        self.ot = TTFont(path, fontNumber=index)
        self.cmap = self.ot.getBestCmap()
        self.handle = None

    def set_size(self, size, weight):
        if self.handle:
            self.lib.source_close(self.handle)
        self.handle = self.lib.source_open(os.fsencode(self.path), self.index, size, weight)
        if not self.handle:
            raise ValueError(f"FreeType could not open {self.path}")

    def close(self):
        if self.handle:
            self.lib.source_close(self.handle)
            self.handle = None
        self.ot.close()

    def metrics(self):
        metrics = (C.c_int * 2)()
        self.lib.source_metrics(self.handle, metrics)
        return tuple(metrics)

    def advance(self, cp):
        advance = self.lib.source_advance(self.handle, cp)
        if advance < 0:
            raise ValueError(f"FreeType could not measure U+{cp:04X}")
        return (advance + 32) & ~63 if cjk(cp) else advance

    def bitmap(self, cp, phase, *, lcd=False, aligned=False):
        metrics = (C.c_int * 4)()
        pixels = (C.c_uint8 * (160 * 160))()
        args = (self.handle, cp, phase, metrics, pixels, len(pixels))
        size = (self.lib.source_raster_lcd(*args, aligned) if lcd
                else self.lib.source_raster(*args))
        if size < 0:
            raise ValueError(f"FreeType could not rasterize U+{cp:04X}")
        return tuple(metrics), bytes(pixels[:size])

    def layout(self, chars, script=0, kern=True):
        result = (C.c_int * (len(chars) + 1))()
        cp = (C.c_uint32 * len(chars))(*chars)
        if self.lib.source_layout(self.handle, cp, len(chars), script, kern, result) != 0:
            return None
        return tuple(result)


def kern_candidates(font, codepoints):
    """Expand just the nonzero PairPos records for the selected repertoire."""
    by_glyph = collections.defaultdict(list)
    for cp in codepoints:
        if cp in font.cmap:
            by_glyph[font.cmap[cp]].append(cp)

    if "kern" in font.ot:
        for table in font.ot["kern"].kernTables:
            for left, right in table.kernTable:
                for a in by_glyph.get(left, ()):
                    for b in by_glyph.get(right, ()):
                        yield a, b

    if "GPOS" not in font.ot:
        return
    gpos = font.ot["GPOS"].table
    indices = {i for rec in gpos.FeatureList.FeatureRecord if rec.FeatureTag == "kern"
               for i in rec.Feature.LookupListIndex}
    for i in sorted(indices):
        lookup = gpos.LookupList.Lookup[i]
        for sub in lookup.SubTable:
            kind = lookup.LookupType
            if kind == 9:
                kind, sub = sub.ExtensionLookupType, sub.ExtSubTable
            if kind != 2:
                continue
            if sub.Format == 1:
                for left, pairs in zip(sub.Coverage.glyphs, sub.PairSet, strict=True):
                    for pair in pairs.PairValueRecord:
                        for a in by_glyph.get(left, ()):
                            for b in by_glyph.get(pair.SecondGlyph, ()):
                                yield a, b
            else:
                classes = collections.defaultdict(list)
                for name, codes in by_glyph.items():
                    classes[sub.ClassDef2.classDefs.get(name, 0)].extend(codes)
                for left in sub.Coverage.glyphs:
                    if left not in by_glyph:
                        continue
                    row = sub.Class1Record[sub.ClassDef1.classDefs.get(left, 0)].Class2Record
                    for cl, pair in enumerate(row):
                        if not any(getattr(v, "XAdvance", 0) or getattr(v, "XPlacement", 0)
                                   or getattr(v, "XAdvDevice", None) or getattr(v, "XPlaDevice", None)
                                   for v in (pair.Value1, pair.Value2)):
                            continue
                        for a in by_glyph[left]:
                            for b in classes[cl]:
                                yield a, b


def winning_pairs(faces, mapping):
    choices = {}
    for i, face in enumerate(faces[:4]):
        # CJK letters use integral cells. Only punctuation can kern a CJK
        # pair; avoid expanding large ideograph classes against each other.
        for a, b in set(kern_candidates(face, mapping)):
            if not (cjk(a) or cjk(b)) and mapping[a] == mapping[b] == i:
                choices[a, b] = i
            elif (cjk(a) and punct(b) or cjk(b) and punct(a)) and i in (1, 2):
                if (a, b) not in choices or mapping[a] == mapping[b] == i:
                    choices[a, b] = i
    return choices


def pack_alpha(pixels, table=None):
    table = table or ALPHA
    result = bytearray()
    for offset in range(0, len(pixels), 2):
        low = table[pixels[offset]]
        high = table[pixels[offset + 1]] if offset + 1 < len(pixels) else 15
        result.append(low | high << 4)
    return result


ALPHA = tuple(15 - round(15 * (v / 255) ** GAMMA) for v in range(256))


def cjk_alignment(faces):
    # Match the optical centres of square ideographs from all supplements
    # to HarmonyOS SC's baseline at the same pixel size.
    def ink_center(face, cp):
        (_, top, _, rows), _ = face.bitmap(cp, 0)
        return 2 * top - rows

    return {
        i: round(median((ink_center(faces[1], cp) - ink_center(faces[i], cp)) / 2
                        for cp in map(ord, "田日国目口中封肖师字") if cp in faces[i].cmap))
        for i in (2, FACES.index("HarmonyOS Sans TC"), FACES.index("Noto Serif CJK SC"))
    }


def frequency_order(mapping):
    def decoded(encoding, leads, trails, last):
        for lead in leads:
            for trail in trails:
                if (lead, trail) <= last:
                    yield ord(bytes((lead, trail)).decode(encoding))

    gb = decoded("gb2312", range(0xb0, 0xd8), range(0xa1, 0xff), (0xd7, 0xf9))
    big5 = decoded("big5", range(0xa4, 0xc7),
                   (*range(0x40, 0x7f), *range(0xa1, 0xff)), (0xc6, 0x7e))
    common = tuple(cp for cp in dict.fromkeys((*gb, *big5)) if cp in mapping)
    common_set = set(common)
    rest = [cp for cp in mapping if cp not in common_set and cp != REPLACEMENT]
    rest.sort(key=lambda cp: not (cp < 0x80 or punct(cp) or 0x3040 <= cp <= 0x312f or
                                 0x31a0 <= cp <= 0x31ff or 0xac00 <= cp <= 0xd7a3))
    # A replacement is available even if the requested codepoint is absent.
    return (REPLACEMENT, *common, *rest), len(common) + 1


FIXED_RANGES = ((0x3400, 0x9fff), (0xac00, 0xd7a3), (0xf900, 0xfaff))
FIXED_TRAILER = struct.Struct("<4s7I")
BLOCK = 4096


def fixed_ordinal(cp):
    ordinal = 0
    for first, last in FIXED_RANGES:
        if first <= cp <= last:
            return ordinal + cp - first
        ordinal += last - first + 1
    return None


def lcd_bitmap(face, cp, phase):
    """Use identical LCD bounds for RGB coverage and its grayscale fallback."""
    (x, y, w, h), rgb = face.bitmap(cp, phase, lcd=True)
    gray = bytes((r + 2 * g + b + 2) // 4
                 for r, g, b in zip(rgb[0::3], rgb[1::3], rgb[2::3]))
    return (x, y, w, h), pack_alpha(gray) + pack_alpha(rgb)


def compile_font(path, faces, mapping, pairs, size, weight, fusion=None, stock=None):
    """With a stock glyph set, its glyphs and line metrics take precedence and
    every glyph keeps one phase and a whole-pixel advance, without kerning."""
    for face in faces:
        face.set_size(size, weight)

    # The HarmonyOS Sans SC baseline applies to every face. Keep its
    # ascender-to-descender proportion while matching the UI's nominal cell.
    ascent, source_height = faces[1].metrics()
    height = size + (size >= 18)
    ascent = min(height, max(1, ascent - (source_height - height + 1) // 2))
    if stock:
        height, ascent, pairs = stock.height, stock.ascent, {}
    cjk_shifts = cjk_alignment(faces)
    pixel = {cp: fusion[cp] for cp in mapping if ideograph(cp) and cp in fusion} if fusion else {}

    ordinal = {cp: i for i, cp in enumerate(mapping)}
    row = collections.defaultdict(dict)
    col = collections.defaultdict(dict)
    pair_count = 0
    for (a, b), i in sorted(pairs.items()):
        if a in pixel or b in pixel:
            continue
        face = faces[i]
        sc = int.from_bytes(b"Hani", "big") if cjk(a) or cjk(b) else 0
        with_kern = face.layout((a, b), sc)
        without_kern = face.layout((a, b), sc, False)
        if with_kern and without_kern:
            # The format implements horizontal pair advance kerning.
            adjust = with_kern[2] - without_kern[2]
            if adjust:
                row[a][b] = adjust
                col[b][a] = adjust
                pair_count += 1

    def classes(entries):
        unique = {(): 0}
        by_cp = {}
        for cp, values in entries.items():
            by_cp[cp] = unique.setdefault(tuple(sorted(values.items())), len(unique))
        if len(unique) > 256:
            raise ValueError("Too many kerning classes")
        return by_cp, tuple(unique)

    left, left_rows = classes(row)
    right, _ = classes(col)
    entries, offsets = bytearray(), []
    for values in left_rows:
        offsets.append(len(entries) // PAIR.size)
        for cl, adjust in sorted({right[cp]: d for cp, d in values}.items()):
            entries.extend(PAIR.pack(cl, adjust))
    offsets.append(len(entries) // PAIR.size)
    kern = struct.pack(f"<{len(offsets)}H", *offsets) + entries

    glyphs = {}
    max_record = max_advance = 0
    ink_above = ink_below = 0
    order, common_count = frequency_order(mapping)
    for cp in order:
        source = mapping[cp]
        if source is None:
            advance, record = 0, b"\x01\0\0\0\0"
        elif stock and cp in stock.glyphs:
            record, advance = stock.glyphs[cp]
            _, x, y, w, h = struct.unpack_from("<BbbBB", record)
            if w and h:
                ink_above = max(ink_above, y - ascent)
                ink_below = max(ink_below, ascent - y + h - height)
        else:
            face = faces[source]
            dot = pixel.get(cp)
            advance = dot.adv * 64 if dot else face.advance(cp)
            if stock:
                advance = (advance + 32) & ~63
            rgb = size == 12 and not stock
            phases = (0,) if cjk(cp) or stock else (0, 16, 32, 48)
            record = bytearray([len(phases) | (RECORD_RGB if rgb else 0)])
            for phase in phases:
                if rgb:
                    (x, y, w, h), bitmap = lcd_bitmap(face, cp, phase)
                elif dot:
                    (x, y, w, h), ink = glyph_bitmap(dot)
                    y = ascent - (height - h) // 2
                    bitmap = pack_alpha(ink)
                else:
                    (x, y, w, h), ink = face.bitmap(cp, phase)
                    if cjk(cp):
                        y += cjk_shifts.get(source, 0)
                    bitmap = pack_alpha(ink)
                if rgb and cjk(cp):
                    y += cjk_shifts.get(source, 0)
                if w and h:
                    ink_above = max(ink_above, y - ascent)
                    ink_below = max(ink_below, ascent - y + h - height)
                record.extend(struct.pack("<bbBB", x, y, w, h))
                record.extend(bitmap)
        max_record = max(max_record, len(record))
        max_advance = max(max_advance, (advance + 63) // 64)
        glyphs[cp] = (record, advance, left.get(cp, 0), right.get(cp, 0))

    anchors = b"".join(struct.pack("<I", cp) for cp in list(mapping)[::PAGE])
    index_at = HEADER.size + len(anchors) + len(kern)
    data_at = index_at + INDEX.size * len(mapping)
    fixed = {cp: fixed_ordinal(cp) for cp in order
             if fixed_ordinal(cp) is not None and not left.get(cp) and not right.get(cp)}
    hot = order[1:common_count]
    if not all(cp in fixed for cp in hot):
        raise ValueError("A common ideograph requires a variable cell")
    cold = [cp for cp in order[common_count:] if cp in fixed]
    cold.sort(key=lambda cp: not 0xac00 <= cp <= 0xd7a3)
    slots = (*hot, *cold)
    stride = (max(len(glyphs[cp][0]) for cp in slots) + 6 + 8 + 3) & ~3
    ranks = [0xffff] * sum(last - first + 1 for first, last in FIXED_RANGES)
    for rank, cp in enumerate(slots):
        ranks[fixed[cp]] = rank

    records = bytearray()
    entries = {}

    def align(tail=0):
        records.extend(b"\0" * (-(data_at + len(records) + tail) % BLOCK))

    def emit(cp, cell=False):
        record, advance, row, col = glyphs[cp]
        if cell:
            records.extend(struct.pack("<HHBB", len(record), advance, row, col))
        entries[cp] = INDEX.pack(cp, len(records), len(record), advance, row, col)
        records.extend(record)
        if cell:
            records.extend(b"\xff" * (stride - 6 - len(record)))

    align()
    map_at = data_at + len(records)
    records.extend(struct.pack(f"<{len(ranks)}H", *ranks))
    variable = sorted(cp for cp in mapping if cp not in fixed)
    variable_at = len(records)
    records.extend(b"\0" * (INDEX.size * len(variable)))
    emit(REPLACEMENT)
    align()
    hot_at = data_at + len(records)
    for cp in hot:
        emit(cp, cell=True)
    common_bytes = len(hot) * stride
    # Keep kana, punctuation and Hangul close to the front of the cold region.
    for cp in order[common_count:]:
        if cp not in fixed and (cp < 0x80 or punct(cp) or 0x3040 <= cp <= 0x31ff):
            emit(cp)
    align()
    cold_at = data_at + len(records)
    for cp in cold:
        emit(cp, cell=True)
    for cp in order[common_count:]:
        if cp not in entries:
            emit(cp)
    records[variable_at:variable_at + INDEX.size * len(variable)] = b"".join(
        entries[cp] for cp in variable)
    trailer = FIXED_TRAILER.pack(b"FIX1", map_at, hot_at, cold_at, stride,
                                 len(hot), len(slots), len(variable))
    bounds = trailer + b"INK1" + bytes((ink_above, ink_below))
    align(len(bounds))
    index = b"".join(entries[cp] for cp in mapping)
    header = HEADER.pack(b"PBF1", height, ascent, max_advance, max_record,
                         len(anchors) // 4, ordinal[REPLACEMENT],
                         len(mapping), len(left_rows),
                         index_at, data_at, data_at + len(records) + len(bounds))
    path.write_bytes(header + anchors + kern + index + records + bounds)
    return {"size": size, "weight": weight, "height": height, "ascent": ascent,
            "noto_cjk_shift": cjk_shifts[2], "ink_above": ink_above, "ink_below": ink_below,
            "max_record": max_record, "pairs": pair_count,
            "kern_bytes": len(kern), "bytes": path.stat().st_size,
            "common_count": len(hot), "common_bytes": common_bytes,
            "fixed_count": len(slots), "cell_bytes": stride, "map_bytes": len(ranks) * 2 + INDEX.size * len(variable)}


def stock_glyph_sets(directory):
    """The stock firmware glyph sets that stock.py converted, by basename."""
    from stock import GlyphSet
    return {path.stem: GlyphSet(path) for path in sorted(directory.glob("*.sgl"))
            if not path.stem.startswith("cjk")}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--fusion-cache", type=Path, default=FUSION_CACHE)
    parser.add_argument("--lang-dir", type=Path, default=ROOT / "apps-ipod/lang")
    parser.add_argument("--stock-dir", type=Path, default=ROOT / "apps-ipod/fonts/stock")
    parser.add_argument("--font", nargs=3, action="append", default=[],
                        metavar=("FAMILY", "FILE", "FACE_INDEX"))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="podbox-raster-") as temp:
        with ExitStack() as stack:
            lib = build_raster(Path(temp))
            fonts = []
            overrides = {name: (path, int(index)) for name, path, index in args.font}
            for name, path, index in locate_fonts(overrides):
                face = Face(name, path, index, lib)
                stack.callback(face.close)
                fonts.append(face)
            mapping = resolve(fonts, required_codepoints(args.lang_dir))
            pairs = winning_pairs(fonts, mapping)
            index_mapping = resolve(fonts, required_codepoints(args.lang_dir, full=False))
            index_pairs = {pair: face for pair, face in pairs.items()
                           if all(cp in index_mapping for cp in pair)}
            manifest = {
                "glyphs": len(mapping), "gamma": GAMMA,
                "faces": {f.name: {"index": f.index, "source_file": Path(f.path).name,
                                   "codepoints": [f"U+{cp:04X}" for cp, i in mapping.items()
                                                  if i == n]}
                          for n, f in enumerate(fonts)},
                "fonts": {},
            }
            generated = {}
            for name, spec in FONTS.items():
                path = args.output / (name + ".pbf")
                if spec in generated:
                    prev, info = generated[spec]
                    path.write_bytes(prev.read_bytes())
                else:
                    if spec[0] == 56:
                        info = compile_font(path, fonts, index_mapping, index_pairs, *spec)
                    else:
                        fusion = (load_fusion(spec[0], args.fusion_cache, Path(temp))
                                  if spec[0] in (8, 10) else None)
                        info = compile_font(path, fonts, mapping, pairs, *spec, fusion=fusion)
                    generated[spec] = path, info
                manifest["fonts"][name] = info
                print(name, info, flush=True)
            for name, stock in stock_glyph_sets(args.stock_dir).items():
                path = args.output / (name + ".pbf")
                info = compile_font(path, fonts, mapping, pairs, stock.size, stock.weight,
                                    stock=stock)
                info["stock_size64"] = stock.size64
                info["stock_glyphs"] = sum(mapping.get(cp) is not None for cp in stock.glyphs)
                info["stock_outline_glyphs"] = stock.own
                manifest["fonts"][name] = info
                print(name, info, flush=True)
            (args.output / "sources.json").write_text(
                json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
            )


if __name__ == "__main__":
    main()
