/* Resident Cider receiver and bounded cache. Disk I/O stays on its worker. */
#include "config.h"
#include "ui/motion_schedule.h"
#include "screens/playback/cider.h"
#include "screens/playback/handoff.h"
#include "screens/playback/cider_library.h"
#include "screens/playback/cider_store.h"
#include "audio.h"
#include "input/action.h"
#include "audio/spectrum_meter.h"
#include "system/activity.h"
#include "lcd.h"
#include "powermgmt.h"
#include "podlink.h"
#include "screens/system/podlink_media.h"
#include "screens/system/agents_background.h"
#include "kernel.h"
#include "thread.h"
#include "system.h"
#include "storage.h"
#include "usb.h"
#include "file.h"
#include "dir.h"
#include "rbpaths.h"
#include "remote_log.h"
#include "database/tagcache.h"
#include "metadata/album_blur.h"
#include "screens/covers/panel_perf.h"
#include "system/hash.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

/* iPod-local art store: pack files + on-disk hash table (cider_store.c). The
 * old per-cover files under cider-cache/ and cider-v1/ are ignored. */
#define STORE_ROOT ROCKBOX_DIR "/cider-art"
#ifndef CIDER_STORE_CAP
#define CIDER_STORE_CAP CS_DEFAULT_CAP
#endif
/* How long a store hit holds back the USB request before racing it. */
#define HOLD_PREVIEW (HZ / 25)
#define HOLD_ACTIVE (HZ)
#define HOLD_ASLEEP (5 * HZ / 2)
static struct mutex cider_mutex;
static struct cider_state state;
static struct cider_cache cache;
static struct cider_transfer transfer;
static uint32_t cover_sent_us;
static long track_sent_at;
static bool initialized, active, disk_lookup, request_ready, disk_running;
static volatile bool sleep_pause_due;
static bool source_seen;
static uint32_t source_generation;
static volatile bool host_live;
static bool live_reply;
static volatile long live_at;
static char live_nonce[17];
static volatile bool spectrum_visible;
static volatile uint32_t spectrum_generation;
static bool spectrum_subscribed, spectrum_fresh;
static uint16_t spectrum_id;
static long spectrum_renew_at, spectrum_received;
static unsigned char spectrum_frame[2 * SPECTRUM_MAX_BANDS];
static struct cider_store_stats diag;
static bool load_requested, build_on, build_changed;
static long diag_at, ls_at, dirty_at;
static uint32_t art_ack_request;
static unsigned int art_ack_bytes;
static bool art_local_miss;
static unsigned int clock_us(void)
{
#ifdef USEC_TIMER
    return USEC_TIMER;
#else
    return (uint32_t)current_tick * (1000000 / HZ);
#endif
}
static uint32_t next_request, grant, granted, cancel_request;
static bool cancel_all;
static long hello_at, request_at;
static unsigned int request_tries;
static uint32_t play_request, loading_request;
static struct cider_modes modes;
static int play_reply;
static long play_at, art_used_at;
static char warm_keys[7 * 65], warm_tier[3];
static bool warm_ready;
static char commands[12][384];
static struct cider_store store;
static struct {
    enum { TRACK_IDLE, TRACK_READ, TRACK_HIT, TRACK_MISS, TRACK_WRITE } state;
    uint8_t key[32], data[CS_TRACK_BYTES];
} track_cache;
#define BULK_COVERS 6
static bool host_bulk, store_writing;
static uint32_t bulk_request;
static unsigned int bulk_received, bulk_count, bulk_tier;
static uint8_t bulk_key[32];
static long bulk_at;
static bool host_store, host_diag, ls_due, want_ready, list_req, list_ready, pin_on;
static long hold_until, want_at, pin_at;
static uint32_t list_cursor;
static unsigned int list_len, nfills, noffers, nacks;
static char list_page[480], pin_key[65];
static struct { char key[65]; unsigned char tier; } fills[24], offers[8];
static struct { char key[65]; unsigned char tier; uint32_t crc; } acks[8];
static struct { uint8_t key[32]; unsigned char tier; long until; } missed[8];
static unsigned int miss_next;
static struct { uint32_t epoch, count, digest, total; } snap;
static struct cs_item list_items[16];
static char xs_keys[5][65];
static unsigned int xs_count;
static uint32_t xs_request;
static long xs_at;
static bool xs_sent;
static unsigned int ncommands, art_revision;
static void (* volatile art_notify)(void);
static unsigned char disk_data[CS_REC_MAX] CACHEALIGN_ATTR;
static long disk_stack[DEFAULT_STACK_SIZE * 6 / sizeof(long)];
static struct event_queue disk_queue;
static unsigned int disk_thread_id;
static volatile uint32_t status_word;
_Static_assert(sizeof cache + sizeof transfer + sizeof disk_data +
               sizeof disk_stack + sizeof state + sizeof commands +
               sizeof store + sizeof track_cache + sizeof list_page + sizeof fills +
               sizeof offers + sizeof acks + sizeof missed +
               sizeof list_items + sizeof modes + sizeof status_word + sizeof loading_request +
               sizeof diag + 4 * 1024 +
               (120 + 112 + 56 + 36) * 1024 < 1152 * 1024,
               "Cider resident, WPS, pages, browser and carousel below 1152 KiB");

static void publish_status(void)
{
    status_word = (state.available ? 1u : 0) | (state.muted ? 2u : 0) |
        (state.loading ? 4u : 0) | (state.rate ? 8u : 0) |
        ((unsigned)state.volume << 4) | ((unsigned)state.shuffle << 11) |
        ((unsigned)state.repeat << 12) | ((state.remote & 1u) << 15);
}

static void pin_apply(void);
static void store_text(const unsigned char *d, unsigned int n);
static void background_start(void);
static bool bulk_idle(void);
static void bulk_cancel(void);
static const char *tier_name(unsigned int t);
static unsigned int store_record(char *record, unsigned int size);

static void notify_art_ready(void)
{
    void (*fn)(void) = art_notify;
    if (fn)
        fn();
    /* Active carousel motion polls every frame and wakes the loader directly;
     * avoid posting BUTTON_REDRAW mid-scroll so the loop keeps partial damage. */
    if (!fn || !ui_motion_active())
        button_queue_post(BUTTON_REDRAW, 0);
}

static void cider_log(const char *format, ...)
{
    char text[REMOTE_LOG_DATA];
    va_list ap;
    va_start(ap, format);
    vsnprintf(text, sizeof text, format, ap);
    va_end(ap);
    remote_log_event(REMOTE_LOG_CIDER, current_tick, 0, 0,
                     text, strlen(text) + 1);
}

static uint32_t u32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static bool valid_key(const char *key)
{
    if (strlen(key) != 64) return false;
    for (int i = 0; i < 64; i++)
        if (!((key[i] >= '0' && key[i] <= '9') ||
              (key[i] >= 'a' && key[i] <= 'f'))) return false;
    return true;
}

static void cover_event(enum panel_perf_event event, const char *key,
                        const char *tier, uint32_t elapsed)
{
    int t = cs_tier(tier);
    if (t >= 0)
        panel_perf_record(event, elapsed, fnv1a_str(key) & 0x3fffffffu, t);
}

static void cover_cancelled(void)
{
    if (transfer.active && !transfer.fill && request_tries)
        cover_event(PP_ART_CANCEL, transfer.key, transfer.tier,
                    clock_us() - cover_sent_us);
}

static void fail_art(void)
{
    cover_cancelled();
    cider_cache_fail(&cache, transfer.key, transfer.tier, current_tick, HZ);
    transfer.active = disk_lookup = request_ready = false;
    if (!strcmp(transfer.key, state.art) && !strcmp(transfer.tier, "L"))
        album_blur_request_wps();
}

static void finish_xs(void)
{
    for (unsigned int i = 0; i < xs_count; i++)
        if (cider_cache_find(&cache, xs_keys[i], "XS") < 0)
            cider_cache_fail(&cache, xs_keys[i], "XS", current_tick, HZ);
    xs_count = 0;
}

static bool host_palette;

static void reset(void)
{
    mutex_lock(&cider_mutex);
    state.position = cider_position(&state,current_tick,HZ);
    state.available = false;
    state.loading = false;
    loading_request = 0;
    publish_status();
    if (play_request) play_reply = -1;
    cider_library_reset();
    state.generation = state.library_generation = 0;
    host_live = live_reply = host_palette = false;
    cover_cancelled();
    transfer.active = false;
    disk_lookup = request_ready = false;
    ncommands = grant = granted = cancel_request = 0;
    memset(&modes, 0, sizeof modes);
    xs_count = 0; cancel_all = false;
    warm_keys[0] = 0; warm_ready = false;
    host_store = host_diag = want_ready = list_req = list_ready = false;
    host_bulk = false; bulk_request = 0;
    art_ack_request = 0; build_changed = build_on;
    diag.build_state = diag.build_done = diag.build_total = 0;
    diag_at = ls_at = current_tick - 5 * HZ;
    nfills = noffers = nacks = 0; hold_until = 0;
    hello_at = current_tick - 2 * HZ;
    mutex_unlock(&cider_mutex);
    button_queue_post(BUTTON_REDRAW, 0);
}

/* The current burst borrows the ordinary receive buffer while it is idle.
 * Each frame may finish several small covers or carry part of one M cover. */
static bool receive_bulk(const unsigned char *d, unsigned int n)
{
    if (n < 12 || !bulk_request || u32(d + 8) != bulk_request)
        return true;                         /* an interrupted burst */
    if (n > 16364 || !bulk_idle() || transfer.active || xs_count)
        return false;
    if (n == 12)
    {
        if (bulk_received) return false;
        want_at = current_tick + (bulk_count ? -HZ : HZ);
        bulk_request = 0;
        return true;
    }
    unsigned int pos = 12;
    bool dirty = false, valid = true;
    while (n - pos >= 39)
    {
        const unsigned char *item = d + pos;
        unsigned int t = item[32], total = item[33] | item[34] << 8;
        unsigned int offset = item[35] | item[36] << 8;
        unsigned int count = item[37] | item[38] << 8;
        pos += 39;
        if (t < CS_M || t > CS_XS || total != cs_payload_size(t) ||
            offset != bulk_received || offset >= total || !count ||
            count > total - offset || count > n - pos || bulk_count >= BULK_COVERS)
        { valid = false; break; }
        if (!offset)
        {
            memcpy(bulk_key, item, sizeof bulk_key);
            bulk_tier = t;
        }
        else if (t != bulk_tier || memcmp(bulk_key, item, sizeof bulk_key))
        { valid = false; break; }
        memcpy(transfer.data + offset, d + pos, count);
        bulk_received += count;
        pos += count;
        if (bulk_received == total)
        {
            char key[65];
            cs_key_to_hex(bulk_key, key);
            const char *tier = tier_name(t);
            bool cached = cider_cache_find(&cache, key, tier) >= 0;
            if (cider_cache_put(&cache, key, tier, transfer.data, total, true))
            {
                int slot = cider_cache_find(&cache, key, tier);
                cache.slot[slot].bg = !cached;
                dirty = true;
            }
            bulk_received = 0;
            bulk_count++;
        }
    }
    if (dirty)
        queue_post(&disk_queue, 1, 0);
    bulk_at = current_tick;
    return valid && pos == n;
}

