/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Cover thumbnails for the video files of the video index, so the Videos
 * lists draw a cover from a small file instead of decoding a JPEG per row.
 *
 * Each video with a 'covr' image gets two thumbnails, fitted inside a
 * VIDEO_ART_DIM square for its row and a VIDEO_ART_PANE square for the
 * menu pane's slideshow, at its own shape, in the art cache's file format
 * under library/cache/art/video/, named by the index's key for it. A video
 * with no cover has none, and its rows show the stock placeholder: a frame
 * of the film would need the H.264 decoder, which only the video player
 * plugin holds.
 *
 * The documents walk rewrites the index and sends this an update; the pass
 * makes what is missing and, having seen every video, removes the
 * thumbnails no video names. Like the walk it takes its turn on the shared
 * thread through a tick of its own.
 ****************************************************************************/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "file.h"
#include "dir.h"
#include "usb.h"
#include "core_alloc.h"
#include "string-extra.h"
#include "draw/bmp.h"
#include "draw/image_load.h"
#include "database/tagcache.h"
#include "system/library_files.h"
#include "system/debug_log.h"
#include "ui/motion_schedule.h"
#include "files/file_index.h"
#include "files/video_index.h"
#include "metadata/art_cache.h"         /* struct art_cache_header */
#include "video_art.h"

#define VIDEO_ART_VERSION 1
/* Videos whose thumbnails a pass keeps. Past it, the sweep is skipped. */
#define VIDEO_ART_MAX 1024

#define WORK_BYTES (BM_SCALED_SIZE(VIDEO_ART_PANE, VIDEO_ART_PANE, \
                                   FORMAT_NATIVE, 0) + IMAGE_DECODE_OVERHEAD)

/* A thumbnail's file name ends in its size's extension, or while it is
 * being written in its partial one */
static const char *const extensions[2][2] = {
    { "aat", "new" },       /* a row's */
    { "aap", "pnw" },       /* the pane's */
};

static volatile bool wants_pass;
static volatile unsigned generation;

unsigned video_art_generation(void)
{
    return generation;
}

static void thumb_path(uint64_t key, bool pane, char *out, size_t size,
                       bool partial)
{
    snprintf(out, size, LIB_VIDEO_ART_DIR "/%08lx%08lx.%s",
             (unsigned long)(uint32_t)(key >> 32), (unsigned long)(uint32_t)key,
             extensions[pane][partial]);
}

static bool load(uint64_t art_key, bool pane, int dim, fb_data *pixels,
                 fb_data background)
{
    char path[MAX_PATH];
    struct art_cache_header h;
    fb_data row[VIDEO_ART_PANE];
    int edge = pane ? VIDEO_ART_PANE : VIDEO_ART_DIM;
    if (!art_key)
        return false;
    thumb_path(art_key, pane, path, sizeof(path), false);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;
    bool ok = read(fd, &h, sizeof(h)) == (ssize_t)sizeof(h) &&
              h.magic == ART_CACHE_MAGIC && h.version == VIDEO_ART_VERSION &&
              h.width && h.height && h.width <= edge && h.height <= edge;
    if (ok)
    {
        for (int i = 0; i < dim * dim; i++)
            pixels[i] = background;
        int x0 = (dim - h.width) / 2, y0 = (dim - h.height) / 2;
        for (int y = 0; y < h.height && ok; y++)
        {
            ok = read(fd, row, h.width * sizeof(fb_data)) ==
                 (ssize_t)(h.width * sizeof(fb_data));
            if (y + y0 < 0 || y + y0 >= dim)
                continue;
            for (int x = 0; x < h.width; x++)
                if (x + x0 >= 0 && x + x0 < dim)
                    pixels[(y + y0) * dim + x + x0] = row[x];
        }
    }
    close(fd);
    return ok;
}

bool video_art_load(uint64_t art_key, int dim, fb_data *pixels,
                    fb_data background)
{
    return load(art_key, false, dim, pixels, background);
}

struct pick
{
    unsigned kinds;
    int previous;
    int seen, eligible, chosen;
    uint64_t key;
};

/* Reservoir sampling: each eligible video is kept with an equal chance.
 * The last cover shown is eligible only when no other is. */
static bool pick_one(const struct video_entry *e, void *data)
{
    struct pick *p = data;
    if (!e->art_key || !(e->kind & p->kinds))
        return true;
    int n = p->seen++;
    if (n == p->previous && p->eligible)
        return true;
    if (n == p->previous || rand() % ++p->eligible == 0)
    {
        p->chosen = n;
        p->key = e->art_key;
    }
    return true;
}

int video_art_pane_load(unsigned kinds, int previous, fb_data *pixels)
{
    static struct video_index_text text;
    struct pick p = { .kinds = kinds, .previous = previous, .chosen = -1 };
    video_index_each(pick_one, &p, &text);
    if (p.chosen < 0 ||
        !load(p.key, true, VIDEO_ART_PANE, pixels, LCD_BLACK))
        return -1;
    return p.chosen;
}

/* ---- the pass ---------------------------------------------------------- */

