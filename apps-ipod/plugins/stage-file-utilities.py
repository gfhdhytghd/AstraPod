#!/usr/bin/env python3
"""Stage the AstraPod adapters for the two upstream file utilities."""
from hashlib import sha256
from pathlib import Path
import re
import sys

DIGESTS = {
    "stats.c": "e4ce0bd3b455b49c88c2a82d87ee0f4e63f2eb6b97d8e81788fbca8276f37dce",
    "search.c": "681372195d3d840ec14aeb95d0cc12ed81bc284085120c81dbf2b8cc45c98072",
    "mul_id3.c": "309509d47bd2a6a590518c937c6c7a43f509b2fa81822ef95ce04928ce848a42",
}


def checked_source(path):
    data = path.read_bytes()
    if sha256(data).hexdigest() != DIGESTS[path.name]:
        raise SystemExit(f"{path}: review the AstraPod file adapter for this source")
    return data.decode()


source, adapter, output, *dependencies = map(Path, sys.argv[1:])
original = checked_source(source)
header = original[:original.index('#include "plugin.h"')]
staged = header + '#include "plugin.h"\n'
if dependencies:
    library = checked_source(dependencies[0])
    for name in ("image_exts", "video_exts"):
        staged += re.search(rf"static const char \*{name}\[\] = .*?;", library)[0] + "\n"

output.write_text(staged + adapter.read_text())