static bool receive(const struct podlink_message *m)
{
    bool binary = m->type == PODLINK_BINARY && m->length >= 4 &&
        (!memcmp(m->data, "CAR1", 4) || !memcmp(m->data, "CXS1", 4) ||
         !memcmp(m->data, "CFS1", 4));
    bool text = m->type == PODLINK_TEXT && m->length >= 3 &&
        (!memcmp(m->data, "CA\t", 3) || !memcmp(m->data, "CN\t", 3) ||
         !memcmp(m->data, "CK\t", 3) || !memcmp(m->data, "CL\t", 3) ||
         !memcmp(m->data, "CG\t", 3) || !memcmp(m->data, "CP\t", 3) ||
         !memcmp(m->data, "CF\t", 3) || !memcmp(m->data, "CO\t", 3) ||
         !memcmp(m->data, "CQ\t", 3) || !memcmp(m->data, "CB\t", 3) ||
         !memcmp(m->data, "CH\t", 3));
    if (!binary && !text) return false;
    bool art_ready = false;
    mutex_lock(&cider_mutex);
    if (text && !memcmp(m->data, "CH\t", 3))
    {
        if (cider_live_parse(m->data, m->length, state.generation, live_nonce))
        { host_live = live_reply = true; live_at = current_tick; }
        mutex_unlock(&cider_mutex);
        return true;
    }
    if (binary && !state.generation &&
        (!transfer.generation || m->length < 12 ||
         u32(m->data + 4) != transfer.generation))
    {
        mutex_unlock(&cider_mutex);
        return false;
    }
    if (binary || (text && !memcmp(m->data, "CL\t", 3)))
    {
        if (binary && (m->length < 12 ||
                       u32(m->data + 4) != state.generation))
        {
            mutex_unlock(&cider_mutex);
            return true;
        }
        /* Bytes are returned only after the dedicated sink consumes them. */
        if (m->length > granted)
        {
            cover_cancelled();
            transfer.active = false;
            cider_log("CIDER credit violation tick=%ld bytes=%lu credit=%lu",
                       current_tick, (unsigned long)m->length,
                       (unsigned long)granted);
        }
        else
        {
            granted -= m->length; grant += m->length;
            if (text)
            {
                if (cider_library_receive(m->data, m->length,
                                         state.generation, state.library_generation))
                {
                    art_revision++;
                    art_ready = true;
                    cider_log("page received tick=%ld bytes=%lu",current_tick,
                              (unsigned long)m->length);
                }
            }
            else if (!memcmp(m->data, "CFS1", 4))
            {
                if (!receive_bulk(m->data, m->length))
                    bulk_cancel();
            }
            else if (!memcmp(m->data, "CAR1", 4))
            {
                int result = cider_chunk(&transfer, m->data, m->length);
                if (m->length >= 28)
                    cider_log("chunk req=%lu off=%lu bytes=%lu result=%d",
                              (unsigned long)u32(m->data + 8),
                              (unsigned long)u32(m->data + 12),
                              (unsigned long)m->length - 28, result);
                if (result > 0)
                {
                    if (!transfer.fill)
                        cover_event(PP_ART_DONE, transfer.key, transfer.tier,
                                    clock_us() - cover_sent_us);
                    if (host_diag) { art_ack_request = transfer.request; art_ack_bytes = transfer.total; }
                    /* Palette pixels are transient and do not wake the store. */
                    bool persist = strcmp(transfer.tier, "P") != 0;
                    if (persist)
                    {
                        dirty_at = current_tick;
                        if (disk_running) queue_post(&disk_queue, 1, 0);
                    }
                    art_revision++;
                    disk_lookup = false;
                    cider_cache_put(&cache, transfer.key, transfer.tier,
                                    transfer.data, transfer.total, persist);
                    if (persist && !strcmp(transfer.key, state.art))
                        album_blur_request_wps();
                    if (transfer.fill)
                        cider_cache_demote(&cache, transfer.key, transfer.tier);
                    else
                        art_ready = true;
                    pin_apply();
                    cider_log("CIDER art done tick=%ld req=%lu bytes=%lu",
                               current_tick, (unsigned long)transfer.request,
                               (unsigned long)transfer.total);
                }
                else if (result < 0)
                {
                    transfer.received = 0;
                    request_ready = transfer.active;
                }
            }
            else if (binary && m->length >= 16 && xs_count &&
                     u32(m->data + 8) == xs_request)
            {
                unsigned int count = u32(m->data + 12), pos = 16;
                bool added = false;
                for (unsigned int i = 0; i < count && count <= 5; i++)
                {
                    char key[65];
                    if (pos + 40 > m->length) break;
                    for (int j = 0; j < 32; j++)
                        snprintf(key + j * 2, 3, "%02x", m->data[pos + j]);
                    unsigned int size = u32(m->data + pos + 32);
                    unsigned int crc = u32(m->data + pos + 36);
                    pos += 40;
                    if (size > m->length - pos || size != 2060 ||
                        podlink_crc32(m->data + pos, size) != crc) break;
                    bool cached = cider_cache_put(&cache, key, "XS", m->data + pos, size, true);
                    added |= cached;
                    if (cached && !strcmp(key, state.art))
                        album_blur_request_wps();
                    pos += size;
                }
                if (added)
                {
                    art_revision++;
                    dirty_at = current_tick;
                    if (disk_running) queue_post(&disk_queue, 1, 0);
                    art_ready = true;
                }
                if (pos == m->length)
                {
                    if (host_diag) { art_ack_request = xs_request; art_ack_bytes = m->length; }
                    finish_xs();
                }
            }
        }
    }
    else if (text && (!memcmp(m->data, "CF\t", 3) ||
                      !memcmp(m->data, "CO\t", 3) ||
                      !memcmp(m->data, "CQ\t", 3) || !memcmp(m->data, "CB\t", 3) ||
         !memcmp(m->data, "CH\t", 3)))
    {
        unsigned char remote = state.remote;
        store_text(m->data, m->length);
        if (remote != state.remote) button_queue_post(BUTTON_REDRAW, 0); /* Home row label */
    }
    else
    {
        uint32_t previous = state.generation;
        uint32_t library = state.library_generation;
        bool available = state.available;
        bool had_state = state.have;
        bool was_loading = state.loading;
        char previous_art[sizeof(state.art)];
        memcpy(previous_art, state.art, sizeof(previous_art));
        uint32_t position = cider_position(&state,current_tick,HZ);
        cider_state_apply(&state, m->data, m->length, current_tick, HZ);
        if (state.generation != previous || state.available != available ||
            strcmp(previous_art, state.art))
            album_blur_playing_changed();
        if (available != state.available || had_state != state.have)
            button_queue_post(BUTTON_REDRAW, 0);
        if (was_loading != state.loading)
            cider_log("CIDER loading received tick=%ld loading=%d seq=%lu", current_tick,
                      state.loading, (unsigned long)state.seq);
        if (available && !state.available)
        {
            state.position = position;
            state.position_tick = current_tick;
        }
        if (library != state.library_generation)
        {
            cider_library_invalidate(state.library_generation);
            diag.build_state = 0;
        }
        if (state.generation != previous || state.available != available)
            cider_log("availability gen=%lu available=%d reason=%s",
                      (unsigned long)state.generation, state.available,
                      state.reason);
        if (state.generation != previous)
        {
            host_live = live_reply = host_palette = false;
            cider_library_reset();
            host_store = host_diag = false;
            diag.build_state = 0;
            if (play_request) play_reply = -1;
            cover_cancelled();
            transfer.active = false; ncommands = 0;
            xs_count = 0;
            memset(&modes, 0, sizeof modes);
            disk_lookup = request_ready = false;
            grant = granted = cancel_request = 0;
        }
        if (!state.loading) loading_request = 0;
        if (!memcmp(m->data, "CK\t", 3))
        {
            if (xs_count &&
                cider_reply_status(m->data, m->length, state.generation, xs_request) < 0)
                finish_xs();
            if (transfer.active &&
                cider_reply_status(m->data, m->length, state.generation,
                                   transfer.request) < 0)
            {
                /* A failed art answer is terminal for this request. Keep
                 * the placeholder; do not resend the rejected fill. */
                fail_art();
            }
            if (cider_reply_status(m->data, m->length, state.generation, loading_request) < 0)
            {
                state.loading = false; loading_request = 0;
                cider_log("CIDER loading failed tick=%ld", current_tick);
            }
            int reply = cider_reply_status(m->data, m->length,
                                           state.generation, play_request);
            if (reply) play_reply = reply;
            for (int i = 0; i < CIDER_MODE_COUNT; i++)
                if (modes.pending[i] &&
                    cider_reply_status(m->data, m->length, state.generation,
                                       modes.request[i]) < 0)
                    cider_mode_failed(&modes, modes.request[i]);
        }
        if (!memcmp(m->data, "CK\t", 3))
            cider_log("CIDER reply tick=%ld %.*s", current_tick,
                       (int)m->length, (const char *)m->data);
    }
    publish_status();
    mutex_unlock(&cider_mutex);
    if (art_ready)
        notify_art_ready();
    return true;
}