struct pass
{
    void *work;
    uint64_t *keys;
    int count;
    bool stopped;
};

static bool render(const struct video_entry *e, bool pane, const char *out,
                   struct pass *p)
{
    int edge = pane ? VIDEO_ART_PANE : VIDEO_ART_DIM;
    struct image_src src = {
        .fd = open(e->path, O_RDONLY), .pos = e->cover_at,
        .len = e->cover_size,
    };
    struct bitmap bm = {
        .width = edge, .height = edge, .format = FORMAT_NATIVE,
        .data = p->work,
    };
    if (src.fd < 0)
        return false;
    int rc = image_load(&src, &bm, WORK_BYTES,
                        FORMAT_NATIVE | FORMAT_RESIZE | FORMAT_KEEP_ASPECT |
                        FORMAT_DITHER, NULL);
    close(src.fd);
    if (rc <= 0 || bm.width <= 0 || bm.height <= 0 ||
        bm.width > edge || bm.height > edge)
        return false;

    char partial[MAX_PATH];
    struct art_cache_header h = {
        .magic = ART_CACHE_MAGIC, .version = VIDEO_ART_VERSION,
        .width = bm.width, .height = bm.height, .layout = AA_ROWS,
    };
    size_t bytes = (size_t)bm.width * bm.height * sizeof(fb_data);
    thumb_path(e->art_key, pane, partial, sizeof(partial), true);
    int fd = open(partial, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return false;
    bool ok = write(fd, &h, sizeof(h)) == (ssize_t)sizeof(h) &&
              write(fd, bm.data, bytes) == (ssize_t)bytes;
    ok = close(fd) == 0 && ok && rename(partial, out) == 0;
    if (!ok)
        remove(partial);
    else
        generation++;
    return ok;
}

static bool visit(const struct video_entry *e, void *data)
{
    struct pass *p = data;
    char out[MAX_PATH];
    if (ui_motion_active() || bg_task_should_stop(&video_art_task))
    {
        p->stopped = true;
        return false;
    }
    if (!e->art_key)
        return true;
    if (p->count < VIDEO_ART_MAX)
        p->keys[p->count] = e->art_key;
    p->count++;
    for (int pane = 0; pane < 2; pane++)
    {
        thumb_path(e->art_key, pane, out, sizeof(out), false);
        if (!file_exists(out) && !render(e, pane, out, p))
        {
            debug_log(DEBUG_LOG_TASKS, "%s: no cover from %s",
                      video_art_task.name, e->path);
            break;
        }
        yield();
    }
    return true;
}

static bool named(const struct pass *p, const char *name)
{
    char *end;
    if (strlen(name) != 20 || (strcasecmp(name + 16, ".aat") &&
                               strcasecmp(name + 16, ".aap")))
        return true;            /* not a thumbnail: left alone */
    char high[9];
    strlcpy(high, name, sizeof(high));
    uint64_t key = (uint64_t)strtoul(high, &end, 16) << 32;
    key |= strtoul(name + 8, &end, 16);
    for (int i = 0; i < p->count; i++)
        if (p->keys[i] == key)
            return true;
    return false;
}

static void sweep(const struct pass *p)
{
    char path[MAX_PATH];
    DIR *dir = opendir(LIB_VIDEO_ART_DIR);
    struct dirent *entry;
    if (!dir)
        return;
    while ((entry = readdir(dir)))
    {
        if (named(p, (char *)entry->d_name))
            continue;
        snprintf(path, sizeof(path), LIB_VIDEO_ART_DIR "/%s",
                 (char *)entry->d_name);
        remove(path);
        generation++;
    }
    closedir(dir);
}

static void run_pass(void)
{
    static struct video_index_text text;
    struct pass p = {0};
    int handle = core_alloc(WORK_BYTES + VIDEO_ART_MAX * sizeof(uint64_t));
    if (handle <= 0)
    {
        wants_pass = true;
        return;
    }
    p.keys = core_get_data_pinned(handle);
    p.work = p.keys + VIDEO_ART_MAX;
    mkdir(LIB_ART_DIR);
    mkdir(LIB_VIDEO_ART_DIR);
    video_index_each(visit, &p, &text);
    if (p.stopped)
        wants_pass = true;
    else if (p.count <= VIDEO_ART_MAX)
        sweep(&p);
    core_unpin(handle);
    core_free(handle);
}

static void video_art_tick(void)
{
    if (wants_pass && !file_index_is_busy() && !tagcache_is_busy() &&
        !usb_host_wants_storage() && !ui_motion_active() &&
        !bg_task_preempted(&video_art_task))
    {
        wants_pass = false;
        run_pass();
    }
}

static void video_art_request(bool rebuild)
{
    (void)rebuild;
    wants_pass = true;
}

struct bg_task video_art_task =
{
    .name    = "video covers",
    .rank    = BG_RANK_FILES,
    .request = video_art_request,
    .tick    = video_art_tick,
};

void video_art_init(void)
{
    video_art_task.work_bytes = WORK_BYTES + VIDEO_ART_MAX * sizeof(uint64_t);
    bg_task_init(&video_art_task);
    wants_pass = !video_index_stale();
}
