/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "metadata/album_blur.h"
#include "bidi.h"
#include "audio.h"
#include "audio/playback.h"
#include "database/path_key.h"
#include "events.h"
#include "core_alloc.h"
#include "crc32.h"
#include "dir.h"
#include "file.h"
#include "font_pbf.h"
#include "kernel.h"
#include "speech/language.h"
#include "lang.h"
#include "settings/settings.h"
#include "panel_cache.h"
#include "panel_perf.h"
#include "rbpaths.h"
#include "screens/playback/cider.h"
#include "screens/playback/cider_library.h"
#include "screens/playback/cider_wps.h"
#include "string-extra.h"
#include "system.h"
#include "system/bg_task.h"
#include "system/appevents.h"
#include "usb.h"

#define PANEL_DIR ROCKBOX_DIR "/podbox/cache/panel"
#define PANEL_MAGIC 0x314c4e50u /* PNL1 */
#define PANEL_SLOTS 4
#define PANEL_BYTES (96 * 1024)
#define PANEL_DISC_MARKS 256
#define PANEL_PACK_ROWS (2 * ALBUM_BLUR_ROWS + 1)

struct panel_disk {
    uint32_t magic, bytes, crc, revision, fonts, stamp, locale;
    int32_t album_seek, artist_seek;
    uint32_t first, count, total, disc_count;
    uint64_t key;
    uint32_t offset[PANEL_FACES], length[PANEL_FACES];
    char artist[CAROUSEL_TRACK_TEXT];
};

struct panel_disc {
    int32_t track, number;
};

union panel_buffer {
    struct panel_cached {
        struct panel_disk disk;
        struct panel_disc discs[PANEL_DISC_MARKS];
        struct panel_cache_row rows[PANEL_PAGE_ROWS];
    } data;
    unsigned char bytes[PANEL_BYTES];
};

_Static_assert(sizeof(struct panel_cached) < PANEL_BYTES, "Panel cache slot");

static struct cached_slot {
    union panel_buffer *buffer;
    struct panel_request request;
    unsigned used, refs;
} slots[PANEL_SLOTS];

static union panel_buffer *scratch;
static struct panel_request window[PANEL_WINDOW], work;
static unsigned pending_count, serial, working_serial, lru_clock;
static uint32_t settled_at, remote_at;
static uint64_t remote_key, local_playing;
static int remote_first;
static long retry_at;
static struct mutex lock;
static const char *const faces[PANEL_FACES] = {
    FONT_DIR "/18-noto-sans.pbf", FONT_DIR "/14-noto-sans.pbf",
    FONT_DIR "/12-noto-sans.pbf",
};
static const unsigned sizes[PANEL_FACES] = {18, 14, 12};
static int fonts[PANEL_FACES] = {-1, -1, -1};
static uint32_t fonts_revision, db_stamp, db_seen_generation, db_seen_revision;
static struct bg_task task;

uint64_t panel_cache_identity(const char *category, const char *a, const char *b)
{
    uint64_t key = UINT64_C(14695981039346656037);
    const char *parts[] = {category, a, b};
    for (unsigned n = 0; n < ARRAYLEN(parts); n++)
    {
        const unsigned char *p = (const unsigned char *)parts[n];
        do {
            key ^= *p;
            key *= UINT64_C(1099511628211);
        } while (*p++);
    }
    return key;
}

bool panel_cache_same(const struct panel_request *a, const struct panel_request *b)
{
    return a->key == b->key && a->first == b->first &&
           a->revision == b->revision && a->generation == b->generation && a->locale == b->locale;
}

static int lookup(const struct panel_request *request, int first)
{
    for (int i = 0; i < PANEL_SLOTS; i++)
    {
        const struct panel_request *r = &slots[i].request;
        if (slots[i].used && r->first == first && request->key == r->key &&
            request->revision == r->revision && request->generation == r->generation &&
            request->locale == r->locale)
            return i;
    }
    return -1;
}

const struct panel_cached *panel_cache_acquire(const struct panel_request *request, int first)
{
    const struct panel_cached *result = NULL;
    mutex_lock(&lock);
    int i = lookup(request, first);
    if (i >= 0)
    {
        ++slots[i].refs;
        slots[i].used = ++lru_clock;
        result = &slots[i].buffer->data;
    }
    mutex_unlock(&lock);
    return result;
}

