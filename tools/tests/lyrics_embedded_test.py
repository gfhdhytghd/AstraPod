"""Read real audio containers through the firmware's lyrics document on the host."""
import argparse
import ast
import hashlib
from pathlib import Path
import re
import struct
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("output", type=Path)
parser.add_argument("--baseline", type=Path, help="FU58 source snapshot for negative controls")
args = parser.parse_args()
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
repo = Path(__file__).resolve().parents[2]


def encode(name, codec, *options):
    subprocess.run([
        "ffmpeg", "-nostdin", "-v", "error", "-y",
        "-f", "lavfi", "-i", "anullsrc=r=44100:cl=mono", "-t", "0.15",
        "-c:a", codec, *options, out / name,
    ], check=True)


timed = "\ufeff[offset:100]\r\n[00:01.00]晚风来了\r\n[00:02.00]<00:02.00>夜色<00:03.00>渐深"
plain = "\ufeff[Verse]\r\n中文歌词\r\nLast line"
encode("timed.m4a", "alac", "-metadata", f"lyrics={timed}")
encode("plain.m4a", "aac", "-metadata", f"lyrics={plain}", "-movflags", "+faststart")
encode("empty.m4a", "aac")
encode("large.m4a", "alac", "-metadata", "lyrics=" + "".join(
    f"[{n // 60:02d}:{n % 60:02d}.00]第{n:04d}行 " + "a" * 88 + "\r\n"
    for n in range(700)
))
encode("timed.flac", "flac", "-metadata", f"unsyncedlyrics={timed}")
encode("plain.flac", "flac", "-metadata", f"LYRICS={plain}")
encode("cross-page.ogg", "libvorbis", "-metadata", "comment=" + "c" * 66000,
       "-metadata", f"LYRICS={timed}")
encode("audio.mp3", "libmp3lame", "-id3v2_version", "0")
audio = (out / "audio.mp3").read_bytes()
assert not audio.startswith(b"ID3")


def syncsafe(value):
    return bytes((value >> shift) & 127 for shift in (21, 14, 7, 0))


def frame(name, body, version=4, flags=0):
    if version == 2:
        return name + len(body).to_bytes(3, "big") + body
    size = syncsafe(len(body)) if version == 4 else struct.pack(">I", len(body))
    return name + size + struct.pack(">H", flags) + body


def id3(name, frames, version=4, flags=0, footer=False):
    header = bytes((version, 0, flags | (0x10 if footer else 0))) + syncsafe(len(frames))
    (out / name).write_bytes(b"ID3" + header + frames + (b"3DI" + header if footer else b"") + audio)


def unsync(body):
    return re.sub(b"\xff(?=[\xe0-\xff\x00]|$)", b"\xff\x00", body)


uslt = frame(b"USLT", b"\x03engdescription\0" + plain.encode())
id3("plain.mp3", uslt, footer=True)
utf16 = lambda text: b"\xff\xfe" + text.encode("utf-16-le")
id3("utf16.mp3", frame(b"USLT", b"\x01eng" + utf16("中文描述\0")
                        + utf16("中文🎵\r\nLast line"), version=3), version=3)
id3("legacy.mp3", frame(b"ULT", b"\x00eng\0Caf\xe9\r\nLast line", version=2), version=2)
id3("unsynced.mp3", frame(b"USLT", unsync(b"\xff" + syncsafe(0)
    + b"\x00eng\0Caf\xff\xe0\r\nLast line"), flags=0x43))
id3("large.mp3", frame(b"USLT", b"\x03eng\0" + (b"long lyrics\r\n" * 6000)))
id3("timed.mp3", uslt + frame(b"SYLT", b"\x03eng\x02\x01description\0"
    + "晚风来了".encode() + b"\0" + struct.pack(">I", 1000)
    + "\n夜色渐深".encode() + b"\0" + struct.pack(">I", 2100)))
id3("bad-sylt.mp3", frame(b"SYLT", b"\x03eng\x02\x01no terminator") + uslt)
id3("bad-uslt.mp3", frame(b"USLT", b"\x03engno terminator"))

# Keep a valid audio stream but damage the inner data atom's declared length.
bad = bytearray((out / "timed.m4a").read_bytes())
at = bad.index(b"\xa9lyr") + 4
assert bad[at + 4:at + 8] == b"data"
bad[at:at + 4] = struct.pack(">I", 0xfffffff0)
(out / "bad.m4a").write_bytes(bad)

# Extend the codec-independent harness with the production tag helpers.
# Use source slices so the harness needs no codec, audio buffer or simulator.
pre_ast = ast.parse((repo / "tools/tests/lyrics_document_test.py").read_text())
pre = next(ast.literal_eval(node.value) for node in pre_ast.body
           if isinstance(node, ast.Assign) and node.targets[0].id == "pre")
