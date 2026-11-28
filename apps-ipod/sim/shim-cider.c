/* SPDX-License-Identifier: GPL-2.0-or-later
 * One remote Cover Flow fixture. The real panel and cover store run with
 * synthetic host replies when PODBOX_SIM_CIDER is set. */
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dir.h"
#include "debug.h"
#include "button.h"
#include "crc32.h"
#include "podlink.h"
#include "file.h"
#include "kernel.h"
#include "lcd.h"
#include "rbpaths.h"
#include "root_menu.h"
#include "screens/playback/cider.h"
#include "screens/playback/cider_controls.h"
#include "screens/playback/cider_library.h"
#include "screens/playback/cider_wps.h"
#include "screens/playback/wps.h"
#include "metadata.h"
#include "sim/shim-cider.h"
#include "screens/covers/panel_perf.h"
#include "system/hash.h"
#include "system/bg_task.h"
#include "string-extra.h"

#define TRACKS 40
#define ART_ALBUMS 160
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
    unsigned last_visible, used[2];
} art[ART_ALBUMS];
static unsigned art_revision = 1;
static bool scanned;
static struct bg_task art_task;
static long art_delay;
static bool serial_art, xs_only;
static long decode_ticks;
static int active_art = -1, active_tier;
static unsigned use_clock;
static long issued_at;
static struct cider_state playback;
static long play_at, track_at, track_delay;
static bool play_pending;

static void __attribute__((constructor)) init_art_fixture(void)
{
    const char *ms = getenv("PODBOX_SIM_CIDER_ART_MS");
    if (ms)
        art_delay = MAX(1, atol(ms) * HZ / 1000);
    serial_art = getenv("PODBOX_SIM_CIDER_SERIAL") != NULL;
    xs_only = getenv("PODBOX_SIM_CIDER_XS_ONLY") != NULL;
    decode_ticks = serial_art ? 4 : 0;
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
    const char *delay = getenv("PODBOX_SIM_CIDER_PLAY_MS");
    if (!delay)
        return false;
    unsigned album = 0;
    sscanf(context, "album/sim/%u", &album);
    struct cider_row row;
    if (!cider_page_peek(album, &row))
        return false;
    strlcpy(playback.album, row.title, sizeof(playback.album));
    strlcpy(playback.artist, row.subtitle, sizeof(playback.artist));
    strlcpy(playback.art, row.art, sizeof(playback.art));
    snprintf(playback.title, sizeof(playback.title), "Remote song %02d", index + 1);
    play_at = current_tick + atol(delay) * HZ / 1000;
    const char *track_ms = getenv("PODBOX_SIM_CIDER_TRACK_MS");
    track_delay = track_ms ? atol(track_ms) * HZ / 1000 : 0;
    play_pending = true;
    return true;
}

int cider_play_status(void)
{
    if (play_pending)
    {
        if (TIME_BEFORE(current_tick, play_at))
            return 0;
        play_pending = false;
        track_at = current_tick + track_delay;
        playback.have = playback.available = true;
        playback.rate = 1;
        if (getenv("PODBOX_SIM_CIDER_HOLD_LCD"))
            lcd_hold_updates(true);
    }
    return playback.have ? 1 : -1;
}

bool cider_active(void) { return playback.have; }

void cider_snapshot(struct cider_state *state) { *state = playback; }

void cider_status(struct cider_status *status)
{
    *status = (struct cider_status){
        .available = playback.available, .rate = playback.rate,
        .loading = playback.have && TIME_BEFORE(current_tick, track_at),
    };
}

void cider_control(const char *command, int value)
{
    (void)value;
    if (playback.have && !strcmp(command, "toggle"))
        playback.rate = !playback.rate;
}

void cider_wps_update(struct wps_state *state)
{
    static struct mp3entry track;
    if (!cider_active())
        return;
    track.title = playback.title;
    track.artist = playback.artist;
    track.album = playback.album;
    track.length = 123000;
    strlcpy(track.path, "cider/sim", sizeof(track.path));
    state->id3 = &track;
    state->nid3 = NULL;
    state->paused = !playback.rate;
}

int cider_browse_covers(bool home)
{
    (void)home;
    return GO_TO_ALBUM_COVERS;
}

bool cider_context_show(void) { return false; }
int cider_context_destination(void) { return GO_TO_PREVIOUS; }

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

