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
#include "sim/shim-cider.h"
#include "system/bg_task.h"

#define TRACKS 40
#define ART_ALBUMS 24
#define SIM_ART_EVENT 0x1187
static const char art_key[] = "1234567812345678123456781234567812345678123456781234567812345678";
static long reply_at;
static bool requested;
static struct cider_store store;
static uint8_t record[CS_REC_MAX];
static struct {
    long warmed;
    long due[2];
    bool stored[2], resident[2];
    unsigned last_visible;
} art[ART_ALBUMS];
static unsigned art_revision = 1;
static bool scanned;
static struct bg_task art_task;
static long art_delay;

static void __attribute__((constructor)) init_art_fixture(void)
{
    const char *ms = getenv("PODBOX_SIM_CIDER_ART_MS");
    if (ms)
        art_delay = MAX(1, atol(ms) * HZ / 1000);
}

static void fixture_art_key(int index, char key[65])
{
    snprintf(key, 65, "%08x%s", index + 1, art_key + 8);
}

static int art_index(const char *key)
{
    if (strlen(key) != 64)
        return -1;
    /* The suffix is kept at full width even on a host with a 64-bit long. */
    char prefix[9];
    memcpy(prefix, key, 8);
    prefix[8] = 0;
    unsigned n = strtoul(prefix, NULL, 16);
    return n > 0 && n <= ART_ALBUMS ? (int)n - 1 : -1;
}

uint32_t podlink_crc32(const unsigned char *data, unsigned size)
{
    return ~crc_32r(data, size, UINT32_MAX);
}

bool cider_available(void) { return getenv("PODBOX_SIM_CIDER") != NULL; }
uint32_t cider_panel_revision(void) { return 1; }
int cider_page_total(void) { return cider_available() ? (art_delay ? ART_ALBUMS : 1) : 0; }
bool cider_page_stale(void) { return false; }
bool cider_page_pending(void) { return false; }
void cider_page_open(const char *route) { (void)route; }

