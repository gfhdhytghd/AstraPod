#!/usr/bin/env python3
"""Check a local Han glyph overlay against its original firmware ZIP.

The checked ranges come from the original glyph offsets, independently of
the rasterizer. Header bytes, untouched pixels, tables and file size must
agree exactly. All replacement cells must contain visible ink.
"""
from collections import Counter
from hashlib import sha256
import json
from pathlib import Path
from struct import unpack_from
import sys
from zipfile import ZipFile

from fontTools.unicodedata import script

base, directory = map(Path, sys.argv[1:])
report = json.loads((directory / "report.json").read_text())
fonts = [path for path in directory.glob(".rockbox/fonts/*.fnt")]
assert len(fonts) == len(report["fonts"]) == 11
seen = set()
with ZipFile(base) as package:
    for path in sorted(fonts):
        name = path.relative_to(directory).as_posix()
        old, new = package.read(name), path.read_bytes()
        stats = report["fonts"][name]
        assert len(old) == len(new)
        assert sha256(old).hexdigest() == stats["base_sha256"]
        assert sha256(new).hexdigest() == stats["overlay_sha256"]
        _, _, height, _, _, first, default, count, bits, _, _ = unpack_from("<4s4H6I", old)
        assert bits >= 0xFFDB
        offsets_start = (36 + bits + 3) & ~3
        addresses = unpack_from(f"<{count}I", old, offsets_start)
        widths = old[offsets_start + 4 * count:]
        assert len(widths) == count
        default_addr = addresses[default - first]
        uses = Counter(addresses)
        masked = bytearray(new)
        slots = changed_slots = 0
        for index, (address, width) in enumerate(zip(addresses, widths)):
            cp = first + index
            if address == default_addr or script(chr(cp)) != "Hani":
                continue
            assert uses[address] == 1
            pos = address + 36
            end = pos + (width * height + 1) // 2
            glyph = new[pos:end]
            visible_samples = [pixel for byte in glyph for pixel in (byte & 15, byte >> 4)]
            assert min(visible_samples[:width * height]) < 15, (name, hex(cp))
            changed_slots += glyph != old[pos:end]
            slots += 1
            masked[pos:end] = old[pos:end]

        assert slots == sum(stats["covered"].values())
        assert slots == 9477 and changed_slots == stats["changed"] >= slots * .99
        assert masked == old, name
        print(f"{name}: {changed_slots} changed Han cells, {slots} covered, other bytes exact")
        seen.add(name)
    assert seen == set(report["fonts"])
