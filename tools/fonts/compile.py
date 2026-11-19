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

ROOT = Path(__file__).resolve().parents[2]
HEADER = struct.Struct("<4s6H5I")
INDEX = struct.Struct("<IIHHBB")
PAIR = struct.Struct("<Bh")
PAGE = 64
GAMMA = 0.90

# Nominal pixel sizes come from the existing basenames and theme roles.
# Preserve the old basenames to read settings and themes from previous releases.
FONTS = {
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
)
REPLACEMENT = 0xFFFD
COVERAGE = "\u00ad‐‑‒–—―‘’‚“”„†‡•…‰′″‹›※、。・「」『』【】《》！（），．：；？～·￥✓✗←↑→↓▶⏸♫⌘␣□"


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


def required_codepoints(lang_dir):
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
    return result


def ideograph(cp):
    return 0x3400 <= cp <= 0x9FFF or 0xF900 <= cp <= 0xFAFF or 0x20000 <= cp <= 0x323AF


def cjk(cp):
    return ideograph(cp) or 0x3040 <= cp <= 0x30FF or 0xAC00 <= cp <= 0xD7AF or 0x3100 <= cp <= 0x312F


def punct(cp):
    return ud.category(chr(cp)).startswith("P")


def invisible(cp):
    return ud.category(chr(cp)) in {"Cc", "Cf", "Zl", "Zp"}


def resolve(fonts, required):
    sources = {}
    missing = []
    for cp in sorted(required):
        if invisible(cp):
            sources[cp] = None
            continue
        order = ((0, 1, 3, 2) if cp < 0x300 and not cjk(cp) else (1, 0, 2, 3))
        source = next((i for i in (*order, *range(4, len(fonts))) if cp in fonts[i].cmap), None)
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
        "source_close": (None, C.c_void_p),
        "source_metrics": (None, C.c_void_p, C.POINTER(C.c_int)),
        "source_advance": (C.c_int, C.c_void_p, C.c_uint32),
        "source_raster": (C.c_int, C.c_void_p, C.c_uint32, C.c_int,
                          C.POINTER(C.c_int), C.POINTER(C.c_uint8), C.c_uint),
        "source_layout": (C.c_int, C.c_void_p, C.POINTER(C.c_uint32), C.c_uint,
                          C.c_int, C.c_int, C.POINTER(C.c_int)),
    }
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

    def bitmap(self, cp, phase):
        metrics = (C.c_int * 4)()
        pixels = (C.c_uint8 * (160 * 160))()
        size = self.lib.source_raster(self.handle, cp, phase, metrics, pixels, len(pixels))
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
        for a, b in set(kern_candidates(face, mapping)):
            if not (cjk(a) or cjk(b)) and mapping[a] == mapping[b] == i:
                choices[a, b] = i
            elif (cjk(a) and punct(b) or cjk(b) and punct(a)) and i in (1, 2):
                if (a, b) not in choices or mapping[a] == mapping[b] == i:
                    choices[a, b] = i
    return choices


def pack_alpha(pixels):
    result = bytearray()
    for offset in range(0, len(pixels), 2):
        low = ALPHA[pixels[offset]]
        high = ALPHA[pixels[offset + 1]] if offset + 1 < len(pixels) else 15
        result.append(low | high << 4)
    return result


ALPHA = tuple(15 - round(15 * (v / 255) ** GAMMA) for v in range(256))


def compile_font(path, faces, mapping, pairs, size, weight):
    for face in faces:
        face.set_size(size, weight)

    # The HarmonyOS Sans SC baseline applies to every face. Keep its
    # ascender-to-descender proportion while matching the UI's nominal cell.
    ascent, source_height = faces[1].metrics()
    height = size + (size >= 18)
    ascent = min(height, max(1, ascent - (source_height - height + 1) // 2))
    # Give the CJK supplement the same optical centre as HarmonyOS, using
    # ordinary square ideographs present in both faces at this ppem.
    def ink_center(face, cp):
        (_, top, _, rows), _ = face.bitmap(cp, 0)
        return 2 * top - rows

    cjk_shift = round(median((ink_center(faces[1], cp) - ink_center(faces[2], cp)) / 2
                             for cp in map(ord, "田日国目口中封肖师字")))

    ordinal = {cp: i for i, cp in enumerate(mapping)}
    row = collections.defaultdict(dict)
    col = collections.defaultdict(dict)
    pair_count = 0
    for (a, b), i in sorted(pairs.items()):
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

    records = bytearray()
    index = bytearray()
    max_record = max_advance = 0
    for cp, source in mapping.items():
        if source is None:
            advance, record = 0, b"\x01\0\0\0\0"
        else:
            face = faces[source]
            advance = face.advance(cp)
            phases = (0,) if cjk(cp) else (0, 16, 32, 48)
            record = bytearray([len(phases)])
            for phase in phases:
                (x, y, w, h), bitmap = face.bitmap(cp, phase)
                if source == 2 and cjk(cp):
                    y += cjk_shift
                record.extend(struct.pack("<bbBB", x, y, w, h))
                record.extend(pack_alpha(bitmap))
        max_record = max(max_record, len(record))
        max_advance = max(max_advance, (advance + 63) // 64)
        index.extend(INDEX.pack(cp, len(records), len(record), advance,
                                left.get(cp, 0), right.get(cp, 0)))
        records.extend(record)

    anchors = b"".join(struct.pack("<I", cp) for cp in list(mapping)[::PAGE])
    index_at = HEADER.size + len(anchors) + len(kern)
    data_at = index_at + len(index)
    header = HEADER.pack(b"PBF1", height, ascent, max_advance, max_record,
                         len(anchors) // 4, ordinal[REPLACEMENT],
                         len(mapping), len(left_rows),
                         index_at, data_at, data_at + len(records))
    path.write_bytes(header + anchors + kern + index + records)
    return {"size": size, "weight": weight, "height": height, "ascent": ascent,
            "noto_cjk_shift": cjk_shift,
            "max_record": max_record, "pairs": pair_count,
            "kern_bytes": len(kern), "bytes": path.stat().st_size}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--lang-dir", type=Path, default=ROOT / "apps-ipod/lang")
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
                    info = compile_font(path, fonts, mapping, pairs, *spec)
                    generated[spec] = path, info
                manifest["fonts"][name] = info
                print(name, info, flush=True)
            (args.output / "sources.json").write_text(
                json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
            )


if __name__ == "__main__":
    main()
