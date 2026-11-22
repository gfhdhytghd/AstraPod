#!/usr/bin/env python3
"""Check XRick's resource ownership and PCM against a local Rockbox data pack."""
import argparse
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("data", type=Path, help="extracted .rockbox/xrick directory")
parser.add_argument("output", type=Path)
options = parser.parse_args()
output = options.output.resolve()
output.mkdir(parents=True, exist_ok=True)

root = Path(__file__).resolve().parents[2]
stock = root / "apps/plugins/xrick"
glue = root / "apps-ipod/plugins"
fixtures = glue / "xrick-tests"

sources = [
    stock / name for name in (
        "draw.c", "ents.c", "maps.c", "res_magic.c", "resources.c",
        "scr_xrick.c", "util.c", "data/img.c", "data/pics.c",
        "data/sounds.c", "data/sprites.c", "data/tiles.c",
        "system/sysfile_rockbox.c", "system/sysmem_rockbox.c",
    )
]
sources += [fixtures / "probe.c", glue / "xrick-validate.c", glue / "xrick-audio.c"]
probe = output / "xrick-host"
subprocess.run([
    "cc", "-std=gnu11", "-g", "-O1", "-no-pie", "-DASTRAPOD_XRICK",
    "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
    "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
    f"-I{fixtures}", f"-I{glue}", f"-I{stock.parent}", *sources, "-o", probe,
], check=True)
subprocess.run([probe, options.data.resolve(), "valid"], check=True)
print("PASS XRick resources, arena cleanup, PCM at 44.1/48 kHz, pause and revoke")
