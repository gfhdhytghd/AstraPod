/* SPDX-License-Identifier: GPL-2.0-or-later
 * One remote Cover Flow fixture. The real panel and cover store run with
 * synthetic host replies when PODBOX_SIM_CIDER is set. */
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dir.h"
#include "debug.h"
#include "crc32.h"
#include "podlink.h"
#include "file.h"
#include "kernel.h"
#include "rbpaths.h"
#include "screens/playback/cider.h"
#include "screens/playback/cider_library.h"

#define TRACKS 40
static const char art_key[] = "1234567812345678123456781234567812345678123456781234567812345678";
static long reply_at;
static bool requested;
static struct cider_store store;
static uint8_t record[CS_REC_MAX];

uint32_t podlink_crc32(const unsigned char *data, unsigned size)
{
    return ~crc_32r(data, size, UINT32_MAX);
}

bool cider_available(void) { return getenv("PODBOX_SIM_CIDER") != NULL; }
uint32_t cider_panel_revision(void) { return 1; }
int cider_page_total(void) { return cider_available() ? 1 : 0; }
bool cider_page_stale(void) { return false; }
bool cider_page_pending(void) { return false; }
void cider_page_open(const char *route) { (void)route; }

bool cider_page_peek(int index, struct cider_row *row)
{
    if (index || !cider_available())
        return false;
    *row = (struct cider_row){
        .id = "sim-album", .title = "Remote album",
        .subtitle = "Remote artist", .route = "album/sim?format=panel",
        .kind = CIDER_ROW_ALBUM, .count = TRACKS,
    };
    memcpy(row->art, art_key, sizeof art_key);
    return true;
}

bool cider_page_row(int index, struct cider_row *row)
{
    return cider_page_peek(index, row);
}

bool cider_panel_album(int index, struct cider_row *row, uint32_t *revision)
{
    *revision = cider_panel_revision();
    return cider_page_peek(index, row);
}

void cider_track_page_open(const char *route)
{
    if (strcmp(route, "album/sim?format=panel"))
        requested = false;
    else if (!requested)
    {
        const char *ms = getenv("PODBOX_SIM_CIDER");
        reply_at = current_tick + atol(ms) * HZ / 1000;
        requested = true;
        DEBUGF("sim Cider: track list requested\n");
    }
}

int cider_track_page_total(bool *stale)
{
    if (stale)
        *stale = false;
    return requested && !TIME_BEFORE(current_tick, reply_at) ? TRACKS : -1;
}

bool cider_track_page_row(int index, struct cider_row *row)
{
    if (index < 0 || index >= cider_track_page_total(NULL))
        return false;
    *row = (struct cider_row){.kind = CIDER_ROW_SONG, .duration = 123000};
    snprintf(row->title, sizeof row->title, "Remote song %02d", index + 1);
    snprintf(row->route, sizeof row->route, "play/sim/%d", index);
    snprintf(row->id, sizeof row->id, "sim-%d", index);
    return true;
}

bool cider_play_in(const char *route, const char *context, int index)
{
    DEBUGF("sim Cider: play %s %s %d\n", route, context, index);
    return false; /* No streaming service in a simulator. */
}

int cider_play_status(void) { return -1; }

static int store_open(const char *path, int mode)
{
    return open(path, mode == CS_O_READ ? O_RDONLY : O_RDWR | O_CREAT, 0666);
}

static int store_read(int fd, void *buf, unsigned size) { return read(fd, buf, size); }
static int store_write(int fd, const void *buf, unsigned size) { return write(fd, buf, size); }
static int store_seek(int fd, uint32_t offset) { return lseek(fd, offset, SEEK_SET); }
static int store_size(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    int size = ffilesize(fd);
    close(fd);
    return size;
}
static void store_mkdir(const char *path) { mkdir(path); }

static bool store_ready(void)
{
    static const struct cs_io io = {store_open, store_read, store_write,
        store_seek, fsync, close, remove, store_size, store_mkdir, yield};
    if (!store.io)
        cs_init(&store, &io, ROCKBOX_DIR "/cider-art", CS_DEFAULT_CAP);
    return store.loaded || cs_load(&store, record, sizeof record, 1) == CS_OK;
}

int cider_track_cache_read(const uint8_t key[32], void *page, size_t bytes)
{
    const uint8_t *payload;
    unsigned length;
    if (!store_ready() || cs_get(&store, key, CS_TRACKS, record, sizeof record,
                                 &payload, &length) != CS_OK || bytes > length)
        return -1;
    memcpy(page, payload, bytes);
    return 1;
}

bool cider_track_cache_write(const uint8_t key[32], const void *page, size_t bytes)
{
    if (store_ready() && bytes <= CS_TRACK_BYTES)
    {
        memcpy(record + CS_REC_HEAD, page, bytes);
        memset(record + CS_REC_HEAD + bytes, 0, CS_TRACK_BYTES - bytes);
        cs_put(&store, key, CS_TRACKS, record + CS_REC_HEAD, CS_TRACK_BYTES,
               record, sizeof record, NULL);
    }
    return true;
}

bool cider_art_local(const char *key, const char *tier)
{ return !strcmp(key, art_key) && !strcmp(tier, "M"); }
bool cider_has_art(const char *key, const char *tier)
{ return cider_art_local(key, tier); }
bool cider_copy_art(const char *key, const char *tier, void *pixels, unsigned size)
{
    if (!cider_has_art(key, tier) || size < CIDER_COVER_SCRATCH_BYTES)
        return false;
    memset(pixels, 0x6c, CIDER_COVER_SCRATCH_BYTES);
    return true;
}
unsigned cider_art_revision(void) { return 1; }
unsigned cider_read_ahead_count(unsigned speed, unsigned visible)
{ (void)speed; (void)visible; return 0; }
bool cider_prefetch_art(const char *key, const char *tier)
{ (void)key; (void)tier; return false; }
void cider_read_ahead(const char *const *keys, unsigned count, const char *tier)
{ (void)keys; (void)count; (void)tier; }
void cider_warm_art(const char *const *keys, unsigned count, const char *tier)
{ (void)keys; (void)count; (void)tier; }
void cider_retain_art(const char *const *keys, unsigned count, const char *tier)
{ (void)keys; (void)count; (void)tier; }
void cider_request_xs(const char *const *keys, unsigned count)
{ (void)keys; (void)count; }
void cider_cancel_art(void) {}
