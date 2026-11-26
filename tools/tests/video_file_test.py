#!/usr/bin/env python3
"""Which files the video player opens, on generated MP4s; no player needed.

mp4_has_video() from lib/rbcodec/metadata/mp4.c and filetype_is_video() from
apps-ipod/files/filetypes.c, compiled with sanitizers against the ipod6g
simulator's headers by the albumart harness (tools/tests/albumart/host.py).

    python3 tools/tests/video_file_test.py [scratch dir]
"""
import re
import struct
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent / "albumart"))
from host import ROOT, OUT, run, compile_sources, link, function

FIX = OUT / "video-fixtures"
FIX.mkdir(exist_ok=True)


def box(kind, payload=b""):
    return struct.pack(">I4s", 8 + len(payload), kind) + payload


def trak(handler, entry, entry_size):
    hdlr = box(b"hdlr", bytes(8) + handler + bytes(12) + b"\0")
    stsd = box(b"stsd", struct.pack(">II", 0, 1) + box(entry, bytes(entry_size)))
    minf = box(b"minf", box(b"vmhd", bytes(12)) + box(b"stbl", stsd))
    return box(b"trak", box(b"tkhd", bytes(84))
               + box(b"mdia", box(b"mdhd", bytes(24)) + hdlr + minf))


FTYP = box(b"ftyp", b"isom" + bytes(4) + b"isommp41")
MDAT = box(b"mdat", bytes(32))
SOUND = trak(b"soun", b"mp4a", 28)
FILM = trak(b"vide", b"avc1", 78)
PICTURES = trak(b"vide", b"jpeg", 78)


def moov(*traks):
    return box(b"moov", box(b"mvhd", bytes(100)) + b"".join(traks))


files = {
    "song.mp4": FTYP + moov(SOUND) + MDAT,
    "film.mp4": FTYP + moov(FILM, SOUND) + MDAT,
    "FILM.MP4": FTYP + moov(SOUND, FILM) + MDAT,
    "moov-last.mp4": FTYP + MDAT + moov(FILM),
    "chapters.mp4": FTYP + moov(SOUND, PICTURES) + MDAT,
    "film.m4a": FTYP + moov(FILM, SOUND) + MDAT,
    "broken.mp4": FTYP + box(b"moov", box(b"trak", b"\0\0\0\x30mdia")),
    "empty.mp4": b"",
}
for name, data in files.items():
    (FIX / name).write_bytes(data)

attrs = dict(re.findall(r"#define (FILE_ATTR_(?:AUDIO|VIDEO|TXT|MASK))\s+(0x[0-9A-Fa-f]+)",
                        (ROOT / "apps-ipod/files/filetypes.h").read_text()))
assert len(attrs) == 4, attrs

source = r'''
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "system.h"
#include "file.h"
#include "metadata.h"
#include "lib/rbcodec/metadata/mp4.c"
void yield(void) {}
void debugf(const char *fmt, ...) { (void)fmt; }
void panicf(const char *fmt, ...) { (void)fmt; abort(); }
#define HAVE_VIDEO_PLAYER
''' + "".join("#define %s %s\n" % kv for kv in attrs.items()) + \
    function("apps-ipod/files/filetypes.c", "filetype_is_video") + r'''
int main(int argc, char **argv)
{
    static const int attr[] = { FILE_ATTR_AUDIO, FILE_ATTR_VIDEO, FILE_ATTR_TXT };
    assert(argc == 3);
    printf("%d\n", filetype_is_video(argv[1], attr[atoi(argv[2])]));
    return 0;
}
'''
files_c = ["lib/rbcodec/metadata/" + s + ".c" for s in
           ["id3tags", "flac", "vorbis", "ogg", "metadata_common", "replaygain", "mp3data"]]
files_c += ["apps-ipod/system/strutil.c", "firmware/common/unicode.c",
            "firmware/common/strcasestr.c", "firmware/common/pathfuncs.c",
            "lib/fixedpoint/fixedpoint.c", "tools/tests/albumart/host_io.c"]
probe = link("video_file", source, compile_sources(files_c))

AUDIO, VIDEO, TXT = 0, 1, 2
cases = [
    ("song.mp4", AUDIO, 0),          # music in an .mp4 stays music
    ("film.mp4", AUDIO, 1),          # an .mp4 with a film is a video
    ("FILM.MP4", AUDIO, 1),          # whatever the case, the film second
    ("moov-last.mp4", AUDIO, 1),     # tables after the data
    ("chapters.mp4", AUDIO, 0),      # chapter pictures are not a film
    ("film.m4a", AUDIO, 0),          # only .mp4 is opened to look
    ("broken.mp4", AUDIO, 0),
    ("empty.mp4", AUDIO, 0),
    ("clip.m4v", VIDEO, 1),          # an .m4v by its attribute alone
    ("film.mp4", TXT, 0),            # not audio: never opened
]
failed = []
for name, attr, expected in cases:
    result = run([probe, FIX / name, attr], "video-" + name, check=False)
    ok = result.returncode == 0 and result.stdout.strip() == str(expected)
    print(name, attr, "PASS" if ok else "FAIL " + result.stdout + result.stderr,
          flush=True)
    if not ok:
        failed.append(name)
assert not failed, failed
