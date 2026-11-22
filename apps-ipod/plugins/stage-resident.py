#!/usr/bin/env python3
"""Stage the foreground entries for the two resident plugin services."""
from hashlib import sha256
from pathlib import Path
import sys


DIGESTS = {
    "battery_bench": "b285168e2de70485edd77d08dc8d1fec2b72c48a8ccde03b79397fbf58b7d5d0",
    "announce_status": "14378c586954508c91d126d4fd701fa3f07fb92e168920d013f7cea542c27702",
}

source, adapter, output = map(Path, sys.argv[1:])
data = source.read_bytes()
if sha256(data).hexdigest() != DIGESTS[source.stem]:
    raise SystemExit(f"{source}: review the AstraPod resident adapter")
notice = data.decode().split("\n\n#include", 1)[0]
output.write_text(f'{notice}\n\n#include "plugin.h"\n' + adapter.read_text())