static unsigned int next(char *record, unsigned int size)
{
    unsigned int n = 0;
    /* Independent application deadline. Ordinary CN and wire PING cannot renew it.
     * Reset before taking the client mutex; reset() takes it itself. */
    if (host_live && (unsigned long)(current_tick - live_at) >= 4 * HZ)
    {
        cider_log("CIDER application liveness expired tick=%ld", current_tick);
        reset();
    }
    mutex_lock(&cider_mutex);
    if (!state.generation)
    {
        if (TIME_AFTER(current_tick, hello_at + 2 * HZ))
        {
            /* 'loading' already implies the extended CN layout. Keep eight
             * advertised tokens for hosts that enforce the old limit. */
            n = snprintf(record, size, "ci\t1\t%d\tneighbors,warm,play-ack,localstore,remote-name,loading,localdiag-live,spectrum1", CIDER_CREDIT);
            hello_at = current_tick;
        }
    }
    else if (live_reply)
    {
        n = snprintf(record, size, "ch\t%lu\t%s",
                     (unsigned long)state.generation, live_nonce);
        live_reply = false;
    }
    else if (host_diag && art_ack_request)
    {
        n = snprintf(record, size, "lr\t%lu\t%lu\t%u",
                     (unsigned long)state.generation, (unsigned long)art_ack_request, art_ack_bytes);
        art_ack_request = 0;
    }
    else if (sleep_pause_due)
    {
        sleep_pause_due = false;
        n = snprintf(record, size, "cc\t%lu\t%lu\tpause\t0",
                     (unsigned long)state.generation, (unsigned long)++next_request);
    }
    else if (background_start(), ncommands)
    {
        n = snprintf(record, size, "%s", commands[0]);
        memmove(commands[0], commands[1], --ncommands * sizeof commands[0]);
    }
    else if (cancel_request || cancel_all)
    {
        if (cancel_all) xs_sent = false;
        n = snprintf(record, size, "cx\t%lu\t%lu",
                     (unsigned long)state.generation,
                     cancel_all ? 0ul : (unsigned long)cancel_request);
        cancel_request = 0; cancel_all = false;
    }
    else if ((n = cider_library_next_list(record, size, state.generation)))
    {
        if (!granted) grant = CIDER_CREDIT;
        cider_log("page request tick=%ld %s", current_tick, record);
    }
    else if (grant)
    {
        n = snprintf(record, size, "cg\t%lu\t%lu",
                     (unsigned long)state.generation, (unsigned long)grant);
        cider_log("CIDER credit tick=%ld grant=%lu outstanding=%lu",
                   current_tick, (unsigned long)grant,
                   (unsigned long)granted);
        granted += grant; grant = 0;
    }
    else if (xs_count && !xs_sent)
    {
        n = snprintf(record, size, "cb\t%lu\t%lu\t",
                     (unsigned long)state.generation, (unsigned long)xs_request);
        for (unsigned int i = 0; i < xs_count && n < size; i++)
            n += snprintf(record + n, size - n, "%s%s", i ? "," : "", xs_keys[i]);
        xs_sent = true;
        xs_at = current_tick;
    }
    else if (transfer.active &&
             !(disk_lookup && ((diag.build_state == 2 && strcmp(transfer.tier, "XS")) ||
                              TIME_BEFORE(current_tick, hold_until))) &&
             (request_ready || (hold_until && disk_lookup) ||
              TIME_AFTER(current_tick, request_at + 3 * HZ)))
    {
        if (request_tries++ < 3)
        {
            if (request_tries == 1 && !transfer.fill)
            {
                cover_sent_us = clock_us();
                cover_event(PP_ART_ISSUE, transfer.key, transfer.tier, 0);
            }
            n = snprintf(record, size,
                         "cr\t%lu\t%lu\t%s\t%s\t%lu\t%d\t0",
                         (unsigned long)state.generation,
                         (unsigned long)transfer.request, transfer.key,
                         transfer.tier, (unsigned long)transfer.received,
                         transfer.fill ? 4 : 3);
            if (host_diag && n < size)
                n += snprintf(record + n, size - n, "\t%d", art_local_miss ? 1 : 0);
            cider_log("CIDER request tick=%ld req=%lu off=%lu",
                       current_tick, (unsigned long)transfer.request,
                       (unsigned long)transfer.received);
            request_ready = false; request_at = current_tick;
            hold_until = 0;
        }
        else
        {
            cancel_request = transfer.request;
            fail_art();
        }
    }
    /* Keep the artwork lane ahead of album-track read-ahead. Give the
     * panel one track request per second during a run of thumbnails. */
    if (!n && state.generation &&
        (!transfer.active || strcmp(transfer.tier, "XS") ||
         TIME_AFTER(current_tick, track_sent_at + HZ)) &&
        (n = cider_library_next_tracks(record, size, state.generation)))
    {
        track_sent_at = current_tick;
        if (!granted) grant = CIDER_CREDIT;
        cider_log("page request tick=%ld %s", current_tick, record);
    }
    if (!n && warm_ready && state.generation)
    {
        n = snprintf(record, size, "cw\t%lu\t%s\t%s",
                     (unsigned long)state.generation, warm_tier, warm_keys);
        warm_ready = false;
    }
    if (!n && state.generation) n = store_record(record, size);
    mutex_unlock(&cider_mutex);
    return n < size ? n : 0;
}

/* ---- iPod-local store plumbing. Callers hold cider_mutex unless noted. ---- */

/* Inspect, do not consume: only the worker's outer loop may ACK and park.
 * Search past art wakeups so a queued USB event takes precedence. */
static bool disk_usb_pending(void)
{
    return queue_peek_ex(&disk_queue, NULL, 0,
                         QPEEK_FILTER1(SYS_USB_CONNECTED));
}

static int io_open(const char *path, int mode)
{
    return open(path, mode == CS_O_READ ? O_RDONLY : O_RDWR | O_CREAT, 0666);
}
static int io_read(int fd, void *buf, unsigned int n)
{
    int result = read(fd, buf, n);
    if (result > 0) diag.sectors += (result + CS_SECTOR - 1) / CS_SECTOR;
    return result;
}
static int io_write(int fd, const void *buf, unsigned int n) { return write(fd, buf, n); }
static int io_seek(int fd, uint32_t offset) { return lseek(fd, offset, SEEK_SET) < 0 ? -1 : 0; }
static int io_sync(int fd) { return fsync(fd); }
static int io_close(int fd) { return close(fd); }
static int io_remove(const char *path) { return remove(path); }
static int io_fsize(const char *path)
{
    int fd = open(path, O_RDONLY), n = -1;
    if (fd >= 0) { n = ffilesize(fd); close(fd); }
    return n;
}
static void io_mkdir(const char *path) { mkdir(ROCKBOX_DIR); mkdir(path); }
static void io_yield(void) { yield(); }
static const struct cs_io store_io = {io_open, io_read, io_write, io_seek,
    io_sync, io_close, io_remove, io_fsize, io_mkdir, io_yield};

/* RAM-only: could the local store hold it? Recent failed reads are skipped. */
static bool store_local(const char *key, const char *tier)
{
    uint8_t kb[32];
    int t = cs_tier(tier);
    if (!disk_running || t < 0 || !cs_key_from_hex(key, kb) ||
        !cs_maybe(&store, kb, t)) return false;
    for (unsigned int i = 0; i < ARRAYLEN(missed); i++)
        if (missed[i].until && missed[i].tier == t &&
            !memcmp(missed[i].key, kb, 32) &&
            TIME_BEFORE(current_tick, missed[i].until)) return false;
    diag.bloom_hits++;
    return true;
}
static void miss_note(const char *key, const char *tier)
{
    uint8_t kb[32];
    int t = cs_tier(tier);
    if (t < 0 || !cs_key_from_hex(key, kb)) return;
    memcpy(missed[miss_next].key, kb, 32);
    missed[miss_next].tier = t;
    missed[miss_next].until = current_tick + 30 * HZ;
    miss_next = (miss_next + 1) % ARRAYLEN(missed);
}
static void disk_foreground(bool foreground)
{
#ifdef HAVE_PRIORITY_SCHEDULING
    if (disk_thread_id)
        thread_set_priority(disk_thread_id,
                            foreground ? PRIORITY_BUFFERING
                                       : PRIORITY_BACKGROUND);
#else
    (void)foreground;
#endif
}

static void disk_wake_foreground(void)
{
    if (!disk_running)
        return;
    disk_foreground(true);
    if (queue_empty(&disk_queue))
        queue_post(&disk_queue, 1, 0);
}

static bool disk_fg_pending(void)
{
    return nfills > 0 || (transfer.active && disk_lookup) ||
           load_requested || track_cache.state == TRACK_READ;
}

static bool disk_bg_cancelled(void)
{
    return disk_usb_pending() || ui_motion_active() || disk_fg_pending();
}

static void fill_add(const char *key, const char *tier)
{
    if (nfills >= ARRAYLEN(fills) || cider_cache_find(&cache, key, tier) >= 0)
        return;
    for (unsigned int i = 0; i < nfills; i++)
        if (!strcmp(fills[i].key, key) && fills[i].tier == cs_tier(tier)) return;
    snprintf(fills[nfills].key, sizeof fills[0].key, "%s", key);
    fills[nfills++].tier = cs_tier(tier);
    disk_wake_foreground();
}
static const char *tier_name(unsigned int t)
{
    static const char *const names[] = {"L", "M", "S", "XS"};
    return names[t & 3];
}

/* The album track list pins its L cover: never evicted while pinned. */
static void pin_apply(void)
{
    if (pin_on) cider_cache_pin(&cache, pin_key, "L");
}

static void store_text(const unsigned char *d, unsigned int n)
{
    if (d[1] == 'B')
    {
        if (!host_diag) return;
        /* Strict bounded decimal fields; never use scanf on the 32-bit ABI. */
        char text[96], *p, *end;
        if (n >= sizeof text) return;
        memcpy(text, d, n); text[n] = 0; p = text + 3;
        uint32_t values[5];
        for (int i = 0; i < 5; i++)
        {
            if (*p < '0' || *p > '9') return;
            uint32_t value = 0;
            while (*p >= '0' && *p <= '9')
            {
                unsigned int digit = *p++ - '0';
                if (value > (UINT32_MAX - digit) / 10) return;
                value = value * 10 + digit;
            }
            end = p;
            if (i < 4 ? *end != '\t' : (*end != 0 || end != text + n)) return;
            values[i] = value; p = end + 1;
        }
        if (values[0] != state.generation || values[1] != state.library_generation ||
            values[2] > values[3] || values[4] > 3) return;
        diag.build_done = values[2]; diag.build_total = values[3]; diag.build_state = values[4];
        if (build_on && (values[4] == 2 || values[4] == 3))
        { build_on = false; build_changed = true; }
    }
    else if (d[1] == 'F')
    {
        char want[48];
        int len = snprintf(want, sizeof want, "CF\t%lu\tlocalstore",
                           (unsigned long)state.generation);
        if (len > 0 && n == (unsigned int)len && !memcmp(d, want, n))
        { host_store = true; ls_due = true; load_requested = true;
          if (disk_running) queue_post(&disk_queue, 1, 0); }
        len = snprintf(want, sizeof want, "CF\t%lu\tlocaldiag", (unsigned long)state.generation);
        if (len > 0 && n == (unsigned int)len && !memcmp(d, want, n)) host_diag = true;
        len = snprintf(want, sizeof want, "CF\t%lu\tspectrum1", (unsigned long)state.generation);
        if (state.generation && n == (unsigned int)len && !memcmp(d, want, n))
        {
            spectrum_generation = state.generation;
            spectrum_subscribed = spectrum_fresh = false;
        }
        len = snprintf(want, sizeof want, "CF\t%lu\tpalette1",
                       (unsigned long)state.generation);
        if (state.generation && n == (unsigned int)len && !memcmp(d, want, n))
            host_palette = true;
        len = snprintf(want, sizeof want, "CF\t%lu\tartbulk1",
                       (unsigned long)state.generation);
        if (state.generation && n == (unsigned int)len && !memcmp(d, want, n))
            host_bulk = true;
        unsigned char remote = cider_remote_parse((const char *)d, n, state.generation);
        if (remote && remote != state.remote)
        {
            state.remote = remote;
            noffers = 0;
        }
    }
    else if (d[1] == 'Q' && host_store)
    {
        uint32_t cursor;
        if (cs_parse_cursor((const char *)d, n, state.generation, &cursor) == 0)
        { list_req = true; list_cursor = cursor; queue_post(&disk_queue, 1, 0); }
    }
    else if (d[1] == 'O' && host_store)
    {
        static uint8_t keys[5][32];
        int tier, count = cs_parse_offer((const char *)d, n, state.generation,
                                         &tier, keys, 5);
        for (int i = 0; i < count && noffers < ARRAYLEN(offers); i++)
        {
            char hex[65];
            cs_key_to_hex(keys[i], hex);
            bool dup = false;
            for (unsigned int j = 0; j < noffers; j++)
                dup |= offers[j].tier == tier && !strcmp(offers[j].key, hex);
            if (dup) continue;
            memcpy(offers[noffers].key, hex, sizeof hex);
            offers[noffers++].tier = tier;
        }
    }
}