void panel_cache_release(const struct panel_cached *data)
{
    if (!data)
        return;
    mutex_lock(&lock);
    for (unsigned i = 0; i < PANEL_SLOTS; i++)
        if (&slots[i].buffer->data == data)
        {
            --slots[i].refs;
            break;
        }
    mutex_unlock(&lock);
}

int panel_cache_total(const struct panel_cached *data) { return data->disk.total; }
int panel_cache_discs(const struct panel_cached *data) { return data->disk.disc_count; }
int panel_cache_disc(const struct panel_cached *data, int n) { return data->discs[n].track; }

unsigned panel_cache_disc_number(const struct panel_cached *data, int n)
{
    return data->discs[n].number;
}

const char *panel_cache_artist(const struct panel_cached *data) { return data->disk.artist; }

const struct panel_cache_row *panel_cache_row(const struct panel_cached *data, int index)
{
    if (!data || index < (int)data->disk.first || index - (int)data->disk.first >= (int)data->disk.count)
        return NULL;
    return &data->rows[index - data->disk.first];
}

int panel_cache_font(enum panel_face face)
{
    return fonts[face];
}

/* Only the UI thread binds or unbinds. The slot remains leased while drawn. */
static const unsigned char *pack_at(const struct panel_cached *data, unsigned face)
{
    return data && data->disk.length[face] ?
           (const unsigned char *)data + data->disk.offset[face] : NULL;
}

void panel_cache_bind(const struct panel_cached *primary, const struct panel_cached *adjacent)
{
    for (unsigned i = 0; i < PANEL_FACES; i++)
        font_pbf_pack_bind(fonts[i], pack_at(primary, i), pack_at(adjacent, i));
}

void panel_cache_window(const struct panel_request *requests, unsigned count)
{
    if (!scratch)
        return;
    count = MIN(count, PANEL_WINDOW);
    mutex_lock(&lock);
    bool changed = count != pending_count;
    for (unsigned i = 0; !changed && i < count; i++)
        changed = !panel_cache_same(&window[i], &requests[i]);
    bool cancelled = changed && pending_count &&
                     (count == 0 || !panel_cache_same(&window[0], &requests[0]));
    if (changed)
    {
        ++serial;
        pending_count = count;
        if (count)
            memcpy(window, requests, count * sizeof(*window));
        retry_at = current_tick;
        settled_at = USEC_TIMER;
    }
    mutex_unlock(&lock);
    if (cancelled)
    {
        panel_perf_record(PP_CANCEL, 0, 0, 0);
        if (!bg_task_queued(&task, BG_EVENT_PANEL))
            bg_task_post(&task, BG_EVENT_PANEL);
    }
    if (changed && count)
        panel_perf_record(PP_SETTLE, 0, requests[0].first, requests[0].remote);
}

bool panel_cache_pump(void)
{
    if (!scratch || usb_host_wants_storage())
        return false;
    bool pending = false;
    mutex_lock(&lock);
    for (unsigned i = 0; i < pending_count && !pending; i++)
        pending = lookup(&window[i], window[i].first) < 0;
    bool post = pending && !TIME_BEFORE(current_tick, retry_at);
    mutex_unlock(&lock);
    if (post && !bg_task_queued(&task, BG_EVENT_PANEL))
        bg_task_post(&task, BG_EVENT_PANEL);
    return pending;
}

static bool cancelled(void)
{
    if (usb_host_wants_storage())
        return true;
    mutex_lock(&lock);
    bool stale = serial != working_serial;
    mutex_unlock(&lock);
    return stale;
}

static bool fonts_ready(void)
{
    if (fonts_revision)
        return true;
    uint32_t revision = PANEL_MAGIC;
    for (unsigned i = 0; i < PANEL_FACES; i++)
    {
        if (fonts[i] < 0)
        {
            uint32_t start = USEC_TIMER;
            fonts[i] = font_load_private(faces[i], 96);
            panel_perf_record(PP_FONT, USEC_TIMER - start, sizes[i], 0);
        }
        if (fonts[i] < 0)
            return false;
        revision = (revision << 5 | revision >> 27) ^ font_pbf_pack_revision(fonts[i]);
        if (cancelled())
            return false;
    }
    fonts_revision = revision ? revision : 1;
    return true;
}