bool cider_page_peek(int index, struct cider_row *row)
{
    if (index < 0 || index >= cider_page_total())
        return false;
    *row = (struct cider_row){
        .id = "sim-album", .title = "Remote album",
        .subtitle = "Remote artist", .route = "album/sim?format=panel",
        .kind = CIDER_ROW_ALBUM, .count = TRACKS,
    };
    if (art_delay)
    {
        fixture_art_key(index, row->art);
        snprintf(row->id, sizeof row->id, "sim-album-%d", index);
        snprintf(row->title, sizeof row->title, "Remote album %02d", index);
        snprintf(row->route, sizeof row->route, "album/sim/%d?format=panel", index);
    }
    else
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

static int art_tier(const char *tier) { return !strcmp(tier, "XS"); }
static unsigned pack_tier(int tier) { return tier ? CS_XS : CS_M; }
static unsigned cover_pixel(int index) { return 0x6c24 + 32 * index; }

static void service_art(const struct queue_event *event)
{
    (void)event;
    if (!store_ready())
        return;
    for (int i = 0; i < ART_ALBUMS; i++)
    {
        char key[65];
        uint8_t bytes[32];
        fixture_art_key(i, key);
        cs_key_from_hex(key, bytes);
        for (int t = 0; t < 2; t++)
        {
            unsigned tier = pack_tier(t);
            if (!scanned)
                art[i].stored[t] = cs_maybe(&store, bytes, tier);
            if (!art[i].due[t] || TIME_BEFORE(current_tick, art[i].due[t]))
                continue;
            bool disk = art[i].stored[t];
            unsigned length = cs_payload_size(tier);
            const uint8_t *payload;
            if (disk)
                art[i].resident[t] = cs_get(&store, bytes, tier, record, sizeof record,
                                            &payload, &length) == CS_OK;
            else
            {
                uint16_t *out = (uint16_t *)(record + CS_REC_HEAD);
                unsigned dim = t ? 32 : 128;
                out[0] = 0x5441;
                out[1] = 8;
                out[2] = out[3] = dim;
                out[4] = !t;
                out[5] = 0;
                for (unsigned j = 0; j < dim * dim; j++)
                    out[6 + j] = cover_pixel(i);
                art[i].stored[t] = cs_put(&store, bytes, tier, out, length,
                                         record, sizeof record, NULL) >= 0;
                art[i].resident[t] = true;
            }
            art[i].due[t] = 0;
            if (art[i].resident[t])
            {
                art_revision++;
                DEBUGF("sim Cider art %d %s %s\n", i, t ? "XS" : "M",
                       disk ? "disk" : "host");
            }
        }
    }
    scanned = true;
}
static struct bg_task art_task = {.name = "sim remote art", .handle_event = service_art};

bool cider_art_local(const char *key, const char *tier)
{
    int i = art_index(key), t = art_tier(tier);
    return art_delay ? i >= 0 && art[i].stored[t] :
                       !strcmp(key, art_key) && !strcmp(tier, "M");
}

bool cider_has_art(const char *key, const char *tier)
{
    int i = art_index(key);
    return art_delay ? i >= 0 && art[i].resident[art_tier(tier)] :
                       cider_art_local(key, tier);
}

bool cider_copy_art(const char *key, const char *tier, void *pixels, unsigned size)
{
    if (!cider_has_art(key, tier))
        return false;
    uint16_t *out = pixels;
    unsigned color = art_delay ? cover_pixel(art_index(key)) : 0x6c6c;
    for (unsigned i = 0; i < size / sizeof(*out); i++)
        out[i] = color;
    return true;
}

unsigned cider_art_revision(void)
{
    if (art_delay && !bg_task_queued(&art_task, SIM_ART_EVENT))
        bg_task_post(&art_task, SIM_ART_EVENT);
    return art_revision;
}

unsigned cider_read_ahead_count(unsigned speed, unsigned visible)
{ (void)speed; return visible < 15 ? 15 - visible : 0; }

static void schedule_art(int i, int tier)
{
    if (i < 0 || art[i].resident[tier] || art[i].due[tier])
        return;
    long ready = art[i].warmed ? art[i].warmed + art_delay : current_tick + art_delay;
    art[i].due[tier] = art[i].stored[tier] ? current_tick :
                       MAX(ready, current_tick + HZ / 20);
}

bool cider_prefetch_art(const char *key, const char *tier)
{
    if (!art_delay || !scanned)
        return false;
    int t = art_tier(tier);
    for (int i = 0; i < ART_ALBUMS; i++)
        if (art[i].due[t] && !art[i].stored[t])
            return false;
    schedule_art(art_index(key), t);
    return true;
}

void cider_read_ahead(const char *const *keys, unsigned count, const char *tier)
{
    if (!art_delay)
        return;
    for (unsigned n = 0; n < count; n++)
        if (cider_art_local(keys[n], tier))
            schedule_art(art_index(keys[n]), art_tier(tier));
}

void cider_warm_art(const char *const *keys, unsigned count, const char *tier)
{
    (void)tier;
    if (!art_delay)
        return;
    for (unsigned n = 0; n < count; n++)
    {
        int i = art_index(keys[n]);
        if (i >= 0 && !art[i].warmed)
            art[i].warmed = current_tick;
    }
}

void cider_retain_art(const char *const *keys, unsigned count, const char *tier)
{ (void)keys; (void)count; (void)tier; }

void cider_request_xs(const char *const *keys, unsigned count)
{
    if (!art_delay || !scanned)
        return;
    for (int i = 0; i < ART_ALBUMS; i++)
        if (art[i].due[1] && !art[i].stored[1])
            return;
    for (unsigned n = 0; n < count; n++)
        schedule_art(art_index(keys[n]), 1);
}

void sim_cider_visible(int index, unsigned quality, unsigned pixel)
{
    if (!art_delay || (unsigned)index >= ART_ALBUMS || art[index].last_visible == quality + 1)
        return;
    art[index].last_visible = quality + 1;
    DEBUGF("sim Cider visible %d %u %u\n", index, quality, pixel);
}

void cider_cancel_art(void) {}