static bool begin_transfer(const char *key, const char *tier, bool fill)
{
    if (cider_cache_backoff(&cache, key, tier, current_tick)) return false;
    if (transfer.active)
    {
        cover_cancelled();
        cancel_all |= cancel_request != 0;
        cancel_request = transfer.request;
    }
    if (!fill) art_used_at = current_tick;
    memset(&transfer, 0, sizeof transfer);
    transfer.active = true; transfer.generation = state.generation;
    transfer.request = ++next_request; transfer.fill = fill;
    snprintf(transfer.key, sizeof transfer.key, "%s", key);
    snprintf(transfer.tier, sizeof transfer.tier, "%s", tier);
    /* A local hit holds the USB request back; a miss goes out at once. */
    bool persist = strcmp(tier, "P") != 0;
    load_requested |= persist;
    bool local = !fill && persist &&
                 (store_local(key, tier) || (disk_running && !store.loaded));
    art_local_miss = !local;
    if (!local) diag.misses++;
    disk_lookup = local;
    request_ready = !local; request_tries = 0; request_at = current_tick;
    hold_until = local ? current_tick + (!strcmp(tier, "XS") ? HOLD_PREVIEW :
        storage_disk_is_active() ? HOLD_ACTIVE : HOLD_ASLEEP) : 0;
    if (!granted) grant = CIDER_CREDIT;
    if (persist && disk_running)
    {
        if (local) disk_wake_foreground();
        else queue_post(&disk_queue, 1, 0);
    }
    if (request_ready || cancel_request || cancel_all) podlink_wake();
    return true;
}

/* Idle-time work, started from next(): the pinned L cover first (above idle
 * fill, below any visible cover, which replaces the transfer), then fills the
 * host offered. Fills run only while the disk already spins and the UI idles;
 * nothing here wakes a sleeping disk. */
/* Video playback is a plugin activity. Let local decoding, interaction,
 * database work and existing foreground art use the device first. */
static bool bulk_idle(void)
{
    enum current_activity activity = get_current_activity();
    return state.remote == 3 && state.available && !ui_motion_active() &&
           activity != ACTIVITY_PLUGIN && activity != ACTIVITY_VIDEOBROWSER &&
           !(audio_status() & AUDIO_STATUS_PLAY) && !tagcache_is_busy() &&
           !nfills && track_cache.state != TRACK_READ &&
           TIME_AFTER(current_tick, get_action_tick() + 3 * HZ) &&
           (build_on || storage_disk_is_active());
}

static void bulk_cancel(void)
{
    if (!bulk_request) return;
    cancel_all |= cancel_request != 0;
    cancel_request = bulk_request;
    bulk_request = 0;
    bulk_received = 0;
}

static void background_start(void)
{
    if (bulk_request && (!bulk_idle() || transfer.active || ncommands || xs_count ||
                         TIME_AFTER(current_tick, bulk_at + 5 * HZ)))
        bulk_cancel();
    if (!state.generation || transfer.active || ncommands || cancel_request ||
        cancel_all || xs_count) return;
    if (pin_on)
    {
        int i = cider_cache_find(&cache, pin_key, "L");
        if (i >= 0) pin_apply();
        else if (TIME_AFTER(current_tick, pin_at + 5 * HZ))
        {
            pin_at = current_tick;
            begin_transfer(pin_key, "L", false);
            return;
        }
    }
    bool rufin_fill = state.remote == 3 && state.available && diag.build_state == 1;
    if (host_bulk && state.remote == 3)
        return;     /* negotiated bursts use store_record, below foreground */
    if (!host_store || !store.loaded || cs_busy(&store) ||
        (!storage_disk_is_active() && !build_on && !rufin_fill) ||
        (!build_on && TIME_BEFORE(current_tick, art_used_at + 3 * HZ)) ||
        store.total + CS_REC_MAX > store.cap) return;
    for (int i = 0; i < CIDER_SLOTS; i++)
        if (cache.slot[i].length && cache.slot[i].dirty && cache.slot[i].bg)
            return;                            /* previous fill not written */
    if (noffers)
    {
        char key[65];
        unsigned int tier = offers[0].tier;
        snprintf(key, sizeof key, "%s", offers[0].key);
        memmove(&offers[0], &offers[1], --noffers * sizeof offers[0]);
        if (cider_cache_find(&cache, key, tier_name(tier)) < 0)
            begin_transfer(key, tier_name(tier), true);
    }
    else if (TIME_AFTER(current_tick, want_at + (build_on || rufin_fill ? HZ / 2 : 4 * HZ)))
        want_ready = true;
}

static unsigned int hex8(char *out, const uint8_t *p, unsigned int n)
{
    for (unsigned int i = 0; i < n; i++)
    {
        out[i * 2] = "0123456789abcdef"[p[i] >> 4];
        out[i * 2 + 1] = "0123456789abcdef"[p[i] & 15];
    }
    return n * 2;
}

/* Store protocol records, lowest priority. See cider-remote-protocol.md. */
static unsigned int store_record(char *record, unsigned int size)
{
    unsigned long gen = state.generation;
    if (!host_store) return 0;
    if (host_diag && build_changed)
    {
        build_changed = false;
        return snprintf(record, size, "lf\t%lu\t%d", gen, build_on);
    }
    if (ls_due && store.loaded && !nacks && !list_ready &&
        !TIME_BEFORE(current_tick, ls_at + 5 * HZ))
    {
        ls_due = false; ls_at = current_tick;
        return snprintf(record, size, "ls\t%lu\t%lu\t%lu\t%lu\t%lu\t%lu", gen,
                        (unsigned long)snap.epoch, (unsigned long)snap.count,
                        (unsigned long)snap.digest,
                        (unsigned long)(snap.total >> 10),
                        (unsigned long)(store.cap >> 10));
    }
    if (list_ready && list_len < size)
    {
        memcpy(record, list_page, list_len);
        list_ready = false;
        return list_len;
    }
    if (nacks)
    {
        unsigned int n = snprintf(record, size, "la\t%lu\t%s\t%s\t%lu", gen,
                                  acks[0].key, tier_name(acks[0].tier),
                                  (unsigned long)acks[0].crc);
        memmove(&acks[0], &acks[1], --nacks * sizeof acks[0]);
        return n;
    }
    if (host_bulk && host_store && !bulk_request && bulk_idle() &&
        !transfer.active && !xs_count && !store_writing && store.loaded &&
        !cs_busy(&store) && (build_on || diag.build_state == 1) &&
        snap.total <= store.cap && store.cap - snap.total >= 2560 &&
        TIME_AFTER(current_tick, want_at + HZ / 4))
    {
        for (unsigned int i = 0; i < CIDER_SLOTS; i++)
            if (cache.slot[i].length && cache.slot[i].dirty)
                return 0;                 /* persist or defer the preceding covers */
        bulk_request = ++next_request;
        bulk_at = want_at = current_tick;
        bulk_received = bulk_count = 0;
        if (!granted) grant = CIDER_CREDIT;
        return snprintf(record, size, "lb\t%lu\t%lu\t%lu", gen,
                        (unsigned long)bulk_request, (unsigned long)(store.cap - snap.total));
    }
    if (want_ready)
    {
        want_ready = false; want_at = current_tick;
        return snprintf(record, size, "lw\t%lu\t%u", gen,
                        (unsigned int)(ARRAYLEN(offers) - noffers));
    }
    if (host_diag && TIME_AFTER(current_tick, diag_at + 5 * HZ))
    {
        diag_at = current_tick;
        return snprintf(record, size,
            "ld\t%lu\t%d\t%lu\t%lu\t%lu\t%lu\t%lu\t%lu\t%lu\t%lu\t%lu\t%lu\t%lu\t%d",
            gen, store.loaded, (unsigned long)snap.count,
            (unsigned long)diag.bloom_hits, (unsigned long)diag.false_positives,
            (unsigned long)diag.sectors, (unsigned long)diag.hits,
            (unsigned long)diag.misses, (unsigned long)diag.writes,
            (unsigned long)diag.deferrals, (unsigned long)diag.loads,
            (unsigned long)diag.read_us, (unsigned long)diag.max_read_us, diag.last_error);
    }
    return 0;
}

/* ---- worker (single owner of the store) ---- */

static void snap_store(void)
{
    mutex_lock(&cider_mutex);
    snap.epoch = store.epoch; snap.count = store.count;
    snap.digest = store.digest; snap.total = store.total;
    ls_due = true; art_revision++;
    mutex_unlock(&cider_mutex);
}

static void build_list(void)
{
    uint32_t cursor;
    unsigned int n = 0, len;
    mutex_lock(&cider_mutex);
    cursor = list_cursor;
    mutex_unlock(&cider_mutex);
    store.cancelled = disk_bg_cancelled;
    int next = cs_list(&store, cursor, list_items, ARRAYLEN(list_items), &n,
                       disk_data, sizeof disk_data);
    store.cancelled = disk_usb_pending;
    if (next == CS_BUSY || disk_usb_pending()) return;
    mutex_lock(&cider_mutex);
    if (next < 0 || (uint32_t)next <= cursor) { next = 0; n = 0; }
    len = snprintf(list_page, sizeof list_page, "lv\t%lu\t%lu\t%lu\t%u\n",
                   (unsigned long)state.generation, (unsigned long)cursor,
                   (unsigned long)next, n);
    for (unsigned int i = 0; i < n && len + 28 < sizeof list_page; i++)
    {
        len += hex8(list_page + len, list_items[i].key, 8);
        list_page[len++] = '0' + list_items[i].tier;
        uint8_t c[4] = {list_items[i].crc >> 24, list_items[i].crc >> 16,
                        list_items[i].crc >> 8, list_items[i].crc};
        len += hex8(list_page + len, c, 4);
        list_page[len++] = '\n';
    }
    list_len = len; list_ready = true; list_req = false;
    mutex_unlock(&cider_mutex);
}