static void cache_path(char path[MAX_PATH])
{
    snprintf(path, MAX_PATH, PANEL_DIR "/%08lx%08lx-%u.pnl",
             (unsigned long)(work.key >> 32), (unsigned long)(uint32_t)work.key,
             (unsigned)work.first);
}

static unsigned slot_bytes(const struct panel_disk *d)
{
    return offsetof(struct panel_cached, rows) + d->count * sizeof(struct panel_cache_row);
}

static bool strings_ok(const struct panel_cached *data)
{
    const struct panel_disk *d = &data->disk;
    if (!memchr(d->artist, 0, sizeof(d->artist)))
        return false;
    for (unsigned i = 0; i < d->disc_count; i++)
        if (data->discs[i].track <= 0 || data->discs[i].track >= (int)d->total ||
            (i && data->discs[i].track <= data->discs[i - 1].track))
            return false;
    for (unsigned i = 0; i < d->count; i++)
    {
        const struct panel_cache_row *r = &data->rows[i];
        if (!memchr(r->title, 0, sizeof(r->title)) ||
            !memchr(r->route, 0, sizeof(r->route)) ||
            !memchr(r->remote_id, 0, sizeof(r->remote_id)))
            return false;
    }
    return true;
}

static bool cache_read_file(const char *path)
{
    uint32_t start = USEC_TIMER;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;
    struct panel_disk *d = &scratch->data.disk;
    size_t head = sizeof(*d);
    bool valid = read(fd, d, head) == (ssize_t)head &&
                 d->magic == PANEL_MAGIC && d->key == work.key &&
                 d->revision == work.revision && d->fonts == fonts_revision &&
                 d->locale == work.locale && d->stamp == (work.remote ? 0 : db_stamp) &&
                 d->album_seek == work.album.seek && d->artist_seek == work.album.artist_seek &&
                 d->first == (unsigned)work.first && d->count <= PANEL_PAGE_ROWS &&
                 d->total <= 1000000 && d->first <= d->total &&
                 d->count == MIN(PANEL_PAGE_ROWS, d->total - d->first) &&
                 d->disc_count <= PANEL_DISC_MARKS &&
                 d->bytes >= slot_bytes(d) && d->bytes <= PANEL_BYTES &&
                 ffilesize(fd) == (off_t)d->bytes;
    /* One contiguous read for the rows and all three glyph packs. */
    if (valid)
        valid = read(fd, scratch->bytes + head, d->bytes - head) == (ssize_t)(d->bytes - head);
    close(fd);
    uint32_t expect = d->crc;
    d->crc = 0;
    if (!valid || crc_32(scratch->bytes, d->bytes, 0) != expect ||
        !strings_ok(&scratch->data))
        return false;
    unsigned next = slot_bytes(d);
    for (unsigned i = 0; i < PANEL_FACES; i++)
    {
        if (d->offset[i] != next || d->length[i] > d->bytes - next ||
            !font_pbf_pack_valid(fonts[i], scratch->bytes + next, d->length[i]))
            return false;
        next += d->length[i];
    }
    valid = next == d->bytes;
    if (valid)
        panel_perf_record(PP_DISK, USEC_TIMER - start, d->bytes, work.remote);
    return valid;
}

static void cache_write_file(const char *path, const union panel_buffer *buffer)
{
    char temporary[MAX_PATH];
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    mkdir(ROCKBOX_DIR "/podbox");
    mkdir(ROCKBOX_DIR "/podbox/cache");
    mkdir(PANEL_DIR);
    struct panel_disk d = buffer->data.disk;
    d.crc = 0;
    uint32_t crc = crc_32(&d, sizeof(d), 0);
    d.crc = crc_32(buffer->bytes + sizeof(d), d.bytes - sizeof(d), crc);
    int fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return;
    size_t body = d.bytes - sizeof(d);
    bool written = write(fd, &d, sizeof(d)) == (ssize_t)sizeof(d) &&
                   write(fd, buffer->bytes + sizeof(d), body) == (ssize_t)body;
    written = close(fd) == 0 && written;
    if (!written || cancelled() || rename(temporary, path) < 0)
        remove(temporary);
}

