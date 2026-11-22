#!/usr/bin/env python3
"""Stage the upstream MD5Sum entry with the AstraPod filesystem adapter."""
from hashlib import sha256
from pathlib import Path
import sys

UPSTREAM = "5a675a6eefc783d1d7c76f7e16e97afae461b13165f3b9095e083ae5e03614c2"

source, adapter, output = map(Path, sys.argv[1:])
data = source.read_bytes()
if sha256(data).hexdigest() != UPSTREAM:
    raise SystemExit(f"{source}: review the MD5Sum adapter for this source")
header = data.decode().split("#define BUFFERSIZE", 1)[0].rstrip()
output.write_text(header + "\n\n" + adapter.read_text())
