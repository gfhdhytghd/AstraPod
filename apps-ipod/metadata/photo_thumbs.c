/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Thumbnails of the pictures copied to the player as files, made on the
 * player while Photos is open, and kept so that the next visit draws them
 * at once.
 *
 * A thumbnail is the picture fitted inside PHOTO_THUMB_EDGE square, centred
 * on black, as iTunes makes the Photo Database's. The cache lives in
 * LIB_CACHE_DIR/photos: thumbs.dat holds the thumbnails' pixels back to back
 * and thumbs.idx one record per thumbnail made, keyed by two hashes of the
 * path with the file's size and time, so a file that is changed, renamed or
 * moved is made again. A picture no reader decodes has a record too, and is
 * not tried again until it changes. The index is read whole when Photos
 * opens; an index too large for that, or of another version, is discarded
 * with the thumbnails. A record that a later one supersedes stays as waste
 * until then.
 *
 * Screen copies: a picture shown full screen is decoded at screen size,
 * which the JPEG reader does by scaling in the DCT. A decode that took longer
 * than SLOW_DECODE leaves its result in photos/screen, so the next showing
 * reads it rather than decoding again.
 *
 * One worker thread does the decoding and the writing, for as long as Photos
 * is open. It runs below the interface and yields after every row it
 * decodes, and a screen copy someone is waiting for, then a thumbnail in
 * view, cut in on whatever it is doing. The interface reads thumbnails from
 * memory, or from thumbs.dat behind the one lock both threads take to use
 * that file.
 *
 * Parts, in order:
 *   - the cache on disk
 *   - decoding
 *   - the worker and what it chooses to do next
 *   - the interface's side
 ****************************************************************************/
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "thread.h"
#include "file.h"
#include "dir.h"
#include "usb.h"
#include "lcd.h"
#include "draw/bmp.h"
#include "draw/resize.h"
#include "draw/image_load.h"
#include "system/library_files.h"
#include "photo_thumbs.h"

#define PHOTOS_DIR      LIB_CACHE_DIR "/photos"
#define SCREEN_DIR      PHOTOS_DIR "/screen"
#define THUMBS_DATA     PHOTOS_DIR "/thumbs.dat"
#define THUMBS_INDEX    PHOTOS_DIR "/thumbs.idx"
#define THUMBS_MAGIC    0x4d485450u     /* "PTHM" */
#define SCREEN_MAGIC    0x52435350u     /* "PSCR" */
/* Bump when the meaning of the pixels changes; the cache is then discarded */
#define THUMBS_VERSION  1

#define THUMB_PIXELS (PHOTO_THUMB_EDGE * PHOTO_THUMB_EDGE)
#define THUMB_BYTES (THUMB_PIXELS * (int)sizeof(fb_data))
#define SCREEN_PIXELS (LCD_WIDTH * LCD_HEIGHT)
#define THUMB_NONE (-1)
#define THUMB_FAILED (-2)

/* Thumbnails held in memory, for the grid and its neighbours */
#define RAM_SLOTS 64
/* Files either side of those in view that are made next, and kept in memory */
#define NEARBY 15
/* A decode slower than this leaves a screen copy on disk */
#define SLOW_DECODE (HZ * 3 / 10)

/* A full-screen decode: the bitmap and the scaler's lines, the readers'
 * working memory, and the marks an EXIF quarter turn takes */
#define WORK_BYTES (BM_SCALED_SIZE(LCD_WIDTH, LCD_HEIGHT, FORMAT_NATIVE, 0) + \
                    IMAGE_DECODE_OVERHEAD + SCREEN_PIXELS / 8 + 4096)
#define STACK_BYTES (DEFAULT_STACK_SIZE + 0x2000)

#define EV_WAKE 1
#define EV_EXIT 2

struct index_head {
    uint32_t magic;
    uint16_t version;
    uint16_t edge;
    uint32_t pad[2];
};

/* One thumbnail made, or one picture found unreadable (slot < 0) */
struct index_entry {
    uint32_t hash;
    uint32_t check;
    uint32_t size;
    uint32_t mtime;
    int32_t slot;
    uint8_t w, h;
    uint16_t pad;
};

struct screen_head {
    uint32_t magic;
    uint16_t version;
    uint16_t pad;
    uint32_t check;
    uint32_t size;
    uint32_t mtime;
};

