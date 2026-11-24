#!/usr/bin/env python3
"""Check the firmware's ALAC container parsing without a connected player."""
import sys
from albumart.host import ROOT, OUT, FLAGS, compile_sources, link, run
FLAGS.extend("-I" + str(ROOT / "lib/rbcodec/codecs" / part) for part in ("", "lib"))

source = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "system.h"
#include "file.h"
#include "metadata.h"
#include "metadata_parsers.h"
#include "libm4a/m4a.h"

static struct codec_api api;
static unsigned char *input;
static union {
    max_align_t align;
    unsigned char bytes[1 << 20];
} arena;
static size_t used;

void *codec_malloc_spare(size_t *size)
{
    *size = sizeof(arena.bytes) - used;
    return arena.bytes + used;
}

void *codec_malloc(size_t size)
{
    size = (size + 15) & ~(size_t)15;
    if (size > sizeof(arena.bytes) - used)
        return NULL;
    void *result = arena.bytes + used;
    used += size;
    return result;
}

void codec_free(void *ptr) { (void)ptr; }
void yield(void) {}
void debugf(const char *fmt, ...) { (void)fmt; }
void panicf(const char *fmt, ...) { (void)fmt; abort(); }

static bool seek_buffer(size_t pos)
{
    if (pos > (size_t)api.filesize)
        return false;
    api.curpos = pos;
    return true;
}

static void advance_buffer(size_t bytes)
{
    if (!seek_buffer(api.curpos + bytes))
        api.curpos = api.filesize;
}

static void *request_buffer(size_t *size, size_t requested)
{
    *size = api.filesize - api.curpos;
    if (*size > requested)
        *size = requested;
    return input + api.curpos;
}

static size_t read_filebuf(void *dst, size_t bytes)
{
    size_t size;
    unsigned char *data = request_buffer(&size, bytes);
    memcpy(dst, data, size);
    advance_buffer(size);
    return size;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    int fd = open(argv[1], O_RDONLY);
    assert(fd >= 0);
    struct mp3entry id3 = {0};
    assert(get_mp4_metadata(fd, &id3));
    assert(id3.codectype == AFMT_MP4_ALAC);

    api.id3 = &id3;
    api.filesize = ffilesize(fd);
    api.seek_buffer = seek_buffer;
    api.advance_buffer = advance_buffer;
    api.request_buffer = request_buffer;
    api.read_filebuf = read_filebuf;
    input = calloc(1, api.filesize + 8);
    assert(input);
    assert(lseek(fd, 0, SEEK_SET) == 0);
    assert(read(fd, input, api.filesize) == api.filesize);
    close(fd);
    printf("metadata rate=%lu bits=%u length_ms=%lu samples=%lu media_bytes=%lu\n",
           id3.frequency, id3.bits_per_sample, id3.length, id3.samples, id3.filesize);

    stream_t stream;
    demux_res_t demux = {0};
    stream_create(&stream, &api);
    int success = qtmovie_read(&stream, &demux);
    printf("demux=%d cookie_bytes=%u samples=%u chunks=%u stts=%u mdat_payload=%d\n",
           success, demux.codecdata_len, demux.num_sample_byte_sizes,
           demux.num_chunks, demux.num_time_to_samples, demux.mdat_offset);
    if (success)
    {
        assert(demux.codecdata_len >= 48);
        assert(demux.codecdata[29] == id3.bits_per_sample);
        uint64_t elapsed;
        uint32_t sample, lookup = 0;
        assert(m4a_seek(&demux, &stream, id3.samples / 2,
                        &elapsed, &sample, &lookup));
        printf("seek sample=%u elapsed_samples=%llu pos=%ld\n", sample,
               (unsigned long long)elapsed, (long)api.curpos);
    }
    free(input);
    return !success;
}
'''
files = [
    "lib/rbcodec/metadata/" + name + ".c"
    for name in ("mp4", "metadata_common", "replaygain", "id3tags")
]
files += [
    "lib/rbcodec/codecs/libm4a/demux.c",
    "lib/rbcodec/codecs/libm4a/m4a.c",
    "apps-ipod/system/strutil.c",
    "firmware/common/unicode.c",
    "firmware/common/strcasestr.c",
    "lib/fixedpoint/fixedpoint.c",
    "tools/tests/albumart/host_io.c",
]
probe = link("alac-demux", source, compile_sources(files))

fixtures = []
for btrt in ("0", "1"):
    path = OUT / f"alac-btrt-{btrt}.m4a"
    run([
        "ffmpeg", "-v", "error", "-nostdin", "-y",
        "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000",
        "-t", "1.25", "-ac", "2", "-c:a", "alac", "-sample_fmt", "s32p",
        "-movflags", "+faststart", "-f", "mp4", "-brand", "M4A ",
        "-write_btrt", btrt, path,
    ], f"encode-{btrt}")
    fixtures.append(path)

# Moving the bitrate atom must not change the location of the audio data.
data = fixtures[1].read_bytes()
cookie = data.index(b"\0\0\0$alac")
bitrate = cookie + 36
assert data[bitrate:bitrate + 8] == b"\0\0\0\x14btrt"
reordered = OUT / "alac-btrt-first.m4a"
reordered.write_bytes(data[:cookie] + data[bitrate:bitrate + 20]
                      + data[cookie:bitrate] + data[bitrate + 20:])
fixtures.append(reordered)

for i, path in enumerate(fixtures + sys.argv[2:]):
    result = run([probe, path], f"alac-fixture-{i}")
    print(path, result.stdout, sep="\n", end="")
