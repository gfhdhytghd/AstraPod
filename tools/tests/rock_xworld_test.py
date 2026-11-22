#!/usr/bin/env python3
"""Run ASan/UBSan on the staged XWorld bank, memlist and operand readers."""
from pathlib import Path
import os
import shutil
import subprocess
import sys


root = Path(__file__).resolve().parents[2]
out = Path(sys.argv[1]).resolve()
out.mkdir(parents=True, exist_ok=True)
staged = out / "staged"
staged.mkdir(exist_ok=True)
for source in (root / "apps/plugins/xworld").iterdir():
    if source.suffix in {".c", ".h"}:
        shutil.copyfile(source, staged / source.name)
with (root / "apps-ipod/plugins/xworld-compat.patch").open("rb") as patch:
    subprocess.run(["patch", "--batch", "--forward", "--fuzz=0", "-s", "-p1"],
                   cwd=staged, stdin=patch, check=True)

fixture = root / "tools/tests/fixtures/rock-xworld"
binary = out / "probe"
subprocess.run([
    os.environ.get("CC", "cc"), "-std=gnu11", "-O1", "-g",
    "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
    "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections", "-no-pie",
    "-I", fixture, "-I", staged, "-I", root / "firmware/include",
    "-I", root / "firmware/export",
    fixture / "probe.c", staged / "bank.c", staged / "resource.c",
    staged / "file.c", "-o", binary,
], check=True)
subprocess.run([binary, out], check=True)