/* Store -> arena: one hit ends the USB wait for the transfer's cover. */
static bool read_store(const char *key, const char *tier, bool transfer_job,
                       uint32_t req)
{
    disk_foreground(true);
    uint8_t kb[32];
    const uint8_t *payload = NULL;
    unsigned int length = 0;
    int t = cs_tier(tier);
    bool maybe = t >= 0 && cs_key_from_hex(key, kb) && cs_maybe(&store, kb, t);
    unsigned int started = clock_us();
    int r = t < 0 || !cs_key_from_hex(key, kb) ? CS_BAD :
            cs_get(&store, kb, t, disk_data, sizeof disk_data, &payload, &length);
    if (disk_usb_pending()) return false;
    unsigned int us = clock_us() - started;
    diag.read_us = us;
    if (us > diag.max_read_us) diag.max_read_us = us;
    if (r == CS_MISS && maybe) diag.false_positives++;
    if (r < CS_MISS) diag.last_error = r;
    mutex_lock(&cider_mutex);
    bool ok = r == CS_OK && cider_cache_put(&cache, key, tier, payload, length, false);
    if (ok)
    {
        cover_event(PP_ART_HIT, key, tier, us);
        diag.hits++; art_revision++; pin_apply();
        if (!strcmp(key, state.art))
            album_blur_request_wps();
    }
    else { diag.misses++; diag.build_state = 1; miss_note(key, tier); }
    bool wake_usb = false;
    if (transfer_job && transfer.active && transfer.request == req && disk_lookup)
    {
        if (ok)
        {
            if (request_tries)
            {
                cover_cancelled();
                cancel_request = transfer.request;
                wake_usb = true;
            }
            transfer.active = false;
        }
        else if (!request_tries) { request_ready = true; art_local_miss = true; wake_usb = true; }
        disk_lookup = false;
    }
    mutex_unlock(&cider_mutex);
    if (ok)
        notify_art_ready();
    if (wake_usb)
        podlink_wake();
    return ok;
}

/* Dirty arena covers -> store, a few per wake. Returns true when a started
 * eviction means the rest must retry. Callers check the disk already spins. */
static bool store_write_batch(void)
{
    char key[65], tier[3];
    bool more = false;
    for (int batch = 0; batch < 6; batch++)
    {
        unsigned int length = 0;
        bool bg = false;
        mutex_lock(&cider_mutex);
        for (int i = 0; i < CIDER_SLOTS && nacks < ARRAYLEN(acks); i++)
            if (cache.slot[i].length && cache.slot[i].dirty &&
                (!cache.slot[i].bg || !host_bulk || state.remote != 3 || bulk_idle()))
            {
                struct cider_slot *s = &cache.slot[i];
                length = s->length; bg = s->bg;
                memcpy(disk_data + CS_REC_HEAD, cache.data + s->offset, length);
                snprintf(key, sizeof key, "%s", s->key);
                snprintf(tier, sizeof tier, "%s", s->tier);
                s->dirty = false;
                store_writing = true;
                break;
            }
        mutex_unlock(&cider_mutex);
        if (length && (tagcache_is_busy() || !queue_empty(&disk_queue) ||
                       ui_motion_active() || disk_fg_pending() ||
                       (bg && host_bulk && state.remote == 3 && !bulk_idle())))
        {   /* the UI or the database needs the disk: write it next time */
            mutex_lock(&cider_mutex);
            int i = cider_cache_find(&cache, key, tier);
            if (i >= 0) { cache.slot[i].dirty = true; cache.slot[i].bg = bg; }
            store_writing = false;
            mutex_unlock(&cider_mutex);
            break;
        }
        if (!length) break;
        uint8_t kb[32];
        uint32_t crc = 0;
        int t = cs_tier(tier);
        int r = t < 0 || !cs_key_from_hex(key, kb) ? CS_BAD :
                cs_put(&store, kb, t, disk_data + CS_REC_HEAD, length,
                       disk_data, sizeof disk_data, &crc);
        bool usb = disk_usb_pending();
        mutex_lock(&cider_mutex);
        store_writing = false;
        if (usb)
        {
            mutex_unlock(&cider_mutex);
            return false;
        }
        if (r == CS_OK || r == CS_DUP)
        {
            if (r == CS_OK) diag.writes++;
            if (host_store && nacks < ARRAYLEN(acks))
            {
                snprintf(acks[nacks].key, sizeof acks[0].key, "%s", key);
                acks[nacks].tier = t; acks[nacks++].crc = crc;
            }
            if (bg) cider_cache_drop_bg(&cache, key, tier);
        }
        else if (r == CS_BUSY && cs_busy(&store))
        {   /* eviction started: keep the cover, retry after it */
            int i = cider_cache_find(&cache, key, tier);
            if (i >= 0) { cache.slot[i].dirty = true; cache.slot[i].bg = bg; }
            more = true;
            mutex_unlock(&cider_mutex);
            break;
        }
        else
        {
            diag.last_error = r;
            int i = cider_cache_find(&cache, key, tier);
            if (i >= 0) cache.slot[i].dirty = true;
        }
        mutex_unlock(&cider_mutex);
        if (r == CS_OK) snap_store();
        yield();
    }
    return more;
}

/* Pack I/O runs outside cider_mutex. The mailbox holds one bounded page
 * and keeps its buffers stable until the worker publishes the result. */
static bool service_track_cache(void)
{
    mutex_lock(&cider_mutex);
    int operation = track_cache.state;
    mutex_unlock(&cider_mutex);
    if (operation != TRACK_READ && operation != TRACK_WRITE)
        return false;
    int result;
    if (operation == TRACK_READ)
    {
        const uint8_t *payload;
        unsigned length;
        result = cs_maybe(&store, track_cache.key, CS_TRACKS) ?
            cs_get(&store, track_cache.key, CS_TRACKS, disk_data, sizeof disk_data,
                   &payload, &length) : CS_MISS;
        if (result == CS_OK)
            memcpy(track_cache.data, payload, length);
    }
    else
        result = cs_put(&store, track_cache.key, CS_TRACKS, track_cache.data,
                        sizeof track_cache.data, disk_data, sizeof disk_data, NULL);
    if (operation == TRACK_WRITE && result == CS_BUSY && cs_busy(&store))
        return true;
    mutex_lock(&cider_mutex);
    track_cache.state = operation == TRACK_WRITE ? TRACK_IDLE :
                        result == CS_OK ? TRACK_HIT : TRACK_MISS;
    mutex_unlock(&cider_mutex);
    return false;
}

static void disk_worker(void)
{
    bool more = false, more_fg = false, was_busy = false;
    long load_at = current_tick - HZ;
    for (;;)
    {
        struct queue_event ev;
        if (!more_fg)
        {
            mutex_lock(&cider_mutex);
            if (!disk_fg_pending())
                disk_foreground(false);
            mutex_unlock(&cider_mutex);
        }
        queue_wait_w_tmo(&disk_queue, &ev,
                         more_fg ? TIMEOUT_NOBLOCK : more ? HZ / 20 : HZ);
        if (more_fg && ev.id == SYS_TIMEOUT)
            yield();
        more_fg = false;
        if (ev.id == SYS_USB_CONNECTED)
        {
            podlink_media_disconnect();
            podlink_media_work();
            cs_unload(&store);            /* sync, close, forget the index */
            /* No UI/service mutex on the handover path. The service resets
             * the link on detach; clean up any late list after remount. */
            usb_acknowledge(SYS_USB_CONNECTED_ACK, ev.data);
            do { queue_wait(&disk_queue, &ev); }
            while (ev.id != SYS_USB_DISCONNECTED);
            mutex_lock(&cider_mutex);
            list_req = list_ready = false;
            track_cache.state = TRACK_IDLE;
            mutex_unlock(&cider_mutex);
            more = more_fg = was_busy = false;
            load_at = current_tick - 30 * HZ;
            continue;
        }
        /* Drain older wakeups without work, preserving USB connect/disconnect
         * order. Pulling a later connect out of the queue could mistake an
         * older disconnect for the end of the new storage session. */
        if (disk_usb_pending()) continue;
        if (podlink_media_work())
        {
            /* Do not start unrelated cache I/O between ready stream chunks. */
            more = true;
            continue;
        }
        if (tagcache_is_busy()) continue;
        bool defer_maintenance = ui_motion_active();
        bool active = storage_disk_is_active();
        char key[65], tier[3];
        bool lookup;
        uint32_t req;
        mutex_lock(&cider_mutex);
        lookup = transfer.active && disk_lookup; req = transfer.request;
        snprintf(key, sizeof key, "%s", transfer.key);
        snprintf(tier, sizeof tier, "%s", transfer.tier);
        bool dirty = false, dirty_foreground = false;
        for (int i = 0; i < CIDER_SLOTS; i++)
            if (cache.slot[i].length && cache.slot[i].dirty)
            {
                dirty = true;
                dirty_foreground |= !cache.slot[i].bg;
            }
        bool track_read = track_cache.state == TRACK_READ;
        bool track_write = !defer_maintenance && track_cache.state == TRACK_WRITE;
        bool have_fills = nfills > 0;
        bool fg_wanted = lookup || have_fills || load_requested || track_read;
        bool wanted = fg_wanted || track_write ||
                      (!defer_maintenance && (build_on || dirty));
        if (dirty && !active) diag.deferrals++;
        /* Foreground USB covers must eventually reach disk even when Cider
         * is the only source (no local music to keep the disk spinning). */
        bool flush = dirty_foreground && TIME_AFTER(current_tick, dirty_at + HZ);
        bool build = build_on || (!host_bulk && state.remote == 3 &&
                                  state.available && diag.build_state == 1);
        load_requested = false;
        mutex_unlock(&cider_mutex);
        /* Load when the disk already spins, or when the user's browsing
         * needs it (that spin-up is theirs, not a background wake). */
        bool load = !store.loaded &&
                    (wanted || (active && !defer_maintenance)) &&
                    !TIME_BEFORE(current_tick, load_at + HZ);
        if (load)
        {
            load_at = current_tick;
            diag.loads++;
            disk_foreground(fg_wanted);
            int result = cs_load(&store, disk_data, sizeof disk_data,
                        (uint32_t)current_tick * 2654435761u + 0x5bd1e995u);
            if (disk_usb_pending()) continue;
            if (result == CS_OK) snap_store();
            else diag.last_error = result;
        }
        if (disk_usb_pending()) continue;
        if (lookup) read_store(key, tier, true, req);
        /* Visible window and neighbours share one spin-up. */
        for (int n = 0; n < 6; n++)
        {
            if (disk_usb_pending()) break;
            mutex_lock(&cider_mutex);
            bool have = nfills > 0;
            if (have)
            {
                snprintf(key, sizeof key, "%s", fills[0].key);
                snprintf(tier, sizeof tier, "%s", tier_name(fills[0].tier));
                memmove(&fills[0], &fills[1], --nfills * sizeof fills[0]);
            }
            mutex_unlock(&cider_mutex);
            if (!have) break;
            read_store(key, tier, false, 0);
            yield();
        }
        bool track_more = false;
        if (track_read && !disk_usb_pending())
        {
            disk_foreground(true);
            track_more = (!store.loaded && !load) || service_track_cache();
        }
        if (disk_usb_pending()) continue;
        mutex_lock(&cider_mutex);
        more_fg = store.loaded && disk_fg_pending();
        if (!more_fg)
            disk_foreground(false);
        bool busy = transfer.active || TIME_BEFORE(current_tick, art_used_at + HZ);
        bool answer = !more_fg && list_req && !list_ready && !ui_motion_active() &&
                      (active || build || state.remote == 3);
        mutex_unlock(&cider_mutex);
        if (more_fg) continue;
        active = storage_disk_is_active();
        if (answer && store.loaded) build_list();
        if (disk_usb_pending()) continue;
        more = track_more || (track_write && ((!store.loaded && !load) ||
                                              service_track_cache()));
        if ((active || flush || build || more) && !busy && store.loaded &&
            !ui_motion_active())
        {
            more |= store_write_batch();
            bool idle = TIME_AFTER(current_tick, art_used_at + 5 * HZ);
            if (!queue_empty(&disk_queue) || ui_motion_active() ||
                disk_fg_pending())
                continue;
            store.cancelled = disk_bg_cancelled;
            more |= cs_maintain(&store, disk_data, sizeof disk_data, idle);
            store.cancelled = disk_usb_pending;
            if (disk_usb_pending()) continue;
            if (was_busy && !cs_busy(&store)) snap_store();
            was_busy = cs_busy(&store);
        }
        if (!more && !more_fg && queue_empty(&disk_queue)) cs_release(&store);
    }
}