static bool trim_sim_arena(void)
{
    unsigned slots = 0, bytes = 0;
    int oldest = -1, tier = 0;
    for (int i = 0; i < ART_ALBUMS; i++)
        for (int t = 0; t < 2; t++)
            if (art[i].resident[t])
            {
                slots++;
                bytes += cs_payload_size(pack_tier(t));
                if (oldest < 0 || art[i].used[t] < art[oldest].used[tier])
                {
                    oldest = i;
                    tier = t;
                }
            }
    if (slots <= CIDER_SLOTS && bytes <= CIDER_RAM_BYTES)
        return false;
    art[oldest].resident[tier] = false;
    return true;
}

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
            if (!scanned && xs_only && t == 1 && !art[i].stored[t])
                art[i].due[t] = current_tick;
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
                art[i].resident[t] = scanned || !xs_only;
            }
            if (active_art == i && active_tier == t)
            {
                active_art = -1;
                panel_perf_record(PP_ART_DONE, (current_tick - issued_at) * (1000000 / HZ),
                                  fnv1a_str(key) & 0x3fffffffu, tier);
            }
            art[i].due[t] = 0;
            if (art[i].resident[t])
            {
                art[i].used[t] = ++use_clock;
                if (serial_art)
                    while (trim_sim_arena()) {}
                if (disk)
                    panel_perf_record(PP_ART_HIT, 0, fnv1a_str(key) & 0x3fffffffu, tier);
                art_revision++;
                button_queue_post(BUTTON_REDRAW, 0);
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
    if (!art_delay)
        return cider_art_local(key, tier);
    if (i < 0 || (strcmp(tier, "M") && strcmp(tier, "XS")))
        return false;
    int t = art_tier(tier);
    if (!art[i].resident[t])
        return false;
    art[i].used[t] = ++use_clock;
    return true;
}

bool cider_copy_art(const char *key, const char *tier, void *pixels, unsigned size)
{
    if (!cider_has_art(key, tier))
        return false;
    if (decode_ticks)
        sleep(!strcmp(tier, "M") ? decode_ticks : 1);
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
    long ready = !serial_art && art[i].warmed ? art[i].warmed + art_delay :
                                                current_tick + art_delay;
    /* Represent 48 KiB/s application throughput and one foreground worker.
     * Host preparation and the payload transfer charge separate time. */
    long wire = serial_art ? (cs_payload_size(pack_tier(tier)) * HZ + 49151) / 49152 : 0;
    art[i].due[tier] = art[i].stored[tier] ? current_tick + (serial_art ? 4 : 0) :
                       MAX(ready, current_tick + HZ / 20) + wire;
}

bool cider_prefetch_art(const char *key, const char *tier)
{
    if (!art_delay || !scanned)
        return false;
    int t = art_tier(tier);
    if (serial_art)
    {
        int i = art_index(key);
        if (i < 0 || active_art >= 0 || xs_only || art[i].resident[t])
            return false;
        schedule_art(i, t);
        active_art = i;
        active_tier = t;
        issued_at = current_tick;
        panel_perf_record(PP_ART_ISSUE, 0, fnv1a_str(key) & 0x3fffffffu, pack_tier(t));
        return true;
    }
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
    if (serial_art)
        for (int i = 0; i < ART_ALBUMS; i++)
            if (art[i].stored[1])
                art[i].due[1] = 0;
    for (unsigned n = 0; n < count; n++)
        if (cider_art_local(keys[n], tier))
            schedule_art(art_index(keys[n]), art_tier(tier));
}

void cider_cover_read_ahead(const char *const *keys, unsigned count, unsigned full)
{
    if (!art_delay)
        return;
    for (unsigned i = 0; i < count; i++)
    {
        int at = art_index(keys[i]);
        if (at < 0)
            continue;
        if (art[at].stored[1])
            schedule_art(at, 1);
        if (art[at].stored[0] && (i < full || !art[at].stored[1]))
            schedule_art(at, 0);
    }
}

bool cider_cover_preview_local(const char *key)
{
    return cider_art_local(key, "XS") || cider_has_art(key, "XS");
}

bool cider_copy_cover_preview(const char *key, uint16_t pixels[32 * 32])
{
    return cider_copy_art(key, "XS", pixels, 32 * 32 * sizeof(*pixels));
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
{
    (void)tier;
    if (!serial_art || active_art < 0)
        return;
    char key[65];
    fixture_art_key(active_art, key);
    for (unsigned i = 0; i < count; i++)
        if (!strcmp(keys[i], key))
            return;
    DEBUGF("sim Cider cancelled %d\n", active_art);
    art[active_art].due[active_tier] = 0;
    active_art = -1;
}

void cider_request_xs(const char *const *keys, unsigned count)
{
    if (!art_delay || !scanned)
        return;
    if (serial_art && count)
    {
        if (cider_art_local(keys[0], "XS"))
            schedule_art(art_index(keys[0]), 1);
        else
            cider_prefetch_art(keys[0], "XS");
        return;
    }
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