meta = repo / "lib/rbcodec/metadata"
mp4 = (meta / "mp4.c").read_text()
vorbis = (meta / "vorbis.c").read_text()
ogg = (meta / "ogg.c").read_text()
id3src = (meta / "id3tags.c").read_text()
helpers = r"""
#define LYRICS_EMBEDDED_FIXTURES
#include <limits.h>
#define FOURCC(a,b,c,d) ((uint32_t)(a)<<24|(uint32_t)(b)<<16|(uint32_t)(c)<<8|(uint32_t)(d))
#define DEBUGF(...) do {} while (0)
"""
helpers += pre
helpers += """
static uint32_t get_long_le(const void *b){
    const unsigned char *p=b;
    return (uint32_t)p[3]<<24|(uint32_t)p[2]<<16|(uint32_t)p[1]<<8|p[0];
}
static int32_t get_slong(const void *b){return get_long_le(b);}
"""
helpers += re.search(r"static unsigned long unsync\([^}]+}", id3src).group() + "\n"
helpers += id3src[id3src.index("int getid3v2len(int fd)"):id3src.index("#ifdef DEBUG_STANDALONE", id3src.index("int getid3v2len(int fd)"))]
helpers = re.sub(r"\bunsync\(", "metadata_unsync(", helpers)
helpers = re.sub(r"^    logf\(.*\n", "", helpers, flags=re.M)
helpers += id3src[id3src.index("int id3_unsynchronize("):id3src.index("static int unsynchronize_frame")]
helpers += ogg[ogg.index("int get_ogg_format_and_move_to_comments("):
               ogg.index("/* A simple parser")]
helpers += vorbis[vorbis.index("static bool file_read_page_header("):
                  vorbis.index("#define B64_START_CHAR")]
helpers += mp4[mp4.index("#define MP4_3gp6"):mp4.index("/* Read the tag data")]

main = r"""
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s: %s\n", argv[1], #c); return 1; } } while (0)

static void sidecar(const char *path, const char *value)
{
    FILE *f = fopen(path, "w");
    assert(f && fputs(value, f) >= 0 && !fclose(f));
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    const char *file = argv[1];
    struct mp3entry song = {.length = 60000};
    int n = snprintf(song.path, sizeof song.path, "%s/%s", argv[2], file);
    assert(n > 0 && n < (int)sizeof song.path);
    if (strstr(file, ".m4a"))
        song.codectype = strstr(file, "timed") || strstr(file, "large") ? AFMT_MP4_ALAC : AFMT_MP4_AAC;
    else if (strstr(file, ".mp3"))
        song.codectype = AFMT_MPA_L3;
    else if (strstr(file, ".flac"))
        song.codectype = AFMT_FLAC;
    else
        song.codectype = AFMT_OGG_VORBIS;

    if (!strcmp(file, "empty.m4a") || !strcmp(file, "bad.m4a")
        || !strcmp(file, "bad-uslt.mp3"))
    {
        CHECK(lyrics_load(&song) == LYRICS_NOT_FOUND);
        CHECK(!lyrics_loaded() && !lyrics_count());
    }
    else
    {
        CHECK(lyrics_load(&song) == LYRICS_OK);
        CHECK(lyrics_loaded() && !strcmp(lyrics_file(), song.path));
        if (!strcmp(file, "timed.mp3"))
        {
            CHECK(lyrics_timed() && lyrics_count() == 2);
            CHECK(!strcmp(lyrics_text(0), "晚风来了") && !strcmp(lyrics_text(1), "夜色渐深"));
            CHECK(lyrics_time(0) == 1000 && lyrics_time(1) == 2100);
        }
        else if (!strcmp(file, "large.m4a"))
        {
            CHECK(lyrics_timed() && lyrics_count() > 200 && lyrics_count() < 700);
            CHECK(!strncmp(lyrics_text(0), "第0000行 ", strlen("第0000行 ")));
            CHECK(lyrics_time(lyrics_count()-1) == 1000L * (lyrics_count()-1));
            CHECK(strlen(lyrics_text(lyrics_count()-1)) == strlen(lyrics_text(0)));
        }
        else if (!strcmp(file, "large.mp3"))
        {
            CHECK(!lyrics_timed() && lyrics_count() > 500 && lyrics_count() < 6000);
            CHECK(!strcmp(lyrics_text(0), "long lyrics"));
        }
        else if (strstr(file, "timed.") || strstr(file, "cross-page"))
        {
            CHECK(lyrics_timed() && lyrics_count() == 2);
            CHECK(!strcmp(lyrics_text(0), "晚风来了") && !strcmp(lyrics_text(1), "夜色渐深"));
            CHECK(lyrics_index_at(1099) == LYRICS_NONE && lyrics_index_at(1100) == 0);
            CHECK(lyrics_time(1) == 2100 && lyrics_word_count(1) == 2);
            CHECK(lyrics_word_time(1, 1) == 3100 && !strcmp(lyrics_word_text(1, 1), "渐深"));
        }
        else if (!strcmp(file, "utf16.mp3") || !strcmp(file, "legacy.mp3")
                 || !strcmp(file, "unsynced.mp3"))
        {
            const char *first = !strcmp(file, "legacy.mp3") ? "Café"
                              : !strcmp(file, "unsynced.mp3") ? "Cafÿà" : "中文🎵";
            CHECK(!lyrics_timed() && lyrics_count() == 2);
            CHECK(!strcmp(lyrics_text(0), first) && !strcmp(lyrics_text(1), "Last line"));
        }
        else
        {
            CHECK(!lyrics_timed() && lyrics_count() == 3);
            CHECK(!strcmp(lyrics_text(0), "[Verse]") && !strcmp(lyrics_text(1), "中文歌词"));
            CHECK(!strcmp(lyrics_text(2), "Last line"));
        }
    }

    if (!strcmp(file, "timed.m4a"))
    {
        char txt[MAX_PATH], lrc_path[MAX_PATH];
        CHECK(snprintf(txt, sizeof txt, "%s/sub/timed.txt", argv[2]) < (int)sizeof txt);
        CHECK(snprintf(lrc_path, sizeof lrc_path, "%s/timed.lrc", argv[2]) < (int)sizeof lrc_path);
        CHECK(!mkdir("sub", 0700));
        CHECK(snprintf(song.path, sizeof song.path, "%s/sub/timed.m4a", argv[2]) < (int)sizeof song.path);
        CHECK(!rename("timed.m4a", "sub/timed.m4a"));
        sidecar(txt, "Plain sidecar wins\n");
        CHECK(lyrics_load(&song) == LYRICS_OK && !lyrics_timed());
        CHECK(!strcmp(lyrics_text(0), "Plain sidecar wins"));
        sidecar(lrc_path, "[00:00.00]Timed sidecar wins\n");
        CHECK(lyrics_load(&song) == LYRICS_OK && lyrics_timed());
        CHECK(!strcmp(lyrics_text(0), "Timed sidecar wins"));
        CHECK(!unlink(lrc_path) && !unlink(txt));
        CHECK(!rename("sub/timed.m4a", "timed.m4a") && !rmdir("sub"));
        CHECK(allocs == 1);
    }

    lyrics_close();
    CHECK(frees == 1);
    puts("PASS");
}
"""