struct ram_slot {
    int file;
    unsigned stamp;
};

enum job { JOB_NONE, JOB_THUMB, JOB_SCREEN };

static struct {
    struct photo_folders *pf;
    fb_data *ram_pixels;
    struct ram_slot ram[RAM_SLOTS];
    unsigned stamp;
    unsigned char *work;
    fb_data *tile;              /* a finished thumbnail, on its way out */
    fb_data *screen;            /* the last screen copy readied */
    long *stack;

    struct mutex lock;          /* dat_fd */
    int dat_fd, idx_fd;
    int records;                /* thumbnails in thumbs.dat */

    /* Asked by the interface */
    volatile int lo, hi, vis_lo, vis_hi;
    volatile unsigned focus;    /* moves with every change of the above */
    volatile int want_screen;

    /* Answered by the worker */
    volatile int screen_file;   /* the file in screen[], or -1 */
    volatile bool screen_ok;
    volatile unsigned generation;

    /* Worker startup and lifetime */
    unsigned thread;
    struct event_queue queue;
    bool running, stopped;
    volatile bool ready, closing;
    bool (*load)(void *);
    void *load_data;
    size_t cache_scratch;
    unsigned seen_focus;
    int forward, backward, sweep;
    enum job job;
    int job_file;
    unsigned job_focus;
    bool aborted;
    char path[MAX_PATH];
} pt;

/* ---- the cache on disk --------------------------------------------- */

static uint32_t hash_path(const char *path, uint32_t h, uint32_t prime)
{
    for (; *path; path++)
        h = (h ^ (unsigned char)*path) * prime;
    return h;
}

static void path_key(const char *path, uint32_t *hash, uint32_t *check)
{
    *hash = hash_path(path, 2166136261u, 16777619u);
    *check = hash_path(path, 5381u, 33u);
}

static void discard_cache(void)
{
    remove(THUMBS_INDEX);
    remove(THUMBS_DATA);
}

static bool create_cache(void)
{
    struct index_head head = { THUMBS_MAGIC, THUMBS_VERSION, PHOTO_THUMB_EDGE,
                               {0, 0} };
    discard_cache();
    pt.idx_fd = open(THUMBS_INDEX, O_RDWR | O_CREAT | O_TRUNC, 0666);
    pt.dat_fd = open(THUMBS_DATA, O_RDWR | O_CREAT | O_TRUNC, 0666);
    pt.records = 0;
    if (pt.idx_fd >= 0 &&
        write(pt.idx_fd, &head, sizeof(head)) == (ssize_t)sizeof(head) &&
        pt.dat_fd >= 0)
        return true;
    if (pt.idx_fd >= 0)
        close(pt.idx_fd);
    if (pt.dat_fd >= 0)
        close(pt.dat_fd);
    pt.idx_fd = pt.dat_fd = -1;
    return false;
}

static int compare_entry(const void *left, const void *right)
{
    const struct index_entry *a = left, *b = right;
    if (a->hash != b->hash)
        return a->hash < b->hash ? -1 : 1;
    if (a->check != b->check)
        return a->check < b->check ? -1 : 1;
    /* the later of two records for one path is the one that counts */
    return (a->pad > b->pad) - (a->pad < b->pad);
}

/* The newest record for a key, or NULL */
static const struct index_entry *find_entry(const struct index_entry *e,
                                            int count, uint32_t hash,
                                            uint32_t check)
{
    int low = 0, high = count;
    while (low < high)
    {
        int mid = low + (high - low) / 2;
        if (e[mid].hash < hash ||
            (e[mid].hash == hash && e[mid].check <= check))
            low = mid + 1;
        else
            high = mid;
    }
    if (low && e[low - 1].hash == hash && e[low - 1].check == check)
        return &e[low - 1];
    return NULL;
}

/* Reads the index into the scratch at temp and gives every file the record
 * that matches it. The scratch is free again afterwards. */