static bool cider_sleep_pause(void)
{
    if (!active) return false;
    /* Power thread never waits for the transport mutex or a queue slot. */
    sleep_pause_due = true;
    return true;
}
static bool spectrum_wanted(void)
{
    return spectrum_visible && lcd_active() &&
        get_current_activity() == ACTIVITY_WPS && cider_active() &&
        state.available && state.have && state.rate;
}

/* Runs under the resident service mutex. No transport API calls here. */
static bool spectrum_receive(const struct podlink_message *m)
{
    const unsigned char *data = m->data;
    if (m->type != PODLINK_BINARY || m->length < 4 || memcmp(data, "CSP1", 4))
        return false;
    if (m->length != 10 + sizeof spectrum_frame) return true;
    for (unsigned i = 0; i < sizeof spectrum_frame; i++)
        if (data[10 + i] > 100) return true;
    mutex_lock(&cider_mutex);
    if (spectrum_subscribed && spectrum_generation == state.generation &&
        u32(data + 4) == state.generation &&
        (unsigned)(data[8] | (data[9] << 8)) == spectrum_id && spectrum_wanted())
    {
        memcpy(spectrum_frame, data + 10, sizeof spectrum_frame);
        spectrum_received = current_tick;
        spectrum_fresh = true;
    }
    mutex_unlock(&cider_mutex);
    return true;
}

static unsigned int spectrum_next(char *record, unsigned int size)
{
    if (!state.generation || spectrum_generation != state.generation) return 0;
    mutex_lock(&cider_mutex);
    bool want = spectrum_wanted();
    unsigned int n = 0;
    if (want != spectrum_subscribed || (want && TIME_AFTER(current_tick, spectrum_renew_at)))
    {
        if (want != spectrum_subscribed)
        {
            spectrum_fresh = false;
            if (want && !++spectrum_id) ++spectrum_id;
            spectrum_subscribed = want;
        }
        spectrum_renew_at = current_tick + HZ / 2;
        n = snprintf(record, size, "cs\t%lu\t%u", (unsigned long)state.generation,
                     want ? spectrum_id : 0u);
    }
    mutex_unlock(&cider_mutex);
    return n < size ? n : 0;
}

void cider_spectrum_wps(bool visible)
{
    spectrum_visible = visible;
}

bool cider_spectrum_levels(unsigned char *levels)
{
    if (!spectrum_generation || !cider_connected()) return false;
    mutex_lock(&cider_mutex);
    bool fresh = spectrum_generation == state.generation && spectrum_fresh &&
        spectrum_wanted() && (unsigned long)(current_tick - spectrum_received) < HZ * 12 / 100;
    if (fresh) memcpy(levels, spectrum_frame, sizeof spectrum_frame);
    mutex_unlock(&cider_mutex);
    return fresh;
}

static bool resident_receive(const struct podlink_message *m)
{
    if (disk_running && podlink_media_enqueue(m))
    {
        queue_post(&disk_queue, 1, 0);
        return true;
    }
    return handoff_receive(m) || spectrum_receive(m) || receive(m) ||
           agents_background_receive(m);
}

static unsigned int resident_next(char *record, unsigned int size)
{
    bool refill = false;
    unsigned int n = podlink_media_next(record, size, &refill);
    if (refill) queue_post(&disk_queue, 1, 0);
    if (!n) n = agents_background_next(record, size);
    if (!n) n = handoff_next(record, size);
    if (!n) n = next(record, size);
    return n ? n : spectrum_next(record, size);
}

static unsigned int resident_next_binary(unsigned char *out, unsigned int size,
                                         uint32_t *raw)
{
    bool refill = false;
    unsigned int n = podlink_media_next_binary(out, size, &refill, raw);
    if (refill) queue_post(&disk_queue, 1, 0);
    return n;
}

static void resident_reset(void)
{
    agents_background_reset();
    podlink_media_disconnect();
    if (disk_running) queue_post(&disk_queue, 1, 0);
    handoff_reset();
    spectrum_generation = 0;
    reset();
}

void cider_init(void)
{
    static const struct podlink_resident consumer = {
        .receive = resident_receive,
        .next = resident_next,
        .reset = resident_reset,
        .next_binary = resident_next_binary,
    };
    if (initialized) return;
    set_sleep_timer_callback(cider_sleep_pause);
    mutex_init(&cider_mutex);
    handoff_init();
    podlink_media_init();
    queue_init(&disk_queue, true);
    cs_init(&store, &store_io, STORE_ROOT, CIDER_STORE_CAP);
    store.cancelled = disk_usb_pending;
    initialized = true;
    reset();
    podlink_set_resident(&consumer);
    disk_thread_id = create_thread(disk_worker, disk_stack,
                    sizeof disk_stack, 0, "cider-cache"
                    IF_PRIO(, PRIORITY_BACKGROUND) IF_COP(, CPU));
    disk_running = disk_thread_id != 0;
    if (!disk_running) queue_delete(&disk_queue);
}

bool cider_connected(void)
{
    return initialized && state.generation && podlink_host_connected() &&
        (!host_live || (unsigned long)(current_tick - live_at) < 4 * HZ);
}
bool cider_available(void)
{
    return initialized && state.available && state.have &&
           (unsigned long)current_tick - state.received < 30 * HZ &&
           cider_connected();
}
/* Preview pending selections immediately. Availability selects the system
 * source; selection ACKs finish the requested track handoff. */
bool cider_active(void) { return active || (play_request && play_reply >= 0); }
bool cider_play_pending(void) { return play_request != 0; }
void cider_source_poll(void)
{
    if (handoff_hold_source()) return;
    bool online = cider_available();
    if (online == source_seen && (!online || source_generation == state.generation)) return;
    source_seen = online;
    source_generation = online ? state.generation : 0;
    /* One Now Playing source. Never resume local audio on an unplug. */
    int local = audio_status();
    if (online && (local & AUDIO_STATUS_PLAY) && !(local & AUDIO_STATUS_PAUSE)) audio_pause();
    active = online;
    album_blur_playing_changed();
}
void cider_remote_source(void)
{
    if (handoff_hold_source()) return;
    bool use_cider = cider_available();
    if (use_cider && (audio_status() & AUDIO_STATUS_PLAY)) audio_pause();
    bool changed = active != use_cider;
    active = use_cider;
    if (changed)
        album_blur_playing_changed();
}
void cider_snapshot(struct cider_state *out)
{
    mutex_lock(&cider_mutex);
    *out = state;
    if (!cider_connected() ||
        (unsigned long)current_tick - state.received >= 30 * HZ)
        out->available = false;
    mutex_unlock(&cider_mutex);
}

void cider_current_id(char id[sizeof(state.id)])
{
    mutex_lock(&cider_mutex);
    if (cider_available())
        memcpy(id, state.id, sizeof(state.id));
    else
        id[0] = '\0';
    mutex_unlock(&cider_mutex);
}

void cider_current_art(char key[65])
{
    mutex_lock(&cider_mutex);
    if (cider_available())
        memcpy(key, state.art, sizeof(state.art));
    else
        key[0] = '\0';
    mutex_unlock(&cider_mutex);
}

void cider_loading_drawn(bool loading)
{
    cider_log("CIDER loading drawn tick=%ld loading=%d seq=%lu", current_tick,
              loading, (unsigned long)state.seq);
}
void cider_status(struct cider_status *out)
{
    uint32_t bits = status_word;
    out->available = (bits & 1) && cider_connected() &&
        (unsigned long)current_tick - state.received < 30 * HZ;
    out->muted = (bits & 2) != 0;
    out->loading = out->available && (bits & 4);
    out->rate = (bits & 8) ? 1000 : 0;
    out->volume = (bits >> 4) & 127;
    out->shuffle = (bits >> 11) & 1;
    out->repeat = (bits >> 12) & 7;
}
/* The backend the host named (CF remote); read lock-free from the status word. */
const char *cider_remote_name(void)
{
    return (status_word & (1u << 15)) ? "Rufin" : "Cider";
}
/* Queue one control; the request id, or 0 when nothing was queued. */
static uint32_t queue_control(const char *command, int value)
{
    if (!state.generation || ncommands >= ARRAYLEN(commands) ||
        (!state.available && strcmp(command, "next") && strcmp(command, "previous") &&
         strcmp(command, "volume-step") && strcmp(command, "volume"))) return 0;
    snprintf(commands[ncommands++], sizeof commands[0],
             "cc\t%lu\t%lu\t%s\t%d", (unsigned long)state.generation,
             (unsigned long)++next_request, command, value);
    return next_request;
}
/* Home row entry: a host that names its backend may switch to its first choice. */
void cider_remote_home(void)
{
    mutex_lock(&cider_mutex);
    if (state.remote & 2) queue_control("remote-home", 0);
    mutex_unlock(&cider_mutex);
}
void cider_control(const char *command, int value)
{
    mutex_lock(&cider_mutex);
    uint32_t req = queue_control(command, value);
    if (req && !strcmp(command, "volume-step"))
    {
        state.volume = MAX(0, MIN(100, state.volume + value));
        state.muted = state.volume == 0;
    }
    else if (req && !strcmp(command, "volume") && value >= 0 && value <= 100)
    {
        state.volume = value;
        state.muted = value == 0;
    }
    if (req && (!strcmp(command, "next") || !strcmp(command, "previous") ||
                !strcmp(command, "seek")))
    {
        state.loading = true;
        loading_request = req;
        cider_log("CIDER loading action tick=%ld req=%lu command=%s", current_tick,
                  (unsigned long)req, command);
    }
    publish_status();
    mutex_unlock(&cider_mutex);
}
/* Cider's own shuffle (0/1) or repeat (0 none, 1 one, 2 all), or the value
 * just asked for while Cider has not yet reported it. */
