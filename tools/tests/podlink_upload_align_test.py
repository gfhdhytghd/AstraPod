#!/usr/bin/env python3
"""Host regression: an MS1 upload hands the file system aligned sector runs.

Compiles podlink_media.c with the PodLink stubs and the iPod Classic's
32-byte cache line. The write() stub splits each call the way file.c does,
into head bytes, whole sectors and tail bytes, and requires every run of
whole sectors to start on a cache line; an unaligned run is what goes
through fat.c's shared bounce buffer. It also checks the stored bytes and
that the upload ends in MS1 STORED. No device or disk access.
"""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(os.environ.get('PODLINK_TEST_ROOT', Path(__file__).resolve().parents[2]))
OUT = Path(tempfile.mkdtemp(prefix='podlink-upload-align-'))

code = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "system.h"
#undef STORAGE_ALIGN_ATTR
#undef STORAGE_PAD
#undef STORAGE_OVERLAP
#define CACHEALIGN_SIZE 32
#define STORAGE_ALIGN_ATTR __attribute__((aligned(CACHEALIGN_SIZE)))
#define STORAGE_PAD(x) ((x) + CACHEALIGN_SIZE - 1)
#define STORAGE_OVERLAP(x) ((x) & (CACHEALIGN_SIZE - 1))
#include "file.h"
#include "dir.h"
#undef open
#undef close
#undef write
#undef fsync
#undef remove
#undef rename
#undef file_exists
#undef mkdir
#undef opendir
#undef readdir
#undef closedir
#undef dir_get_info

#define FILE_MAX (256 * 1024)
static unsigned char file_data[FILE_MAX];
static unsigned int file_pos, sector_size, runs, bad_runs;
static bool file_open, stored;

static int test_open(const char *path, int flags, ...)
{
    (void)path; (void)flags;
    assert(!file_open);
    file_open = true;
    file_pos = 0;
    return 3;
}
static int test_close(int fd) { assert(fd == 3 && file_open); file_open = false; return 0; }
static int test_fsync(int fd) { assert(fd == 3); return 0; }
static int test_remove(const char *path) { (void)path; return 0; }
static int test_rename(const char *from, const char *to)
{
    (void)from; (void)to;
    stored = true;
    return 0;
}
static bool test_file_exists(const char *path) { (void)path; return false; }
static int test_mkdir(const char *path) { (void)path; return 0; }
static DIR *test_opendir(const char *path) { (void)path; return NULL; }
static struct dirent *test_readdir(DIR *dir) { (void)dir; return NULL; }
static int test_closedir(DIR *dir) { (void)dir; return 0; }
static struct dirinfo test_dir_get_info(DIR *dir, struct dirent *entry)
{
    (void)dir; (void)entry;
    struct dirinfo info;
    memset(&info, 0, sizeof(info));
    return info;
}

/* file.c's split: head bytes up to a sector boundary through the stream
 * cache, then whole sectors straight from the caller's buffer. */
static ssize_t test_write(int fd, const void *buf, size_t size)
{
    assert(fd == 3 && file_open && file_pos + size <= FILE_MAX);
    size_t head = (sector_size - file_pos % sector_size) % sector_size;
    if (head > size)
        head = size;
    if (size - head >= sector_size)
    {
        runs++;
        if (((uintptr_t)buf + head) & (CACHEALIGN_SIZE - 1))
            bad_runs++;
    }
    memcpy(file_data + file_pos, buf, size);
    file_pos += size;
    return size;
}

#define open test_open
#define close test_close
#define write test_write
#define fsync test_fsync
#define remove test_remove
#define rename test_rename
#define file_exists test_file_exists
#define mkdir test_mkdir
#define opendir test_opendir
#define readdir test_readdir
#define closedir test_closedir
#define dir_get_info test_dir_get_info

#include "podlink_media.c"

bool test_thread_context = true;
long current_tick;
void podlink_wake(void) {}
bool podlink_library_active(void) { return false; }
void podlink_library_close(void) {}
const char *podlink_library_command(const unsigned char *data, size_t size,
                                    bool *refresh, unsigned char *binary,
                                    unsigned int *binary_size, uint32_t *raw)
{
    (void)data; (void)size; (void)refresh; (void)binary;
    (void)binary_size; (void)raw;
    return "MS3 ERR command";
}
uint32_t podlink_profile_now(void) { return 0; }
void podlink_profile_add(enum podlink_stage stage, uint32_t start, unsigned bytes)
{
    (void)stage; (void)start; (void)bytes;
}
bool podlink_profile_start(bool fast) { (void)fast; return false; }
void podlink_profile_stop(struct podlink_stage_sample *out,
                          uint32_t *idle_us, uint32_t *wall_us)
{
    (void)out; (void)idle_us; (void)wall_us;
}