static bool open_cache(void *temp, size_t temp_size)
{
    struct index_head head;
    mkdir(PHOTOS_DIR);
    mkdir(SCREEN_DIR);
    pt.idx_fd = open(THUMBS_INDEX, O_RDWR);
    pt.dat_fd = open(THUMBS_DATA, O_RDWR);
    if (pt.idx_fd < 0 || pt.dat_fd < 0 ||
        read(pt.idx_fd, &head, sizeof(head)) != (ssize_t)sizeof(head) ||
        head.magic != THUMBS_MAGIC || head.version != THUMBS_VERSION ||
        head.edge != PHOTO_THUMB_EDGE)
    {
        if (pt.idx_fd >= 0)
            close(pt.idx_fd);
        if (pt.dat_fd >= 0)
            close(pt.dat_fd);
        return create_cache();
    }

    off_t bytes = ffilesize(pt.idx_fd) - sizeof(head);
    int count = bytes / sizeof(struct index_entry);
    pt.records = ffilesize(pt.dat_fd) / THUMB_BYTES;
    struct index_entry *e = temp;
    if ((size_t)count > temp_size / sizeof(*e))
    {
        close(pt.idx_fd);
        close(pt.dat_fd);
        return create_cache();
    }
    if (read(pt.idx_fd, e, count * sizeof(*e)) != count * (ssize_t)sizeof(*e))
        count = 0;
    /* the record's place in the file orders two for one path */
    for (int i = 0; i < count; i++)
        e[i].pad = MIN(i, 0xffff);
    qsort(e, count, sizeof(*e), compare_entry);

    for (int i = 0; i < pt.pf->file_count; i++)
    {
        if ((i & 63) == 0 && !photo_thumbs_checkpoint())
            return false;
        struct photo_file *f = &pt.pf->files[i];
        uint32_t hash, check;
        if (!photo_folders_path(pt.pf, i, pt.path, sizeof(pt.path)))
            continue;
        path_key(pt.path, &hash, &check);
        const struct index_entry *r = find_entry(e, count, hash, check);
        if (!r || r->size != f->size || r->mtime != f->mtime ||
            r->slot >= pt.records)
            continue;
        f->thumb = r->slot < 0 ? THUMB_FAILED : r->slot;
        f->thumb_w = r->w;
        f->thumb_h = r->h;
    }
    lseek(pt.idx_fd, 0, SEEK_END);
    return true;
}

/* Appends a thumbnail, or the news that the file has none (tile NULL) */
static void store_thumb(int file, const fb_data *tile, int w, int h)
{
    struct photo_file *f = &pt.pf->files[file];
    struct index_entry e = {
        .size = f->size, .mtime = f->mtime, .slot = -1, .w = w, .h = h,
    };
    path_key(pt.path, &e.hash, &e.check);
    mutex_lock(&pt.lock);
    if (pt.dat_fd >= 0 && pt.idx_fd >= 0)
    {
        if (tile && lseek(pt.dat_fd, (off_t)pt.records * THUMB_BYTES,
                          SEEK_SET) >= 0 &&
            write(pt.dat_fd, tile, THUMB_BYTES) == THUMB_BYTES)
            e.slot = pt.records++;
        if (e.slot >= 0 || !tile)
            write(pt.idx_fd, &e, sizeof(e));
    }
    mutex_unlock(&pt.lock);
    f->thumb_w = w;
    f->thumb_h = h;
    f->thumb = e.slot >= 0 ? e.slot : tile ? THUMB_NONE : THUMB_FAILED;
}

static void screen_path(uint32_t hash, char *buf, size_t size)
{
    snprintf(buf, size, SCREEN_DIR "/%08lX.SCR", (unsigned long)hash);
}

static bool read_screen(const struct photo_file *f, uint32_t hash,
                        uint32_t check, fb_data *out)
{
    char path[64];
    struct screen_head head;
    screen_path(hash, path, sizeof(path));
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;
    bool ok = read(fd, &head, sizeof(head)) == (ssize_t)sizeof(head) &&
              head.magic == SCREEN_MAGIC && head.version == THUMBS_VERSION &&
              head.check == check && head.size == f->size &&
              head.mtime == f->mtime &&
              read(fd, out, SCREEN_PIXELS * sizeof(fb_data)) ==
                  SCREEN_PIXELS * (ssize_t)sizeof(fb_data);
    close(fd);
    return ok;
}

