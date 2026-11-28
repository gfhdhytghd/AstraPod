#!/usr/bin/env python3
"""Exercise the native ALAC size cache with a playback stream past the table."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]

HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

enum { TABLE = 0x849, FRAMES = 901 };
typedef struct {
    unsigned num_sample_byte_sizes;
    off_t sample_byte_sizes_offset;
} demux_res_t;

struct metadata { const char *path; };
static struct {
    off_t filesize, curpos;
    int audio_hid;
    struct metadata *id3;
    bool (*seek_buffer)(off_t);
    size_t (*read_filebuf)(void *, size_t);
    size_t (*alac_read_at)(off_t, void *, size_t);
} api, *ci = &api;

static int ring_fd;
static unsigned ring_seeks;

static bool ring_seek(off_t pos)
{
    ring_seeks++;
    ci->curpos = pos;
    return lseek(ring_fd, pos, SEEK_SET) == pos;
}

static size_t ring_read(void *buffer, size_t size)
{
    ssize_t bytes = read(ring_fd, buffer, size);
    assert(bytes >= 0);
    ci->curpos += bytes;
    return bytes;
}
'''

CHECKS = r'''
int main(int argc, char **argv)
{
    assert(argc == 2);
    struct metadata track = { argv[1] };
    ring_fd = open(track.path, O_CREAT | O_TRUNC | O_RDWR, 0600);
    assert(ring_fd >= 0);
    assert(lseek(ring_fd, TABLE, SEEK_SET) == TABLE);
    for (unsigned frame = 0; frame < FRAMES; frame++)
    {
        unsigned bytes = frame + 0x1000;
        unsigned char be[4] = { 0, 0, bytes >> 8, bytes };
        assert(write(ring_fd, be, sizeof be) == sizeof be);
    }

    ci->audio_hid = 42;
    ci->id3 = &track;
    ci->filesize = 0x2000000;
    ci->seek_buffer = ring_seek;
    ci->read_filebuf = ring_read;
    ci->alac_read_at = SIDE_READ;
    const demux_res_t demux = { FRAMES, TABLE };
    struct alac_sizes cache = { 0 };
    for (unsigned frame = 0; frame < FRAMES; frame++)
    {
        off_t expected = 0x9b710 + 0x5000 * frame;
        ci->curpos = expected;
        assert(alac_size(&cache, &demux, frame) == frame + 0x1000);
        assert(ci->curpos == expected);
    }
    if (ring_seeks)
        fprintf(stderr, "Sample table moved the playback cursor %u times\n", ring_seeks);
    assert(ring_seeks == 0);

    /* A truncated side read must return no packet size and leave playback
     * in place so the caller can fall back to the ARM decoder. */
    assert(ftruncate(ring_fd, TABLE + 4 * 8) == 0);
    cache.count = 0;
    off_t packet = ci->curpos;
    assert(!alac_size(&cache, &demux, 0));
    assert(ci->curpos == packet && ring_seeks == 0);

    /* The offline converter has a private stream and still restores it. */
    ci->audio_hid = -1;
    const demux_res_t tiny = { 8, TABLE };
    assert(alac_size(&cache, &tiny, 0) == 0x1000);
    assert(ci->curpos == packet && ring_seeks == 2);
    puts("ALAC: seven full windows and tail, zero playback seeks, short read and offline restore PASS");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT)
    args = parser.parse_args()
    codec = (args.source / 'lib/rbcodec/codecs/alac.c').read_text()
    size_cache = codec[codec.index('struct alac_sizes {'):codec.index('#endif\n\nCODEC_HEADER')]
    thread = (args.source / 'apps-ipod/audio/codec_thread.c').read_text()
    side_read = ''
    callback = 'NULL'
    if 'static int alac_table_fd' in thread:
        side_read = thread[thread.index('static int alac_table_fd'):
                           thread.index('static bool codec_alac_begin')]
        callback = 'codec_alac_read_at'
    with tempfile.TemporaryDirectory(prefix='alac-size-cache-') as temp:
        temp = Path(temp)
        source = temp / 'probe.c'
        source.write_text(HARNESS + "#define ci api\n" + side_read
                          + "#undef ci\n" + size_cache
                          + CHECKS.replace('SIDE_READ', callback))
        binary = temp / 'probe'
        subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Werror',
                        str(source), '-o', str(binary)], check=True)
        subprocess.run([binary, temp / 'track.m4a'], cwd=temp, check=True)


if __name__ == '__main__':
    main()
