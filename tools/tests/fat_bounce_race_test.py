#!/usr/bin/env python3
"""Host regression for fat.c's shared bounce buffer; no device access.

Compiles fat.c's bounce buffer block, transfer() and fat_init() unchanged
against a stub storage driver that blocks inside the transfer, the way the
iPod Classic's ATA driver waits on DMA. A second thread then starts its own
unaligned transfer on the same volume. The first thread's sectors, or the
bytes it reads, must still be its own.
"""
import os
from pathlib import Path
import sys
import tempfile

ROOT = Path(os.environ.get('ART_TEST_ROOT', Path(__file__).resolve().parents[2]))
OUT = Path(tempfile.mkdtemp(prefix='fat-bounce-tests-'))
os.environ['ART_TEST_ROOT'] = str(ROOT)
os.environ['ART_TEST_OUT'] = str(OUT)
sys.path.insert(0, str(Path(__file__).with_name('albumart')))
from support import build, function, run  # noqa: E402

code = r'''
#include <pthread.h>
#include <time.h>
#define STORAGE_NEEDS_BOUNCE_BUFFER
#define NUM_VOLUMES 1
#define SECTOR_SIZE 512
#define CACHEALIGN_SIZE 32
#define STORAGE_ALIGN_ATTR __attribute__((aligned(CACHEALIGN_SIZE)))
#define STORAGE_OVERLAP(x) ((x) & (CACHEALIGN_SIZE - 1))
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#define IF_MD(...)
#define IF_MV(...)
#define IF_MV_VOL(v) 0
#define LOG_SECTOR_SIZE(bpb) SECTOR_SIZE
#define DEBUGF(...) ((void)0)
#define panicf(...) (fprintf(stderr, __VA_ARGS__), abort())
#define dc_lock_cache() ((void)0)
#define dc_unlock_cache() ((void)0)
#define dc_discard_all(...) ((void)0)
typedef uint64_t sector_t;

struct mutex { pthread_mutex_t m; };
static void mutex_init(struct mutex *m)
{
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&m->m, &attr);
}
static void mutex_lock(struct mutex *m) { assert(!pthread_mutex_lock(&m->m)); }
static void mutex_unlock(struct mutex *m) { assert(!pthread_mutex_unlock(&m->m)); }

struct bpb {
    sector_t startsector, firstdatasector, totalsectors;
    bool mounted;
};
static struct bpb fat_bpbs[NUM_VOLUMES];

#define DISK_SECTORS 1024
static uint8_t disk[DISK_SECTORS][SECTOR_SIZE];

/* Thread A is the one the driver blocks; thread B's copy into the bounce
 * buffer is what it waits for. */
enum role { MAIN, A, B };
static __thread enum role role;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t ata_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool a_in_driver, b_copied;
static void *last_xfer_buf;

static void *test_memcpy(void *d, const void *s, size_t n)
{
    memcpy(d, s, n);
    if (role == B)
    {
        pthread_mutex_lock(&lock);
        b_copied = true;
        pthread_cond_broadcast(&cond);
        pthread_mutex_unlock(&lock);
    }
    return d;
}

/* Thread A waits here for B's copy, as the ATA driver waits on DMA with its
 * mutex held. A fixed transfer keeps B out, so this times out. */
static void driver_block(void)
{
    if (role != A)
        return;
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_nsec += 300 * 1000000L;
    if (until.tv_nsec >= 1000000000L)
    {
        until.tv_sec++;
        until.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&lock);
    a_in_driver = true;
    pthread_cond_broadcast(&cond);
    while (!b_copied)
        if (pthread_cond_timedwait(&cond, &lock, &until))
            break;
    pthread_mutex_unlock(&lock);
}

static int storage_write_sectors(sector_t start, int count, const void *buf)
{
    assert(start + count <= DISK_SECTORS && !STORAGE_OVERLAP((uintptr_t)buf));
    pthread_mutex_lock(&ata_mutex);
    last_xfer_buf = (void *)buf;
    driver_block();
    memcpy(disk[start], buf, count * SECTOR_SIZE);
    pthread_mutex_unlock(&ata_mutex);
    return 0;
}

static int storage_read_sectors(sector_t start, int count, void *buf)
{
    assert(start + count <= DISK_SECTORS && !STORAGE_OVERLAP((uintptr_t)buf));
    pthread_mutex_lock(&ata_mutex);
    last_xfer_buf = buf;
    memcpy(buf, disk[start], count * SECTOR_SIZE);
    driver_block();
    pthread_mutex_unlock(&ata_mutex);
    return 0;
}

#define memcpy test_memcpy
'''

