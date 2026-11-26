#!/usr/bin/env python3
"""Check bundled PBF1 coverage against offline TagCache and lyric fixtures."""
import argparse
import json
from pathlib import Path
import struct
from zipfile import ZipFile

from check import BitmapFont
from compile import FONTS, ROOT, cjk, required_codepoints, stock_glyph_sets

# These are the transcriptions to check, not identifications of the songs.
PHOTO_CANDIDATES = "囉個驚妳謊揮卻暈遺忘臉"


def database_strings(path):
    for file in sorted(path.glob("database_*.tcd")):
        if not file.stem.rsplit("_", 1)[1].isdigit():
            continue
        data = file.read_bytes()
        endian = "<" if data[:4] == b"\x10HCT" else ">"
        magic, _, count = struct.unpack_from(endian + "iii", data)
        assert magic == 0x54434810 and count >= 0, file
        offset = 12
        for _ in range(count):
            length, _ = struct.unpack_from(endian + "ii", data, offset)
            offset += 8
            assert 0 < length <= len(data) - offset, file
            text, sep, _ = data[offset:offset + length].partition(b"\0")
            assert sep, file
            offset += length
            yield text.decode("utf-8")
        assert offset == len(data), file


def gb2312():
    codec = set()
    for lead in range(0xA1, 0xF8):
        for trail in range(0xA1, 0xFF):
            try:
                codec.add(ord(bytes((lead, trail)).decode("gb2312")))
            except UnicodeDecodeError:
                continue
    return codec


def describe(codepoints):
    return [f"U+{cp:04X} {chr(cp)}" for cp in sorted(codepoints)]


def check(font_dir, baseline, database, fixtures):
    gb = gb2312()
    database_rows = list(database_strings(database))
    assert database_rows, database
    db = {ord(ch) for row in database_rows for ch in row}
    lyrics = {ord(ch) for file in fixtures for ch in file.read_text(encoding="utf-8-sig")}
    lyrics.update(map(ord, PHOTO_CANDIDATES))
    required = required_codepoints(ROOT / "apps-ipod/lang")
    old = BitmapFont(baseline.read(".rockbox/fonts/20-noto-sans.pbf"))
    assert gb <= old.index.keys()
    result = {"tag_records": len(database_rows), "tag_codepoints": len(db), "lyric_codepoints": len(lyrics),
              "gb2312_count": len(gb), "baseline_gb2312_missing": [],
              "photo_candidates": {ch: {"codepoint": f"U+{ord(ch):04X}", "in_gb2312": ord(ch) in gb,
                                        "in_baseline": ord(ch) in old.index} for ch in PHOTO_CANDIDATES},
              "fonts": {}}
    for name in [*FONTS, *stock_glyph_sets(ROOT / "apps-ipod/fonts/stock")]:
        font = BitmapFont(font_dir / (name + ".pbf"))
        index_only = name.endswith("-index")
        missing = (db | lyrics) - font.index.keys()
        assert index_only or required <= font.index.keys(), name
        assert index_only or not any(cjk(cp) for cp in missing), (name, describe(missing))
        above = below = 0
        for cp in font.index:
            for (_, top, width, height), _ in font.phases(cp):
                if width and height:
                    above = max(above, top - font.ascent)
                    below = max(below, font.ascent - top + height - font.height)
        assert font.data.endswith(b"INK1" + bytes((above, below))), name
        result["fonts"][name] = {"codepoints": font.count, "bytes": len(font.data),
                                 "ink_bounds": [-above, font.height + below],
                                 "missing": describe(missing)}
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("fonts", type=Path)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("database", type=Path)
    parser.add_argument("fixtures", nargs="+", type=Path)
    args = parser.parse_args()
    with ZipFile(args.baseline) as baseline:
        print(json.dumps(check(args.fonts, baseline, args.database, args.fixtures),
                         ensure_ascii=False, indent=2))