static int clean_title(char *title, unsigned duration, int order, bool legacy)
{
    if (!duration)
        return order;
    unsigned seconds = duration / 1000;
    char suffix[24];
    snprintf(suffix, sizeof(suffix), " - %02u:%02u", seconds / 60, seconds % 60);
    size_t length = strlen(title), tail = strlen(suffix);
    if (length <= tail || strcmp(title + length - tail, suffix))
        return order;

    const char *name = title;
    if (*title >= '0' && *title <= '9')
    {
        char *period, *end;
        long disc = strtol(title, &period, 10);
        if (*period == '.' && period[1] >= '0' && period[1] <= '9')
        {
            long track = strtol(period + 1, &end, 10);
            if (disc > 0 && disc <= 0x7fff && track > 0 && track <= 0xffff &&
                end - period >= 3 && *end == ' ' && end + 1 < title + length - tail &&
                (legacy || (disc == MAX(1, order >> 16) && track == (order & 0xffff))))
            {
                name = end + 1;
                order = ((int)disc << 16) | (int)track;
            }
        }
    }
    if (legacy || name != title)
    {
        title[length - tail] = 0;
        memmove(title, name, strlen(name) + 1);
    }
    return order;
}

static void clean_artist(char *artist, int id)
{
    if (!strstr(artist, " · "))
        return;
    char genre[CAROUSEL_TRACK_TEXT];
    if (!tagcache_entry_string(id, tag_genre, genre, sizeof(genre)) || !*genre)
        return;
    const size_t length = strlen(artist), tail = strlen(genre), separator = sizeof(" · ") - 1;
    if (length > tail + separator)
    {
        char *begin = artist + length - tail - separator;
        if (!strncmp(begin, " · ", separator) && !strcmp(begin + separator, genre))
            *begin = 0;
    }
}

static bool local_current(void)
{
    return tagcache_is_in_ram() && work.revision == (uint32_t)tagcache_commit_id() &&
           work.generation == tagcache_generation();
}

static bool query_local(struct panel_cached *data)
{
    if (!local_current() || !tagcache_search_ready())
        return false;
    uint32_t start = USEC_TIMER;
    int count;
    int handle = db_summary_album_panel_tracks(&work.album, &count, cancelled);
    panel_perf_record(PP_QUERY, USEC_TIMER - start, count, 0);
    if (handle < 0)
        return false;
    if (count < work.first || !local_current() || cancelled())
    {
        if (handle > 0)
            core_free(handle);
        return false;
    }
    struct panel_disk *d = &data->disk;
    d->total = count;
    d->count = MIN(count - work.first, PANEL_PAGE_ROWS);
    if (count == 0)
        return true;

    start = USEC_TIMER;
    struct db_panel_track *tracks = core_get_data_pinned(handle);
    clean_artist(d->artist, tracks[0].track.id);
    for (int i = 1; i < count && d->disc_count < PANEL_DISC_MARKS; i++)
    {
        unsigned disc = tracks[i].track.key >> 16;
        if (disc > (unsigned)MAX(1, tracks[i - 1].track.key >> 16))
            data->discs[d->disc_count++] = (struct panel_disc){i, disc};
    }
    bool ready = true;
    for (unsigned i = 0; i < d->count && ready; i++)
    {
        const struct db_panel_track *t = &tracks[work.first + i];
        struct panel_cache_row *r = &data->rows[i];
        r->id = t->track.id;
        r->order = t->track.key;
        r->playing_key = t->path_key;
        ready = local_current() && !cancelled();
        if (ready && !tagcache_entry_string(r->id, tag_title, r->title, sizeof(r->title)))
            strlcpy(r->title, UNTAGGED, sizeof(r->title));
        long duration;
        if (ready && tagcache_entry_numeric(r->id, tag_length, &duration) && duration > 0)
            r->duration = duration;
        if (ready)
            clean_title(r->title, r->duration, r->order, false);
    }
    core_put_data_pinned(tracks);
    core_free(handle);
    panel_perf_record(PP_QUERY, USEC_TIMER - start, d->count, 1);
    return ready;
}