FAT = 'firmware/common/fat.c'
text = (ROOT / FAT).read_text()
start = text.index('#ifdef STORAGE_NEEDS_BOUNCE_BUFFER\n#if defined(MAX_VIRT_SECTOR_SIZE)')
end = text.index('#endif\n', text.index('#define FAT_BOUNCE_BUFFER', start)) + 7
code += '\n#line %d "%s"\n' % (text[:start].count('\n') + 1, FAT) + text[start:end]
code += function(FAT, 'transfer')
code += function(FAT, 'fat_init')
code += r'''
#undef memcpy

struct job {
    enum role role;
    sector_t start;
    int count;
    bool write;
    uint8_t *buf;
};

static void *run_job(void *arg)
{
    struct job *j = arg;
    role = j->role;
    assert(transfer(&fat_bpbs[0], j->start, j->count, (char *)j->buf,
                    j->write) == 0);
    return NULL;
}

static uint8_t storage_a[3 * SECTOR_SIZE + 64] __attribute__((aligned(64)));
static uint8_t storage_b[3 * SECTOR_SIZE + 64] __attribute__((aligned(64)));

/* A runs its transfer and is held inside the driver; B starts only then. */
static void race(struct job *a, struct job *b)
{
    pthread_t ta, tb;
    a_in_driver = b_copied = false;
    assert(!pthread_create(&ta, NULL, run_job, a));
    pthread_mutex_lock(&lock);
    while (!a_in_driver)
        pthread_cond_wait(&cond, &lock);
    pthread_mutex_unlock(&lock);
    assert(!pthread_create(&tb, NULL, run_job, b));
    assert(!pthread_join(ta, NULL) && !pthread_join(tb, NULL));
}

static int count_not(const uint8_t *p, size_t n, uint8_t v)
{
    int bad = 0;
    for (size_t i = 0; i < n; i++)
        bad += p[i] != v;
    return bad;
}

int main(void)
{
    alarm(10);
    fat_bpbs[0].startsector = 0;
    fat_bpbs[0].firstdatasector = 8;
    fat_bpbs[0].totalsectors = DISK_SECTORS;
    fat_init();

    /* Two unaligned writers: A's sectors must hold A's bytes. */
    uint8_t *a = storage_a + 4, *b = storage_b + 12;
    memset(a, 'A', 2 * SECTOR_SIZE);
    memset(b, 'B', 2 * SECTOR_SIZE);
    struct job wa = { A, 100, 2, true, a }, wb = { B, 200, 2, true, b };
    race(&wa, &wb);
    assert(last_xfer_buf != a && last_xfer_buf != b);
    int bad_write = count_not(disk[100], 2 * SECTOR_SIZE, 'A');
    assert(!count_not(disk[200], 2 * SECTOR_SIZE, 'B'));

    /* An unaligned reader racing a writer must read the disk's bytes. */
    memset(disk[300], 'R', 2 * SECTOR_SIZE);
    memset(a, 0, 2 * SECTOR_SIZE);
    memset(b, 'W', 2 * SECTOR_SIZE);
    struct job ra = { A, 300, 2, false, a }, rb = { B, 400, 2, true, b };
    race(&ra, &rb);
    int bad_read = count_not(a, 2 * SECTOR_SIZE, 'R');
    assert(!count_not(disk[400], 2 * SECTOR_SIZE, 'W'));

    /* The lock is released: a later transfer on this thread proceeds. */
    struct job again = { MAIN, 500, 3, true, b };
    run_job(&again);
    assert(!count_not(disk[500], 2 * SECTOR_SIZE, 'W'));

    if (bad_write || bad_read)
    {
        printf("FAIL: %d of A's written bytes and %d of its read bytes "
               "came from B\n", bad_write, bad_read);
        return 1;
    }
    puts("PASS: concurrent unaligned write/write and read/write through "
         "one bounce buffer");
    return 0;
}
'''
print(run([build('fat-bounce', code)], 'fat-bounce'), end='')
print('Logs:', OUT)