def build(tree, name, old=False):
    directory = tree / "apps-ipod/viewers/lyric_viewer"
    material = "".join((directory / f"lyrics.{ext}").read_text() for ext in ("h", "c"))
    material = re.sub(r'^#include ".*\n', "", material, flags=re.M)
    source = out / f"{name}.c"
    binary = out / name
    legacy = ""
    if old:
        legacy = """
static unsigned char *utf16LEdecode(const unsigned char*s,unsigned char*d,int n){while(n--){*d++=*s;s+=2;}return d;}
static unsigned char *utf16BEdecode(const unsigned char*s,unsigned char*d,int n){while(n--){*d++=s[1];s+=2;}return d;}
"""
    source.write_text(helpers + legacy + material + main)
    subprocess.run(["cc", "-m32", "-std=gnu11", "-g", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-function", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                    "-fno-pie", "-no-pie", source, "-o", binary], check=True)
    return binary


fixtures = sorted(out.glob("*.m4a")) + sorted(out.glob("*.flac")) + sorted(out.glob("*.ogg"))
fixtures += sorted(path for path in out.glob("*.mp3") if path.name != "audio.mp3")
original = {path: hashlib.sha256(path.read_bytes()).digest() for path in fixtures}
binary = build(repo, "embedded")
for path in fixtures:
    subprocess.run([binary, path.name, out], cwd=out, check=True)
    print("PASS production lyrics input", path.name, flush=True)
assert all(hashlib.sha256(path.read_bytes()).digest() == sha for path, sha in original.items())

if args.baseline:
    binary = build(args.baseline, "fu58", old=True)
    for name in ("plain.m4a", "timed.m4a", "plain.flac", "timed.flac", "cross-page.ogg",
                 "plain.mp3", "utf16.mp3", "legacy.mp3"):
        result = subprocess.run([binary, name, out], cwd=out, capture_output=True, text=True)
        assert result.returncode == 1 and result.stderr == (
            f"FAIL {name}: lyrics_load(&song) == LYRICS_OK\n"
        ), result
        print("PASS FU58 negative control", name)