static bool query_remote(struct panel_cached *data, bool *persistable)
{
#ifdef HAVE_USB_PODLINK
    if (cider_panel_revision() != work.revision)
        return false;
    if (remote_key != work.key || remote_first != work.first || !remote_at)
    {
        remote_key = work.key;
        remote_first = work.first;
        remote_at = USEC_TIMER;
    }
    char route[sizeof(work.route) + sizeof("?format=panel")];
    snprintf(route, sizeof(route), "%s?format=panel", work.route);
    cider_track_page_open(route);
    bool stale;
    int count = cider_track_page_total(&stale);
    /* The host can send a stale, empty result after a transient fetch error.
     * Reopen it on the next pass, and keep stale rows off disk. */
    if (!count && stale)
    {
        cider_track_page_open("");
        return false;
    }
    *persistable = !stale;
    if (count < work.first)
        return false;
    struct panel_disk *d = &data->disk;
    d->total = count;
    d->count = MIN(count - work.first, PANEL_PAGE_ROWS);
    for (unsigned i = 0; i < d->count; i++)
    {
        struct cider_row row;
        if (cancelled() || !cider_track_page_row(work.first + i, &row))
            return false;
        struct panel_cache_row *out = &data->rows[i];
        out->duration = row.duration;
        strlcpy(out->title, row.title, sizeof(out->title));
        strlcpy(out->route, row.route, sizeof(out->route));
        strlcpy(out->remote_id, row.id, sizeof(out->remote_id));
        /* Older providers put the disc/track prefix in the display label.
         * Plain rows without those fields retain the provider's track order. */
        unsigned order = row.count > UINT16_MAX ? row.count : 0;
        out->order = clean_title(out->title, out->duration, order, work.legacy_labels);
        if (!out->order)
            out->order = MIN(work.first + i + 1, UINT16_MAX);
    }
    panel_perf_record(PP_REMOTE, USEC_TIMER - remote_at, d->count, 0);
    remote_at = 0;
    return cider_panel_revision() == work.revision;
#else
    (void)data;
    (void)persistable;
    return false;
#endif
}

static bool gather_text(ucschar_t *codes, unsigned *count, const char *str)
{
    static ucschar_t utf[CAROUSEL_TRACK_TEXT], visual[CAROUSEL_TRACK_TEXT];
    const ucschar_t *p = bidi_l2v_buffer((const unsigned char *)str, 1, utf, visual, ARRAYLEN(utf));
    for (; *p; p++)
    {
        unsigned next = 0;
        while (next < *count && codes[next] < *p)
            next++;
        if (next < *count && codes[next] == *p)
            continue;
        if (*count == FONT_PACK_CODES)
            return false;
        memmove(codes + next + 1, codes + next, (*count - next) * sizeof(*codes));
        codes[next] = *p;
        ++*count;
    }
    return true;
}

static void build_packs(struct panel_cached *data)
{
    /* Only the header and the visible and next page need ink at the flip.
     * Later rows use the normal asynchronous PBF cache when scrolled to. */
    static ucschar_t codes[FONT_PACK_CODES];
    struct panel_disk *d = &data->disk;
    d->bytes = slot_bytes(d);
    char count_label[32];
    bool chinese = !strcmp(global_settings.lang_file, "chinese-simp");
    const char *format = chinese ? "%d 首" : (const char *)str(LANG_N_TRACKS_TITLE);
    snprintf(count_label, sizeof(count_label), format, (int)d->total);
    for (unsigned face = 0; face < PANEL_FACES; face++)
    {
        unsigned n = 0;
        gather_text(codes, &n, ".");
        if (face == PANEL_ALBUM)
            gather_text(codes, &n, work.title);
        else if (face == PANEL_BODY)
        {
            gather_text(codes, &n, d->artist);
            gather_text(codes, &n, " ·");
            gather_text(codes, &n, count_label);
            for (unsigned row = 0; row < d->count && row < PANEL_PACK_ROWS; row++)
                gather_text(codes, &n, data->rows[row].title);
        }
        else
        {
            gather_text(codes, &n, "0123456789: ");
            if (d->disc_count)
                gather_text(codes, &n, chinese ? "光盘" : (const char *)str(LANG_DISC));
        }

        uint32_t start = USEC_TIMER;
        d->offset[face] = d->bytes;
        d->length[face] = font_pbf_pack_build(fonts[face], faces[face], codes, n,
                                             scratch->bytes + d->bytes,
                                             PANEL_BYTES - d->bytes, cancelled);
        d->bytes += d->length[face];
        panel_perf_record(PP_FONT, USEC_TIMER - start, sizes[face],
                          d->length[face] ? n : 0);
    }
}

static bool work_current(void)
{
#ifdef HAVE_USB_PODLINK
    if (work.remote)
        return cider_panel_revision() == work.revision;
#endif
    return local_current();
}

static void close_remote_query(void)
{
#ifdef HAVE_USB_PODLINK
    if (remote_key)
        cider_track_page_open("");
#endif
    remote_key = 0;
    remote_at = 0;
}

