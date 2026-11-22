#!/usr/bin/env python3
"""Check XRick against an extracted stock .rockbox/xrick directory. GPL v2+."""
import argparse
from pathlib import Path
import shutil
import struct
import subprocess
import zlib

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("stock", type=Path)
parser.add_argument("output", type=Path)
args = parser.parse_args()
here = Path(__file__).resolve().parent
root = here.parents[2]
args.output.mkdir(parents=True, exist_ok=True)
stock = args.output.resolve() / "staged"
xrick = stock / "xrick"
subprocess.run([
    "python3", here.parent / "stage-xrick.py", root / "apps/plugins/xrick",
    here.parent / "xrick-stage.patch", xrick,
], check=True)
probe = args.output.resolve() / "xrick-host"
sources = [
    xrick / name for name in (
        "draw.c", "ents.c", "maps.c", "res_magic.c", "resources.c",
        "scr_xrick.c", "util.c", "data/img.c", "data/pics.c",
        "data/sounds.c", "data/sprites.c", "data/tiles.c",
        "system/sysfile_rockbox.c", "system/sysmem_rockbox.c",
    )
]
sources += [here / "probe.c", here.parent / "xrick-validate.c",
            here.parent / "xrick-audio.c"]
subprocess.run([
    "cc", "-std=gnu11", "-g", "-O1", "-no-pie", "-DASTRAPOD_XRICK",
    "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
    "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
    f"-I{here}", f"-I{here.parent}", f"-I{stock}", *sources, "-o", probe,
], check=True)


def reseal(data):
    return data[:-4] + struct.pack("<I", zlib.crc32(data[:-4]))


def overwrite(offset, replacement):
    return lambda data: reseal(data[:offset] + replacement +
                               data[offset + len(replacement):])


def u16(offset, value):
    return overwrite(offset, struct.pack("<H", value))


def check(name, relative, change):
    target = fixture / relative
    source = args.stock / relative
    target.write_bytes(change(source.read_bytes()))
    try:
        subprocess.run([probe, fixture, "invalid"], check=True)
    finally:
        shutil.copyfile(source, target)
    print(f"PASS {name}", flush=True)


fixture = args.output / "malformed"
shutil.copytree(args.stock, fixture, dirs_exist_ok=True)
subprocess.run([probe, args.stock, "valid"], check=True)
print("PASS stock resources, arena unwind, PCM 44.1/48 kHz, pause and revoke", flush=True)
check("CRC", "misc/maps.dat", lambda d: d[:-1] + bytes([d[-1] ^ 1]))
check("truncated archive", "misc/maps.dat", lambda d: d[:5])
check("oversized archive", "misc/maps.dat", lambda d: d + bytes(128 * 1024))
check("CRC-valid trailing payload", "misc/maps.dat", lambda d: reseal(d[:-4] + b"x" + d[-4:]))
check("map submap bound", "misc/maps.dat", u16(16, 65535))
check("map row bound", "misc/maps.dat", u16(14, 256))
check("missing connector sentinel", "misc/connect.dat",
      lambda d: reseal(d[:10] + bytes(len(d) - 14) + d[-4:]))
check("invalid submap page", "misc/submaps.dat", u16(10, 256))
check("flags expansion", "misc/eflgc.dat", overwrite(10, b"\xff"))
check("mark entity bound", "misc/marks.dat", overwrite(11, b"\xff"))
check("entity sprite sequence bound", "misc/entdata.dat", u16(10 + 22 * 9 + 2, 65535))
check("intro step bound", "misc/imapsofs.dat", overwrite(10, b"\xff"))
check("intro text partial allocation unwind", "texts/imaptext.dat", u16(8, 65535))
check("intro text early terminator", "texts/imaptext.dat", overwrite(12, b"\xfe"))
check("palette bound", "graphics/palette.dat", u16(8, 65535))
check("image geometry", "graphics/imgsplash.dat", u16(8, 65535))
check("image palette index", "graphics/imgsplash.dat", u16(16, 1))
check("picture geometry", "graphics/picsplash.dat", u16(8, 319))
check("asset path traversal", "filelist.dat", overwrite(10, b"../"))
check("asset path NUL", "filelist.dat", overwrite(10, b"\0"))
check("WAV short payload", "sounds/tune0.wav", lambda d: d[:-1])
check("WAV oversized declaration", "sounds/tune0.wav",
      lambda d: d[:40] + struct.pack("<I", 0xffffffff) + d[44:])