int cider_mode_get(int mode)
{
    mutex_lock(&cider_mutex);
    int live = mode == CIDER_MODE_REPEAT ? state.repeat : state.shuffle;
    int value = cider_mode_show(&modes, mode, live, current_tick, HZ);
    mutex_unlock(&cider_mutex);
    return value;
}
/* Set an explicit value on Cider, shown at once; retries are harmless. */
void cider_mode_set(int mode, int value)
{
    if (mode != CIDER_MODE_SHUFFLE && mode != CIDER_MODE_REPEAT) return;
    mutex_lock(&cider_mutex);
    uint32_t request = queue_control(mode == CIDER_MODE_REPEAT ? "repeat5"
                                                               : "shuffle", value);
    if (request) cider_mode_ask(&modes, mode, value, current_tick, request);
    mutex_unlock(&cider_mutex);
}
void cider_cancel_art(void)
{
    bool wake = false;
    mutex_lock(&cider_mutex);
    if (xs_count && xs_sent)
    {
        cancel_all |= cancel_request != 0;
        cancel_request = xs_request;
        wake = true;
    }
    xs_count = 0;
    if (transfer.active)
    {
        cover_cancelled();
        cancel_all |= cancel_request != 0;
        cancel_request = transfer.request;
        wake = true;
    }
    transfer.active = disk_lookup = request_ready = false;
    mutex_unlock(&cider_mutex);
    if (wake)
        podlink_wake();
}
bool cider_palette_supported(void)
{
    mutex_lock(&cider_mutex);
    bool supported = host_palette;
    mutex_unlock(&cider_mutex);
    return supported;
}

static bool request_art(const char *key, const char *tier, bool prefetch)
{
    if (!valid_key(key) || (strcmp(tier, "L") && strcmp(tier, "M") &&
        strcmp(tier, "S") && strcmp(tier, "XS") && strcmp(tier, "P")))
        return false;
    mutex_lock(&cider_mutex);
    /* A visible cover takes priority over its companion palette. Both are
     * polled by WPS, so letting P replace L would restart them every redraw. */
    bool palette = !strcmp(tier, "P");
    bool cover_in_flight = transfer.active && !transfer.fill &&
                           strcmp(transfer.tier, "P");
    bool in_batch = false;
    if (!strcmp(tier, "XS"))
        for (unsigned i = 0; i < xs_count; i++)
            in_batch |= !strcmp(xs_keys[i], key);
    bool started = false;
    if (!in_batch &&
        (!prefetch || (state.generation && (!transfer.active || transfer.fill))) &&
        (!palette || (host_palette && !cover_in_flight)) &&
        cider_cache_find(&cache, key, tier) < 0 &&
        (!transfer.active || strcmp(transfer.key, key) ||
         strcmp(transfer.tier, tier) || transfer.fill))
        started = begin_transfer(key, tier, false);
    mutex_unlock(&cider_mutex);
    return started;
}

void cider_request_art(const char *key, const char *tier)
{
    request_art(key, tier, false);
}

bool cider_request_playing_art(const char *key)
{
    mutex_lock(&cider_mutex);
    bool failed = false;
    if (cider_available() && !strcmp(key, state.art))
    {
        failed = cider_cache_backoff(&cache, key, "L", current_tick);
        if (!failed && (!transfer.active || transfer.fill) &&
            cider_cache_find(&cache, key, "L") < 0)
            begin_transfer(key, "L", false);
    }
    mutex_unlock(&cider_mutex);
    return failed;
}
void cider_unpin_art(void)
{
    mutex_lock(&cider_mutex);
    pin_on = false;
    cider_cache_pin(&cache, NULL, "L");
    mutex_unlock(&cider_mutex);
}
/* Album track list: keep that album's L cover resident (local store hit,
 * else USB + store write). Visible list covers replace the transfer first. */
void cider_pin_art(const char *key)
{
    if (!valid_key(key)) { cider_unpin_art(); return; }
    mutex_lock(&cider_mutex);
    if (!pin_on || strcmp(pin_key, key))
    {
        snprintf(pin_key, sizeof pin_key, "%s", key);
        pin_on = true; pin_at = current_tick - 10 * HZ;
        pin_apply();
    }
    mutex_unlock(&cider_mutex);
}
bool cider_copy_art(const char *key, const char *tier, void *pixels,
                     unsigned int size)
{
    bool found = false;
    mutex_lock(&cider_mutex);
    int i = cider_cache_find(&cache, key, tier);
    if (i >= 0 && cache.slot[i].length >= size + 12)
    {
        memcpy(pixels, cache.data + cache.slot[i].offset + 12, size);
        found = true;
    }
    mutex_unlock(&cider_mutex);
    return found;
}

void cider_request_xs(const char *const *keys, unsigned int count)
{
    if (!count || count > ARRAYLEN(xs_keys)) return;
    /* Resident receive/next acquire cider_mutex while holding PodLink's
     * service mutex. Read the negotiated limit before taking cider_mutex. */
    unsigned int max_message = podlink_max_message();
    unsigned int capacity = max_message > 16 ? (max_message - 16) / (2060 + 40) : 0;
    mutex_lock(&cider_mutex);
    if (xs_count && xs_sent && TIME_AFTER(current_tick, xs_at + 3 * HZ))
        finish_xs();
    if (!state.generation || xs_count)
    { mutex_unlock(&cider_mutex); return; }
    for (unsigned int i = 0; i < count && xs_count < capacity; i++)
        if (valid_key(keys[i]) && cider_cache_find(&cache, keys[i], "XS") < 0 &&
            !(transfer.active && !strcmp(transfer.tier, "XS") &&
              !strcmp(transfer.key, keys[i])) &&
            !cider_cache_backoff(&cache, keys[i], "XS", current_tick))
        {
            if (store_local(keys[i], "XS")) fill_add(keys[i], "XS");
            else snprintf(xs_keys[xs_count++], sizeof xs_keys[0], "%s", keys[i]);
        }
    if (xs_count)
    {
        xs_request = ++next_request;
        xs_sent = false;
        art_used_at = current_tick;
        if (!granted) grant = CIDER_CREDIT;
    }
    bool wake = xs_count != 0;
    mutex_unlock(&cider_mutex);
    if (wake)
        podlink_wake();
}

/* context: a list route, or "" for none; the host then queues the list
 * around the song. A host that does not know native-context just refuses it. */
static bool play_route(const char *route, const char *context, int index)
{
    if (!cider_available()) return false;
    bool with = context && *context && index >= 0 &&
                strlen(context) + 64 < sizeof commands[0];
    mutex_lock(&cider_mutex);
    if (!play_request && state.have && cider_route_matches(route, state.id))
    {
        /* Opening the current song never rebuilds its queue or seeks to zero. */
        uint32_t req = state.rate ? ++next_request : queue_control("resume", 0);
        if (req)
        {
            play_request = req; play_at = current_tick;
            play_reply = state.rate ? 1 : 0;
            cider_log("CIDER same song tick=%ld resume=%d", current_tick, !state.rate);
        }
        mutex_unlock(&cider_mutex);
        return req != 0;
    }
    bool queued = !play_request && ncommands + (with ? 1 : 0) < ARRAYLEN(commands) &&
                  strlen(route) + 48 < sizeof commands[0];
    if (queued && with)
    {
        snprintf(commands[ncommands++], sizeof commands[0],
                 "cc\t%lu\t%lu\tnative-context\t%d|%s",
                 (unsigned long)state.generation,
                 (unsigned long)++next_request, index, context);
    }
    if (queued)
    {
        play_request = ++next_request;
        play_at = current_tick; play_reply = 0;
        snprintf(commands[ncommands++], sizeof commands[0],
                 "cc\t%lu\t%lu\tnative-play\t%s",
                 (unsigned long)state.generation,
                 (unsigned long)play_request, route);
        state.loading = true;
        loading_request = play_request;
        publish_status();
        cider_log("CIDER loading action tick=%ld req=%lu command=native-play", current_tick,
                  (unsigned long)play_request);
        cider_log("play queued req=%lu", (unsigned long)play_request);
    }
    mutex_unlock(&cider_mutex);
    return queued;
}
bool cider_play(const char *route) { return play_route(route, "", -1); }
bool cider_play_in(const char *route, const char *context, int index)
{
    return play_route(route, context, index);
}

/* Only the UI commits the playback source, after the host confirms selection. */
int cider_play_status(void)
{
    if (!initialized) return 0;
    if (!play_request) return play_reply;
    /* Called from button_get() on the UI thread: never block on a PIP mutex
     * there. The fields are words; a short IRQ-off section is atomic on this
     * single core. */
    int oldlevel = disable_irq_save();
    int reply = play_reply;
    if (play_request && !reply &&
        TIME_AFTER(current_tick, play_at + 6 * HZ)) reply = -1;
    bool commit = reply > 0 && play_request != 0;
    if (reply) { play_request = 0; play_reply = reply; }
    restore_irq(oldlevel);
    if (commit)
    {
        if (audio_status() & AUDIO_STATUS_PLAY) audio_pause();
        active = true;
        album_blur_playing_changed();
    }
    return reply;
}
void cider_local_play(void)
{
    if (!initialized || (!active && !play_request)) return;
    mutex_lock(&cider_mutex);
    bool pending = play_request != 0;
    if (pending) { play_request = 0; play_reply = -1; }
    mutex_unlock(&cider_mutex);
    if (!active && !pending) return;
    cider_control("source-local", 0);
    active = false;
    album_blur_playing_changed();
}
void cider_page_open(const char *route)
{
    mutex_lock(&cider_mutex);
    cider_library_open(route);
    mutex_unlock(&cider_mutex);
    if (*route)
        podlink_wake();
}

bool cider_lyrics_matches(const char *id, uint32_t generation, uint32_t library)
{
    mutex_lock(&cider_mutex);
    bool same = active && state.available && state.generation == generation &&
                state.library_generation == library && !strcmp(state.id, id);
    mutex_unlock(&cider_mutex);
    return same;
}
int cider_page_total(void)
{
    mutex_lock(&cider_mutex);
    int n = cider_library_total();
    mutex_unlock(&cider_mutex);
    return n;
}
bool cider_page_stale(void)
{
    mutex_lock(&cider_mutex);
    bool value = cider_library_stale();
    mutex_unlock(&cider_mutex);
    return value;
}
bool cider_page_pending(void)
{
    mutex_lock(&cider_mutex);
    bool pending = cider_library_pending();
    mutex_unlock(&cider_mutex);
    return pending;
}