static void prepare(const struct queue_event *event)
{
    (void)event;
    if (usb_host_wants_storage())
        return;
    unsigned index = 0;
    uint32_t start;
    mutex_lock(&lock);
    while (index < pending_count && lookup(&window[index], window[index].first) >= 0)
        index++;
    if (index == pending_count)
    {
        mutex_unlock(&lock);
        close_remote_query();
        return;
    }
    work = window[index];
    working_serial = serial;
    start = settled_at;
    retry_at = current_tick + MAX(1, HZ / 25);
    mutex_unlock(&lock);

    if ((!work.remote || work.key != remote_key || work.first != remote_first) && remote_key)
        close_remote_query();
    if (!work_current() || !fonts_ready())
        return;
    if (!work.remote && (!db_stamp || db_seen_generation != work.generation ||
                         db_seen_revision != work.revision))
    {
        db_stamp = tagcache_text_revision();
        db_seen_generation = work.generation;
        db_seen_revision = work.revision;
    }
    char path[MAX_PATH];
    cache_path(path);
    bool persistable = work.remote || db_stamp;
    bool disk = persistable && cache_read_file(path);
    const struct panel_disk *d = &scratch->data.disk;
    if (!disk)
    {
        memset(&scratch->data, 0, sizeof(scratch->data));
        scratch->data.disk = (struct panel_disk){
            .magic = PANEL_MAGIC, .key = work.key, .first = work.first,
            .revision = work.revision, .fonts = fonts_revision, .locale = work.locale,
            .stamp = work.remote ? 0 : db_stamp,
            .album_seek = work.album.seek, .artist_seek = work.album.artist_seek,
        };
        strlcpy(scratch->data.disk.artist, work.artist, sizeof(d->artist));
        if (!(work.remote ? query_remote(&scratch->data, &persistable) : query_local(&scratch->data)))
            return;
        build_packs(&scratch->data);
    }
    if (work.remote)
        close_remote_query();
    if (cancelled() || !work_current())
        return;

    /* Publish before persisting. The just-published slot cannot be swapped
     * out until this worker returns, so writes can use it without the lock. */
    int victim = -1;
    mutex_lock(&lock);
    if (serial == working_serial)
        for (unsigned i = 0; i < PANEL_SLOTS; i++)
            if (!slots[i].refs && (victim < 0 || slots[i].used < slots[victim].used))
                victim = i;
    union panel_buffer *ready = scratch;
    if (victim >= 0)
    {
        scratch = slots[victim].buffer;
        slots[victim] = (struct cached_slot){
            .buffer = ready, .request = work, .used = ++lru_clock,
        };
    }
    mutex_unlock(&lock);
    if (victim >= 0)
    {
        panel_perf_record(PP_READY, USEC_TIMER - start, index, disk);
        if (persistable && !disk && d->length[0] && d->length[1] && d->length[2] && !cancelled())
            cache_write_file(path, ready);
    }
}

static struct bg_task task = {.name = "cover panel", .handle_event = prepare};

void panel_cache_init(void)
{
    mutex_init(&lock);
    int handle = core_alloc_free_space((PANEL_SLOTS + 1) * sizeof(union panel_buffer));
    if (handle <= 0)
        return;
    union panel_buffer *buffers = core_get_data_pinned(handle);
    for (unsigned i = 0; i < PANEL_SLOTS; i++)
        slots[i].buffer = &buffers[i];
    scratch = &buffers[PANEL_SLOTS];
}


/* Playback supplies the live track. Drawing a row never opens a playlist
 * file to discover the playing marker. */
static void track_changed(unsigned short event, void *param)
{
    (void)event;
    const struct mp3entry *track = param ? ((const struct track_event *)param)->id3 : NULL;
    uint64_t key = track && *track->path ? path_key(track->path) : 0;
    int level = disable_irq_save();
    local_playing = key;
    restore_irq(level);
}

void panel_cache_watch_playback(void)
{
    add_event(PLAYBACK_EVENT_TRACK_CHANGE, track_changed);
    add_event(PLAYBACK_EVENT_CUR_TRACK_READY, track_changed);
}

uint64_t panel_cache_playing_key(void)
{
    int level = disable_irq_save();
    uint64_t key = local_playing;
    restore_irq(level);
    return key;
}