static void write_screen(const struct photo_file *f, uint32_t hash,
                         uint32_t check, const fb_data *pixels)
{
    char path[64], temp[64];
    struct screen_head head = { SCREEN_MAGIC, THUMBS_VERSION, 0, check,
                                f->size, f->mtime };
    screen_path(hash, path, sizeof(path));
    snprintf(temp, sizeof(temp), "%s.new", path);
    int fd = open(temp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return;
    bool ok = write(fd, &head, sizeof(head)) == (ssize_t)sizeof(head) &&
              write(fd, pixels, SCREEN_PIXELS * sizeof(fb_data)) ==
                  SCREEN_PIXELS * (ssize_t)sizeof(fb_data);
    if (close(fd) < 0 || !ok)
        remove(temp);
    else
    {
        remove(path);
        rename(temp, path);
    }
}

/* ---- decoding ------------------------------------------------------- */

static bool in_view(int file)
{
    return file >= pt.vis_lo && file < pt.vis_hi;
}

/* Called for every row decoded: lets the interface run, and gives up a job
 * that something more wanted has overtaken */
static bool decode_progress(int done, int rows)
{
    (void)done;
    (void)rows;
    if (!photo_thumbs_checkpoint())
        pt.aborted = true;
    else if (pt.job == JOB_SCREEN)
        pt.aborted = pt.want_screen != pt.job_file;
    else
        pt.aborted = (pt.want_screen >= 0 &&
                      pt.want_screen != pt.screen_file) ||
                     (pt.focus != pt.job_focus && !in_view(pt.job_file));
    return !pt.aborted;
}

/* Decodes pt.path to fit inside w x h, into pt.work */
static bool decode_fit(int w, int h, struct bitmap *bm)
{
    struct image_src src = { .fd = open(pt.path, O_RDONLY), .len = -1 };
    if (src.fd < 0)
        return false;
    struct custom_format format = format_native;
    format.progress = decode_progress;
    *bm = (struct bitmap){ .width = w, .height = h, .data = pt.work,
                           .format = FORMAT_NATIVE };
    int rc = image_load(&src, bm, WORK_BYTES,
                        FORMAT_NATIVE | FORMAT_RESIZE | FORMAT_KEEP_ASPECT,
                        &format);
    close(src.fd);
    return rc > 0 && !pt.aborted && bm->width > 0 && bm->height > 0 &&
           bm->width <= w && bm->height <= h;
}

/* bm centred on black in a w x h picture */
static void centre(const struct bitmap *bm, fb_data *out, int w, int h)
{
    const fb_data *src = (const fb_data *)bm->data;
    int x = (w - bm->width) / 2, y = (h - bm->height) / 2;
    memset(out, 0, (size_t)w * h * sizeof(fb_data));
    for (int row = 0; row < bm->height; row++)
        memcpy(out + (size_t)(y + row) * w + x, src + (size_t)row * bm->width,
               bm->width * sizeof(fb_data));
}

/* ---- the worker ---------------------------------------------------- */

static void ram_store(int file, const fb_data *tile);

static void make_thumb(int file)
{
    struct bitmap bm;
    pt.aborted = false;
    bool ok = decode_fit(PHOTO_THUMB_EDGE, PHOTO_THUMB_EDGE, &bm);
    if (pt.aborted)
        return;
    if (!ok)
    {
        store_thumb(file, NULL, 0, 0);
        return;
    }
    centre(&bm, pt.tile, PHOTO_THUMB_EDGE, PHOTO_THUMB_EDGE);
    store_thumb(file, pt.tile, bm.width, bm.height);
    /* kept in memory too when the disk would not take it */
    if ((file >= pt.vis_lo - NEARBY && file < pt.vis_hi + NEARBY) ||
        pt.pf->files[file].thumb == THUMB_NONE)
        ram_store(file, pt.tile);
    if (in_view(file))
        pt.generation++;
}

static void make_screen(int file)
{
    const struct photo_file *f = &pt.pf->files[file];
    uint32_t hash, check;
    struct bitmap bm;
    path_key(pt.path, &hash, &check);
    pt.aborted = false;
    long start = current_tick;
    if (read_screen(f, hash, check, (fb_data *)pt.work))
    {
        pt.screen_file = -1;
        memcpy(pt.screen, pt.work, SCREEN_PIXELS * sizeof(fb_data));
        pt.screen_ok = true;
        pt.screen_file = file;
        return;
    }
    bool ok = decode_fit(LCD_WIDTH, LCD_HEIGHT, &bm);
    if (pt.aborted)
        return;
    pt.screen_file = -1;
    if (ok)
        centre(&bm, pt.screen, LCD_WIDTH, LCD_HEIGHT);
    pt.screen_ok = ok;
    pt.screen_file = file;
    if (ok && current_tick - start > SLOW_DECODE)
        write_screen(f, hash, check, pt.screen);
}

static bool missing(int file)
{
    const struct photo_file *f = &pt.pf->files[file];
    return f->thumb == THUMB_NONE && f->ram < 0;
}

/* The next file to make a thumbnail of, or -1 */
static int next_thumb(void)
{
    int count = pt.pf->file_count;
    if (pt.seen_focus != pt.focus)
    {
        pt.seen_focus = pt.focus;
        pt.forward = pt.vis_hi;
        pt.backward = pt.vis_lo - 1;
    }
    for (int i = pt.vis_lo; i < pt.vis_hi; i++)
        if (missing(i))
            return i;
    for (int d = 0; d < NEARBY; d++)
    {
        if (pt.vis_hi + d < pt.hi && missing(pt.vis_hi + d))
            return pt.vis_hi + d;
        if (pt.vis_lo - 1 - d >= pt.lo && missing(pt.vis_lo - 1 - d))
            return pt.vis_lo - 1 - d;
    }
    for (; pt.forward < pt.hi; pt.forward++)
        if (missing(pt.forward))
            return pt.forward;
    for (; pt.backward >= pt.lo; pt.backward--)
        if (missing(pt.backward))
            return pt.backward;
    for (; pt.sweep < count; pt.sweep++)
        if (missing(pt.sweep))
            return pt.sweep;
    return -1;
}

static enum job next_job(int *file)
{
    if (!pt.ready || pt.stopped || pt.closing || !pt.pf->file_count)
        return JOB_NONE;
    if (pt.want_screen >= 0 && pt.want_screen != pt.screen_file)
    {
        *file = pt.want_screen;
        return JOB_SCREEN;
    }
    *file = next_thumb();
    return *file >= 0 ? JOB_THUMB : JOB_NONE;
}

static void close_files(void)
{
    mutex_lock(&pt.lock);
    if (pt.dat_fd >= 0)
        close(pt.dat_fd);
    if (pt.idx_fd >= 0)
        close(pt.idx_fd);
    pt.dat_fd = pt.idx_fd = -1;
    mutex_unlock(&pt.lock);
}

static void worker(void)
{
    struct queue_event ev;
    if (pt.load(pt.load_data) && photo_thumbs_checkpoint())
    {
        /* The index borrows the space that will hold the thumbnails. */
        if (pt.pf->file_count)
            open_cache(pt.ram_pixels, pt.cache_scratch);
        if (photo_thumbs_checkpoint())
        {
            membarrier();
            pt.ready = true;
        }
    }
    for (;;)
    {
        int file;
        enum job job = next_job(&file);
        queue_wait_w_tmo(&pt.queue, &ev, job == JOB_NONE ? TIMEOUT_BLOCK : 0);
        if (ev.id == EV_EXIT)
            return;
        if (ev.id == SYS_USB_CONNECTED)
        {
            /* The disk goes to the host, and Photos closes */
            close_files();
            pt.stopped = true;
            usb_acknowledge(SYS_USB_CONNECTED_ACK, ev.data);
            continue;
        }
        if (ev.id != SYS_TIMEOUT)
            continue;
        job = next_job(&file);
        if (job == JOB_NONE ||
            !photo_folders_path(pt.pf, file, pt.path, sizeof(pt.path)))
        {
            if (job == JOB_THUMB)
                pt.pf->files[file].thumb = THUMB_FAILED;
            continue;
        }
        pt.job = job;
        pt.job_file = file;
        pt.job_focus = pt.focus;
        cpu_boost(true);
        if (job == JOB_SCREEN)
            make_screen(file);
        else
            make_thumb(file);
        cpu_boost(false);
        pt.job = JOB_NONE;
        yield();
    }
}

/* ---- the interface's side ------------------------------------------ */

static void wake(void)
{
    if (pt.running && queue_empty(&pt.queue))
        queue_post(&pt.queue, EV_WAKE, 0);
}

size_t photo_thumbs_bytes(void)
{
    return RAM_SLOTS * THUMB_BYTES + THUMB_BYTES + WORK_BYTES +
           SCREEN_PIXELS * sizeof(fb_data) + STACK_BYTES + 64;
}

bool photo_thumbs_open(struct photo_folders *pf, void *buf, size_t size,
                       bool (*load)(void *), void *data)
{
    memset(&pt, 0, sizeof(pt));
    pt.pf = pf;
    pt.load = load;
    pt.load_data = data;
    pt.dat_fd = pt.idx_fd = -1;
    pt.want_screen = pt.screen_file = -1;
    pt.backward = -1;
    for (int i = 0; i < RAM_SLOTS; i++)
        pt.ram[i].file = -1;
    if (size < photo_thumbs_bytes())
        return false;
    unsigned char *p = (unsigned char *)ALIGN_UP((uintptr_t)buf, 8);
    pt.stack = (long *)p;
    p += STACK_BYTES;
    pt.ram_pixels = (fb_data *)p;
    p += RAM_SLOTS * THUMB_BYTES;
    pt.tile = (fb_data *)p;
    p += THUMB_BYTES;
    pt.screen = (fb_data *)p;
    p += SCREEN_PIXELS * sizeof(fb_data);
    pt.work = p;
    mutex_init(&pt.lock);

    pt.cache_scratch = (unsigned char *)buf + size -
                       (unsigned char *)pt.ram_pixels;

    queue_init(&pt.queue, true);
    pt.thread = create_thread(worker, pt.stack, STACK_BYTES, 0, "photo thumbs"
                              IF_PRIO(, PRIORITY_BACKGROUND) IF_COP(, CPU));
    if (!pt.thread)
    {
        queue_delete(&pt.queue);
        return false;
    }
    pt.running = true;
    return true;
}

bool photo_thumbs_ready(void)
{
    bool ready = pt.ready;
    membarrier();
    return ready;
}

bool photo_thumbs_checkpoint(void)
{
    yield();
    return !pt.closing && queue_empty(&pt.queue);
}

void photo_thumbs_close(void)
{
    if (pt.running)
    {
        pt.closing = true;
        queue_post(&pt.queue, EV_EXIT, 0);
        thread_wait(pt.thread);
        queue_delete(&pt.queue);
        pt.running = false;
    }
    if (pt.dat_fd >= 0 || pt.idx_fd >= 0)
        close_files();
    for (int i = 0; i < RAM_SLOTS; i++)
        if (pt.ram[i].file >= 0)
            pt.pf->files[pt.ram[i].file].ram = -1;
}

void photo_thumbs_focus(int lo, int hi, int vis_lo, int vis_hi)
{
    if (lo == pt.lo && hi == pt.hi && vis_lo == pt.vis_lo &&
        vis_hi == pt.vis_hi)
        return;
    pt.lo = lo;
    pt.hi = hi;
    pt.vis_lo = vis_lo;
    pt.vis_hi = vis_hi;
    pt.focus++;
    wake();
}

/* The least recently used slot, emptied */
static int ram_slot(void)
{
    int best = 0;
    for (int i = 1; i < RAM_SLOTS; i++)
        if (pt.ram[i].stamp < pt.ram[best].stamp)
            best = i;
    if (pt.ram[best].file >= 0)
        pt.pf->files[pt.ram[best].file].ram = -1;
    pt.ram[best].file = -1;
    return best;
}

static void ram_store(int file, const fb_data *tile)
{
    int slot = ram_slot();
    memcpy(pt.ram_pixels + slot * THUMB_PIXELS, tile, THUMB_BYTES);
    pt.ram[slot].file = file;
    pt.ram[slot].stamp = ++pt.stamp;
    pt.pf->files[file].ram = slot;
}

enum photo_thumb_state photo_thumbs_tile(int file, const fb_data **tile)
{
    struct photo_file *f = &pt.pf->files[file];
    if (f->ram < 0 && f->thumb >= 0)
    {
        int slot = ram_slot();
        fb_data *pixels = pt.ram_pixels + slot * THUMB_PIXELS;
        bool ok = false;
        mutex_lock(&pt.lock);
        if (pt.dat_fd >= 0 &&
            lseek(pt.dat_fd, (off_t)f->thumb * THUMB_BYTES, SEEK_SET) >= 0)
            ok = read(pt.dat_fd, pixels, THUMB_BYTES) == THUMB_BYTES;
        mutex_unlock(&pt.lock);
        if (!ok)
        {
            f->thumb = THUMB_FAILED;
            return PHOTO_THUMB_BROKEN;
        }
        pt.ram[slot].file = file;
        f->ram = slot;
    }
    if (f->ram >= 0)
    {
        pt.ram[f->ram].stamp = ++pt.stamp;
        *tile = pt.ram_pixels + f->ram * THUMB_PIXELS;
        return PHOTO_THUMB_READY;
    }
    return f->thumb == THUMB_FAILED ? PHOTO_THUMB_BROKEN : PHOTO_THUMB_PENDING;
}

unsigned photo_thumbs_generation(void)
{
    return pt.generation;
}

bool photo_thumbs_pending(void)
{
    if (!pt.running || pt.stopped)
        return false;
    for (int i = pt.vis_lo; i < pt.vis_hi; i++)
        if (missing(i))
            return true;
    return false;
}

enum photo_thumb_state photo_thumbs_screen_take(int file, fb_data *pixels)
{
    if (pt.screen_file != file)
        return pt.running && !pt.stopped ? PHOTO_THUMB_PENDING
                                         : PHOTO_THUMB_BROKEN;
    if (!pt.screen_ok)
        return PHOTO_THUMB_BROKEN;
    memcpy(pixels, pt.screen, SCREEN_PIXELS * sizeof(fb_data));
    return PHOTO_THUMB_READY;
}

void photo_thumbs_prefetch(int file)
{
    if (!pt.running || pt.want_screen == file)
        return;
    pt.want_screen = file;
    wake();
}

enum photo_thumb_state photo_thumbs_screen(int file, fb_data *pixels,
                                           long wait)
{
    photo_thumbs_prefetch(file);
    long end = current_tick + wait;
    enum photo_thumb_state state;
    while ((state = photo_thumbs_screen_take(file, pixels)) ==
               PHOTO_THUMB_PENDING && TIME_BEFORE(current_tick, end))
        sleep(1);
    return state;
}

/* The tile's picture, enlarged to fit the screen and centred on black */
bool photo_thumbs_preview(int file, fb_data *pixels)
{
    const fb_data *tile;
    const struct photo_file *f = &pt.pf->files[file];
    int w = f->thumb_w, h = f->thumb_h;
    if (photo_thumbs_tile(file, &tile) != PHOTO_THUMB_READY || !w || !h)
        return false;
    int x0 = (PHOTO_THUMB_EDGE - w) / 2, y0 = (PHOTO_THUMB_EDGE - h) / 2;
    int dw = LCD_WIDTH, dh = h * LCD_WIDTH / w;
    if (dh > LCD_HEIGHT)
    {
        dh = LCD_HEIGHT;
        dw = w * LCD_HEIGHT / h;
    }
    int dx = (LCD_WIDTH - dw) / 2, dy = (LCD_HEIGHT - dh) / 2;
    memset(pixels, 0, SCREEN_PIXELS * sizeof(fb_data));
    for (int y = 0; y < dh; y++)
    {
        /* 16.16 source position of the row's centre */
        int sy = ((2 * y + 1) * h << 15) / dh - (1 << 15);
        sy = MAX(0, sy);
        int y1 = sy >> 16, fy = (sy >> 8) & 0xff;
        int y2 = MIN(h - 1, y1 + 1);
        const fb_data *r1 = tile + (y0 + y1) * PHOTO_THUMB_EDGE + x0;
        const fb_data *r2 = tile + (y0 + y2) * PHOTO_THUMB_EDGE + x0;
        fb_data *out = pixels + (dy + y) * LCD_WIDTH + dx;
        for (int x = 0; x < dw; x++)
        {
            int sx = ((2 * x + 1) * w << 15) / dw - (1 << 15);
            sx = MAX(0, sx);
            int x1 = sx >> 16, fx = (sx >> 8) & 0xff;
            int x2 = MIN(w - 1, x1 + 1);
            unsigned c[4] = { r1[x1], r1[x2], r2[x1], r2[x2] };
            unsigned w4[4] = { (256 - fx) * (256 - fy), fx * (256 - fy),
                               (256 - fx) * fy, fx * fy };
            unsigned r = 0, g = 0, b = 0;
            for (int k = 0; k < 4; k++)
            {
                r += RGB_UNPACK_RED(c[k]) * w4[k];
                g += RGB_UNPACK_GREEN(c[k]) * w4[k];
                b += RGB_UNPACK_BLUE(c[k]) * w4[k];
            }
            out[x] = FB_RGBPACK(r >> 16, g >> 16, b >> 16);
        }
        if ((y & 31) == 0)
            yield();
    }
    return true;
}
