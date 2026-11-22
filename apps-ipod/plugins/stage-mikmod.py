#!/usr/bin/env python3
"""Stage the bounded foreground front end for the upstream MikMod engine."""
from pathlib import Path
import sys


source, adapter, target = map(Path, sys.argv[1:])
original = source.read_text()
start = "char* mmsupp_strncat("
stop = "/************************* File Access ***************************/"
if original.count(start) != 1 or original.count(stop) != 1:
    raise SystemExit("review the MikMod compatibility source anchors")
wrappers = original[original.index(start):original.index(stop)]
target.write_text(
    '#define NO_MMSUPP_DEFINES\n#include "plugin.h"\n#include "mikmod.h"\n'
    '#define LINE_LENGTH 80\n\n' + wrappers + adapter.read_text()
)
