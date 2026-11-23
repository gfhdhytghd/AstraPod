/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * See core_map.h.
 *
 * Parts: the snapshot, taken without yielding because buflib may compact as
 * soon as another thread runs; the one-line summary; the log writer.
 ****************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "core_alloc.h"
#include "logfile.h"
#include "rbversion.h"
#include "system/library_files.h"
#include "core_map.h"

#define MAP_ENTRIES 384
#define TOP_HOLDERS 5

enum
{
    MAP_PINNED = 1,    /* pin count above zero */
    MAP_FIXED = 2,     /* no move callback, or buflib_ops_locked */
    MAP_SHRINK = 4,    /* has a shrink callback */
};

struct map_entry
{
    unsigned int offset, bytes;   /* bytes; a free block has handle 0 */
    const void *ops;
    unsigned short handle;
    unsigned char flags;
};

static struct
{
    struct map_entry entry[MAP_ENTRIES];
    int count, dropped;
    size_t free_bytes, end_bytes, largest_free, fixed_bytes;
    int handles, fixed;
} map;

/* ---- the snapshot ------------------------------------------------------- */

static void take_snapshot(void)
{
    extern struct buflib_context core_ctx;
    struct buflib_context *ctx = &core_ctx;
    size_t run = 0;

    memset(&map, 0, sizeof(map));
    for (union buflib_data *block = ctx->buf_start; block < ctx->alloc_end;
         block += labs(block->val))
    {
        if (block->val == 0)
            break;   /* a corrupt header: stop rather than loop */
        struct map_entry e = { 0 };
        e.offset = (char *)block - (char *)ctx->buf_start;
        e.bytes = labs(block->val) * sizeof(union buflib_data);
        if (block->val < 0)
        {
            map.free_bytes += e.bytes;
            run += e.bytes;
        }
        else
        {
            struct buflib_callbacks *ops = block[BUFLIB_IDX_OPS].ops;
            e.ops = ops;
            e.handle = ctx->handle_table - block[BUFLIB_IDX_HANDLE].handle;
            if (block[BUFLIB_IDX_PIN].pincount > 0)
                e.flags |= MAP_PINNED;
            if ((ops && !ops->move_callback) || ops == &buflib_ops_locked)
                e.flags |= MAP_FIXED;
            if (ops && ops->shrink_callback)
                e.flags |= MAP_SHRINK;
            map.handles++;
            /* Free space either side of a block that cannot move is two
             * holes, not one. */
            if (e.flags & (MAP_PINNED | MAP_FIXED))
            {
                map.fixed++;
                map.fixed_bytes += e.bytes;
                map.largest_free = MAX(map.largest_free, run);
                run = 0;
            }
        }
        if (map.count < MAP_ENTRIES)
            map.entry[map.count++] = e;
        else
            map.dropped++;
    }
    if (ctx->last_handle > ctx->alloc_end)
        map.end_bytes = (char *)ctx->last_handle - (char *)ctx->alloc_end;
    map.largest_free = MAX(map.largest_free, run + map.end_bytes);
}

/* ---- the summary -------------------------------------------------------- */

size_t core_map_summary(char *buf, size_t size)
{
    int top[TOP_HOLDERS], n = 0;
    size_t len;

    if (!size)
        return 0;
    take_snapshot();
    len = snprintf(buf, size, "free %zuK max %zuK fixed %d/%zuK of %d:",
                   (map.free_bytes + map.end_bytes) / 1024,
                   map.largest_free / 1024, map.fixed,
                   map.fixed_bytes / 1024, map.handles);
    /* The largest holders, by repeated selection: TOP_HOLDERS passes over at
     * most MAP_ENTRIES entries. */
    while (n < TOP_HOLDERS)
    {
        int best = -1;
        for (int i = 0; i < map.count; i++)
        {
            bool taken = !map.entry[i].handle;
            for (int j = 0; j < n && !taken; j++)
                taken = top[j] == i;
            if (!taken && (best < 0 ||
                           map.entry[i].bytes > map.entry[best].bytes))
                best = i;
        }
        if (best < 0)
            break;
        top[n++] = best;
    }
    for (int i = 0; i < n && len < size; i++)
    {
        const struct map_entry *e = &map.entry[top[i]];
        len += snprintf(buf + len, size - len, " %lx=%uK%s%s",
                        (unsigned long)e->ops, e->bytes / 1024,
                        e->flags & MAP_PINNED ? "p" : "",
                        e->flags & MAP_FIXED ? "f" : "");
    }
    return MIN(len, size - 1);
}

/* ---- the log ------------------------------------------------------------ */

void core_map_log(const char *reason)
{
    static char summary[200];

    core_map_summary(summary, sizeof(summary));
    /* The snapshot is copied out before the first write yields. */
    int fd = logfile_open(LIB_LOGS_DIR "/memory.log");
    if (fd < 0)
        return;
    logfile_printf(fd, "== %s, tick %ld, %s\n%s\n"
                   "offset   bytes    handle ops      flags\n",
                   reason, current_tick, RBVERSION, summary);
    for (int i = 0; i < map.count; i++)
    {
        const struct map_entry *e = &map.entry[i];
        if (!e->handle)
            logfile_printf(fd, "%08x %8u free\n", e->offset, e->bytes);
        else
            logfile_printf(fd, "%08x %8u %6u %08lx %s%s%s\n",
                           e->offset, e->bytes, e->handle,
                           (unsigned long)e->ops,
                           e->flags & MAP_PINNED ? "pinned " : "",
                           e->flags & MAP_FIXED ? "fixed " : "",
                           e->flags & MAP_SHRINK ? "shrink" : "");
    }
    if (map.end_bytes)
        logfile_printf(fd, "end      %8zu free\n", map.end_bytes);
    if (map.dropped)
        logfile_printf(fd, "(%d more blocks not listed)\n", map.dropped);
    logfile_close(fd);
}