bool cider_page_row(int index, struct cider_row *row)
{
    mutex_lock(&cider_mutex);
    bool have = cider_library_row(index, row);
    mutex_unlock(&cider_mutex);
    if (!have)
        podlink_wake();
    return have;
}

bool cider_page_peek(int index, struct cider_row *row)
{
    mutex_lock(&cider_mutex);
    bool have = cider_library_peek(index, row);
    mutex_unlock(&cider_mutex);
    return have;
}

uint32_t cider_panel_revision(void)
{
    mutex_lock(&cider_mutex);
    uint32_t revision = cider_connected() ? cider_library_revision() : 0;
    mutex_unlock(&cider_mutex);
    return revision;
}

bool cider_panel_album(int index, struct cider_row *row, uint32_t *revision)
{
    mutex_lock(&cider_mutex);
    *revision = cider_library_revision();
    bool have = *revision && cider_connected() && cider_library_row(index, row);
    mutex_unlock(&cider_mutex);
    return have;
}

int cider_track_cache_read(const uint8_t key[32], void *page, size_t bytes)
{
    if (!disk_running || bytes > sizeof track_cache.data)
        return -1;
    int result = 0;
    mutex_lock(&cider_mutex);
    switch (track_cache.state)
    {
    case TRACK_HIT:
    case TRACK_MISS:
        if (!memcmp(track_cache.key, key, sizeof track_cache.key))
        {
            result = track_cache.state == TRACK_HIT ? 1 : -1;
            if (result > 0)
                memcpy(page, track_cache.data, bytes);
            track_cache.state = TRACK_IDLE;
            break;
        }
        /* The centre changed while the old page was on disk. */
        /* fall through */
    case TRACK_IDLE:
        memcpy(track_cache.key, key, sizeof track_cache.key);
        track_cache.state = TRACK_READ;
        disk_wake_foreground();
        break;
    default:
        break;
    }
    mutex_unlock(&cider_mutex);
    return result;
}

bool cider_track_cache_write(const uint8_t key[32], const void *page, size_t bytes)
{
    if (!disk_running || bytes > sizeof track_cache.data)
        return true;
    mutex_lock(&cider_mutex);
    bool queued = track_cache.state != TRACK_READ && track_cache.state != TRACK_WRITE;
    if (queued)
    {
        memcpy(track_cache.key, key, sizeof track_cache.key);
        memcpy(track_cache.data, page, bytes);
        memset(track_cache.data + bytes, 0, sizeof track_cache.data - bytes);
        track_cache.state = TRACK_WRITE;
        queue_post(&disk_queue, 1, 0);
    }
    mutex_unlock(&cider_mutex);
    return queued;
}

void cider_track_page_open(const char *route)
{
    mutex_lock(&cider_mutex);
    cider_library_tracks_open(route);
    mutex_unlock(&cider_mutex);
}

int cider_track_page_total(bool *stale)
{
    mutex_lock(&cider_mutex);
    int total = cider_library_tracks_total(stale);
    mutex_unlock(&cider_mutex);
    return total;
}

bool cider_track_page_row(int index, struct cider_row *row)
{
    mutex_lock(&cider_mutex);
    bool have = cider_library_track_row(index, row);
    mutex_unlock(&cider_mutex);
    return have;
}

unsigned int cider_art_revision(void)
{
    mutex_lock(&cider_mutex);
    unsigned int value = art_revision;
    mutex_unlock(&cider_mutex);
    return value;
}

bool cider_has_art(const char *key, const char *tier)
{
    mutex_lock(&cider_mutex);
    bool found = cider_cache_find(&cache,key,tier) >= 0;
    mutex_unlock(&cider_mutex);
    return found;
}

bool cider_prefetch_art(const char *key, const char *tier)
{
    return request_art(key, tier, true);
}

void cider_warm_art(const char *const *keys, unsigned int count,
                    const char *tier)
{
    if (count > 7 || (strcmp(tier, "L") && strcmp(tier, "M") &&
                     strcmp(tier, "S") && strcmp(tier, "XS"))) return;
    /* Off the caller's stack: the WPS and browser run on the UI stack. */
    static char joined[sizeof warm_keys];
    unsigned int n = 0;
    mutex_lock(&cider_mutex);
    joined[0] = 0;
    for (unsigned int i = 0; i < count; i++)
    {
        if (!valid_key(keys[i])) continue;
        if (cider_cache_find(&cache, keys[i], tier) >= 0) continue;
        if (store_local(keys[i], tier)) { fill_add(keys[i], tier); continue; }
        n += snprintf(joined + n, sizeof joined - n, "%s%s", n ? "," : "", keys[i]);
    }
    if (strcmp(joined, warm_keys) || strcmp(tier, warm_tier))
    {
        snprintf(warm_keys, sizeof warm_keys, "%s", joined);
        snprintf(warm_tier, sizeof warm_tier, "%s", tier);
        warm_ready = true;
    }
    mutex_unlock(&cider_mutex);
}

void cider_retain_art(const char *const *keys, unsigned int count,
                      const char *tier)
{
    mutex_lock(&cider_mutex);
    bool keep = false;
    for (unsigned int i = 0; i < count; i++)
        if (!strcmp(keys[i],transfer.key)) keep = true;
    /* The WPS's first L key is the playing album. Its P transfer
     * belongs to the same view even though it has a different tier. */
    bool keep_palette = count && !strcmp(tier, "L") &&
                        !strcmp(transfer.tier, "P") &&
                        !strcmp(keys[0], transfer.key);
    if (transfer.active && !keep_palette &&
        (!keep || strcmp(tier, transfer.tier)))
    {
        cover_cancelled();
        cancel_all |= cancel_request != 0;
        cancel_request = transfer.request;
        transfer.active = disk_lookup = request_ready = false;
    }
    mutex_unlock(&cider_mutex);
}

void cider_store_stats(struct cider_store_stats *out)
{
    /* Debug/status callers never block behind disk I/O. */
    int level = disable_irq_save();
    *out = diag;
    out->loaded = store.loaded; out->entries = snap.count; out->build_on = build_on;
    restore_irq(level);
}
void cider_store_build(bool enable)
{
    if (enable && !host_diag) { diag.last_error = CS_NOTREADY; return; }
    int level = disable_irq_save();
    build_on = enable; build_changed = true; load_requested = enable;
    restore_irq(level);
    if (disk_running) queue_post(&disk_queue, 1, 0);
}
bool cider_store_building(void) { return build_on; }
void cider_set_art_notify(void (*fn)(void))
{
    int level = disable_irq_save();
    art_notify = fn;
    restore_irq(level);
}
bool cider_art_local(const char *key, const char *tier)
{
    mutex_lock(&cider_mutex);
    bool local = cider_cache_find(&cache, key, tier) >= 0 || store_local(key, tier);
    mutex_unlock(&cider_mutex);
    return local;
}
void cider_read_ahead(const char *const *keys, unsigned int count, const char *tier)
{
    mutex_lock(&cider_mutex);
    /* Replace stale direction/window jobs on every frame, visible ones first. */
    nfills = 0;
    for (unsigned int i = 0; i < count && nfills < ARRAYLEN(fills); i++)
        if (valid_key(keys[i]) && cider_cache_find(&cache, keys[i], tier) < 0 &&
            store_local(keys[i], tier)) fill_add(keys[i], tier);
    mutex_unlock(&cider_mutex);
}
/* Both tiers must be enqueued together: a redraw replaces the window.
 * Cache previews before large images so a disk hit can be drawn promptly. */
static const char *const cover_previews[] = {"XS", "S", "L"};

void cider_cover_read_ahead(const char *const *keys, unsigned count, unsigned full)
{
    mutex_lock(&cider_mutex);
    nfills = 0;
    for (unsigned i = 0; i < count; i++)
    {
        const char *key = keys[i];
        if (!valid_key(key) || cider_cache_find(&cache, key, "M") >= 0)
            continue;
        bool ready = false, queued = false;
        for (unsigned t = 0; t < ARRAYLEN(cover_previews); t++)
        {
            const char *tier = cover_previews[t];
            ready = cider_cache_find(&cache, key, tier) >= 0;
            if (ready)
                break;
            if (store_local(key, tier))
            {
                fill_add(key, tier);
                queued = true;
                break;
            }
        }
        /* If only M exists, read it as the first visible image. */
        if (!queued && !ready && store_local(key, "M"))
            fill_add(key, "M");
    }
    /* Upgrade existing previews only after their neighbours can get art. */
    for (unsigned i = 0; i < full; i++)
    {
        const char *key = keys[i];
        if (valid_key(key) && store_local(key, "M"))
            fill_add(key, "M");
    }
    if (!store.loaded && disk_running)
    {
        load_requested = true;
        disk_wake_foreground();
    }
    mutex_unlock(&cider_mutex);
}

bool cider_cover_preview_local(const char *key)
{
    bool local = false;
    mutex_lock(&cider_mutex);
    for (unsigned t = 0; t < ARRAYLEN(cover_previews) && !local; t++)
        local = cider_cache_find(&cache, key, cover_previews[t]) >= 0 ||
                store_local(key, cover_previews[t]);
    mutex_unlock(&cider_mutex);
    return local;
}

bool cider_copy_cover_preview(const char *key, uint16_t pixels[32 * 32])
{
    bool found = false;
    mutex_lock(&cider_mutex);
    for (unsigned t = 0; t < ARRAYLEN(cover_previews); t++)
    {
        int slot = cider_cache_find(&cache, key, cover_previews[t]);
        if (slot < 0)
            continue;
        unsigned dim = t == 0 ? 32 : t == 1 ? 56 : 240;
        const unsigned char *src = cache.data + cache.slot[slot].offset + 12;
        for (unsigned y = 0; y < 32; y++)
            for (unsigned x = 0; x < 32; x++)
            {
                unsigned at = 2 * ((y * dim / 32) * dim + x * dim / 32);
                pixels[y * 32 + x] = src[at] | src[at + 1] << 8;
            }
        cover_event(PP_ART_RAM, key, cover_previews[t], 0);
        found = true;
        break;
    }
    mutex_unlock(&cider_mutex);
    return found;
}

unsigned int cider_read_ahead_count(unsigned int speed, unsigned int visible)
{
    /* At least two frames of I/O coverage, and the measured worst read. Bound
     * by both arena bytes and slot count, reserving room for XS previews so
     * M read-ahead cannot evict the visible window in a loop. */
    unsigned int us = MAX(diag.max_read_us, 34000u);
    unsigned int ahead = ((uint64_t)speed * us + 999999u) / 1000000u + 2;
    unsigned int reserve = (pin_on ? cs_payload_size(CS_L) : 0) +
                           16 * (12 + 32 * 32 * 2);
    unsigned int bytes = CIDER_RAM_BYTES > reserve ? CIDER_RAM_BYTES - reserve : 0;
    unsigned int capacity = MIN(CIDER_SLOTS - 8, bytes / (12 + 128 * 128 * 2));
    return MIN(ahead, capacity > visible ? capacity - visible : 0);
}