static unsigned char message_buf[PODLINK_MAX_MESSAGE + 1];

static const char *send(enum podlink_type type,
                        const void *data, unsigned int length)
{
    static char reply[MEDIA_REPLY_SIZE + 1];
    bool refill;
    /* An odd address shows the mailbox copy, not the sender, decides. */
    memcpy(message_buf + 1, data, length);
    struct podlink_message message = {
        .type = type, .length = length, .data = message_buf + 1,
    };
    assert(podlink_media_enqueue(&message));
    podlink_media_work();
    reply[podlink_media_next(reply, MEDIA_REPLY_SIZE, &refill)] = '\0';
    return reply;
}

static void upload(const unsigned int *chunks, unsigned int count)
{
    static unsigned char source[FILE_MAX];
    static unsigned char chunk[PODLINK_MAX_MESSAGE];
    unsigned int total = 0;
    for (unsigned int i = 0; i < count; i++)
        total += chunks[i];
    assert(total <= FILE_MAX);
    for (unsigned int i = 0; i < total; i++)
        source[i] = (unsigned char)(i * 131 + (i >> 9));
    uint32_t crc = crc_32r(source, total, UINT32_MAX) ^ UINT32_MAX;

    char put[160];
    snprintf(put, sizeof(put), "MS1 PUT %08x %08x %064x.flac", total,
             (unsigned)crc, 0);
    stored = false;
    assert(!strcmp(send(PODLINK_TEXT, put, strlen(put)), "MS1 READY"));
    unsigned int offset = 0;
    for (unsigned int i = 0; i < count; i++)
    {
        memcpy(chunk, "MSD1", 4);
        memcpy(chunk + 4, source + offset, chunks[i]);
        assert(!strcmp(send(PODLINK_BINARY, chunk, chunks[i] + 4), "MS1 ACK"));
        offset += chunks[i];
    }
    assert(!strcmp(send(PODLINK_TEXT, "MS1 END", 7), "MS1 STORED"));
    assert(stored && file_pos == total && !memcmp(file_data, source, total));
}

int main(void)
{
    static const unsigned int chunks[] = {
        PODLINK_MAX_MESSAGE - 4, PODLINK_MAX_MESSAGE - 4, 1000, 7, 4093,
        PODLINK_MAX_MESSAGE - 4, 513, PODLINK_MAX_MESSAGE - 4, 8191,
    };
    podlink_media_init();
    for (sector_size = 512; sector_size <= 4096; sector_size *= 8)
        upload(chunks, ARRAYLEN(chunks));
    if (bad_runs)
    {
        printf("FAIL: %u of %u whole-sector runs start off a cache line\n",
               bad_runs, runs);
        return 1;
    }
    printf("PASS: %u whole-sector runs from MS1 chunks, all cache aligned, "
           "for 512 and 4096 byte sectors\n", runs);
    return 0;
}
'''


def main():
    source = OUT / 'podlink_upload_align.c'
    binary = OUT / 'podlink_upload_align'
    source.write_text(code)
    cmd = [os.environ.get('CC', 'cc'), '-std=gnu11', '-g', '-O1', '-DCHECKWPS',
           '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
           '-I', ROOT / 'tools/tests/podlink_stubs',
           '-I', ROOT / 'firmware/export',
           '-idirafter', ROOT / 'firmware/include',
           '-I', ROOT / 'firmware/target/hosted',
           '-I', ROOT / 'apps-ipod',
           '-I', ROOT / 'apps-ipod/screens/system',
           source, ROOT / 'firmware/common/crc32.c', '-o', binary]
    subprocess.run(list(map(str, cmd)), check=True, cwd=OUT)
    result = subprocess.run([str(binary)], cwd=OUT, text=True,
                            capture_output=True)
    print(result.stdout + result.stderr, end='')
    raise SystemExit(result.returncode)


main()
