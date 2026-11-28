/* SPDX-License-Identifier: GPL-2.0-or-later
 * Derive and cache Genius Mixes on the shared low-priority library worker.
 */
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "core_alloc.h"
#include "file.h"
#include "rbpaths.h"
#include "strlcat.h"
#include "database/genius_mixes.h"
#include "database/path_key.h"
#include "database/tagcache.h"
#include "system/bg_task.h"
#include "viewers/playback_viewer/pv_log.h"

#define MIX_FILE ROCKBOX_DIR "/genius-mixes.dat"
#define MIX_TEMP MIX_FILE ".tmp"
#define MIX_VERSION 1
#define ENTRY_LIMIT 24576
#define MEMO_SIZE 512
#define SKIP_SLOTS 4096
#define SKIP_LOG_BYTES (2 * 1024 * 1024)

struct genius_header {
    uint32_t version;
    uint32_t revision;
    struct bg_marks marks;
    int32_t count;
};

struct tag_memo {
    int32_t seek;
    uint32_t value;
};

struct skip_slot {
    uint64_t key;
    unsigned int count;
};

struct work {
    struct tag_memo memo[3][MEMO_SIZE];
    struct skip_slot skip[SKIP_SLOTS];
    unsigned char entry_skips[ENTRY_LIMIT];
    char logbuf[8192];
    bool has_skips;
    struct genius_choice choices[GENIUS_MAX_MIXES];
    struct genius_mix output;
    struct genius_track tracks[];
};

static struct bg_task genius_task;
static volatile uint32_t database_update;
static volatile uint32_t published_update;

static void read_marks(struct bg_marks *marks)
{
    struct genius_header header;
    *marks = (struct bg_marks){-1, -1, -1};
    int fd = open(MIX_FILE, O_RDONLY);
    if (fd >= 0)
    {
        if (read(fd, &header, sizeof(header)) == sizeof(header) &&
            header.version == MIX_VERSION)
            *marks = header.marks;
        close(fd);
    }
}

static void write_marks(const struct bg_marks *marks)
{
    /* The successful pass publishes marks and tracks in the same rename. */
    if (!marks)
        remove(MIX_FILE);
}

static bool artifact_ok(void)
{
    struct genius_header header;
    int fd = open(MIX_FILE, O_RDONLY);
    if (fd < 0)
        return false;
    bool ok = read(fd, &header, sizeof(header)) == sizeof(header) &&
              header.version == MIX_VERSION &&
              header.count >= 0 && header.count <= GENIUS_MAX_MIXES &&
              ffilesize(fd) == (off_t)(sizeof(header) +
                           header.count * sizeof(struct genius_mix)) &&
              header.revision == tagcache_text_revision();
    close(fd);
    if (ok)
        published_update = database_update;
    return ok;
}

static bool should_stop(void)
{
    yield();
    return genius_task.update_req || tagcache_is_busy() ||
           bg_task_should_stop(&genius_task);
}

static struct skip_slot *skip_slot(struct work *w, uint64_t key)
{
    unsigned first = (key ^ (key >> 32)) % SKIP_SLOTS;
    for (unsigned probe = 0; probe < 15; probe++)
    {
        struct skip_slot *slot = &w->skip[(first + probe) % SKIP_SLOTS];
        if (!slot->count || slot->key == key)
            return slot;
    }
    /* Keep room for newer history even when this neighbourhood fills. */
    return &w->skip[(first + 15) % SKIP_SLOTS];
}

static void record_skip(const struct pv_entry *entry, void *data)
{
    if (!entry->skipped || !entry->path[0])
        return;
    uint64_t key = path_key(entry->path);
    struct work *w = data;
    w->has_skips = true;
    struct skip_slot *slot = skip_slot(w, key);
    if (slot->key != key)
        *slot = (struct skip_slot){key, 0};
    if (slot->count < 30)
        slot->count++;
}

static unsigned find_skips(struct work *w, uint64_t key)
{
    const struct skip_slot *slot = skip_slot(w, key);
    return slot->key == key ? slot->count : 0;
}

static bool memo_tag(struct work *w, struct tagcache_search *tcs,
                     int slot, int tag, int32_t seek, uint32_t *value)
{
    struct tag_memo *memo = &w->memo[slot][(uint32_t)seek % MEMO_SIZE];
    if (memo->seek != seek || !memo->seek)
    {
        char text[TAGCACHE_BUFSZ];
        if (!tagcache_retrieve(tcs, tcs->idx_id, tag, text, sizeof(text)))
            return false;
        memo->seek = seek;
        memo->value = tag == tag_genre ? genius_genre(text)
                                        : genius_person(text);
    }
    *value = memo->value;
    return true;
}

static int scan_tracks(struct work *w, struct tagcache_search *tcs,
                       int entries, int capacity, long serial)
{
    static const int tags[] = {tag_genre, tag_artist, tag_albumartist,
                               tag_album, tag_year, tag_playcount, tag_rating,
                               tag_lastplayed, tag_length, tag_virt_spoken};
    enum {COL_GENRE, COL_ARTIST, COL_ALBUMARTIST, COL_ALBUM, COL_YEAR,
          COL_PLAYS, COL_RATING, COL_LAST, COL_LENGTH, COL_SPOKEN};
    int slots = tagcache_path_slots();
    bool indexed = w->has_skips && slots > 0 && entries <= ENTRY_LIMIT;
    for (int i = 0; indexed && i < slots; i++)
    {
        uint64_t key;
        int id;
        if (!(i % 64) && should_stop())
            return -1;
        if (tagcache_path_slot(i, &key, &id) && id >= 0 && id < entries)
            w->entry_skips[id] = find_skips(w, key);
    }

    char numeric[24];
    int count = 0;
    unsigned scanned = 0, seen = 0;
    uint32_t rng = 0x6d2b79f5;
    while (tagcache_get_next(tcs, numeric, sizeof(numeric)))
    {
        if (!(scanned++ % 32) && should_stop())
            return -1;
        long v[ARRAYLEN(tags)];
        if (!tagcache_get_values(tcs, tags, v, ARRAYLEN(tags)))
            return -1;
        if (v[COL_SPOKEN] || v[COL_LENGTH] <= 0)
            continue;
        struct genius_track track = {.id = tcs->idx_id};
        if (!memo_tag(w, tcs, 0, tag_genre, v[COL_GENRE], &track.genre) ||
            !memo_tag(w, tcs, 1, tag_artist, v[COL_ARTIST], &track.artist) ||
            !memo_tag(w, tcs, 2, tag_albumartist, v[COL_ALBUMARTIST],
                      &track.credit))
            return -1;
        track.use_albumartist = track.credit != 0;
        if (!track.credit)
            track.credit = track.artist;
        if (!track.artist)
            track.artist = track.credit;
        if (!track.artist || track.genre == GENIUS_NON_MUSIC)
            continue;

        int pos = count;
        seen++;
        if (count >= capacity)
        {
            /* A deterministic reservoir keeps every part of large libraries
             * eligible without reserving more than the worker's budget. */
            rng = rng * 1664525u + 1013904223u;
            pos = rng % seen;
            if (pos >= capacity)
                continue;
        }
        else
            count++;

        unsigned skips = 0;
        if (indexed && tcs->idx_id >= 0 && tcs->idx_id < entries)
            skips = w->entry_skips[tcs->idx_id];
        else if (w->has_skips)
        {
            char path[MAX_PATH + 1];
            if (tagcache_retrieve(tcs, tcs->idx_id, tag_filename,
                                  path, sizeof(path)))
                skips = find_skips(w, path_key(path));
        }
        /* A tag seek is unique per string. Pair album title and album artist
         * so two releases called Greatest Hits can both supply covers. */
        track.album = (uint32_t)v[COL_ALBUM] * 16777619u ^
                      (track.use_albumartist ? track.credit : 0);
        track.era = genius_era(v[COL_YEAR]);
        track.affinity = genius_affinity(v[COL_PLAYS], v[COL_RATING],
                                         v[COL_LAST], serial, skips,
                                         v[COL_LENGTH]);
        w->tracks[pos] = track;
    }
    return tcs->failed ? -1 : count;
}

static bool make_mix(struct genius_mix *out, const struct genius_choice *choice,
                     struct tagcache_search *tcs)
{
    memset(out, 0, sizeof(*out));
    out->count = choice->count;
    memcpy(out->tracks, choice->ids, choice->count * sizeof(out->tracks[0]));
    memcpy(out->covers, choice->covers, sizeof(out->covers));

    for (int i = 0; i < choice->artist_count; i++)
    {
        char artist[TAGCACHE_BUFSZ];
        int tag = choice->albumartist_mask & (1u << i) ? tag_albumartist
                                                       : tag_artist;
        if (!tagcache_retrieve(tcs, choice->artists[i], tag,
                               artist, sizeof(artist)))
            return false;
        /* Give each credit space, without cutting a UTF-8 character. */
        size_t limit = (sizeof(out->name) - 16) / choice->artist_count;
        if (strlen(artist) > limit)
        {
            while (((unsigned char)artist[limit] & 0xc0) == 0x80)
                limit--;
            artist[limit] = '\0';
        }
        if (i)
            strlcat(out->name, i == choice->artist_count - 1 ? " and " : ", ",
                    sizeof(out->name));
        strlcat(out->name, artist, sizeof(out->name));
    }
    return true;
}

static enum bg_result generate(void)
{
    struct tagcache_search tcs;
    struct tagcache_marks marks;
    struct genius_header header = {
        .version = MIX_VERSION, .marks = genius_task.prev_marks,
    };
    int capacity = MIN(MAX(0, header.marks.entries), ENTRY_LIMIT);
    uint32_t update = database_update;
    uint32_t generation = tagcache_generation();
    int handle = core_alloc_free_space(sizeof(struct work) +
                           capacity * sizeof(struct genius_track));
    if (handle <= 0)
        return BG_INTERRUPTED;

    struct work *w = core_get_data_pinned(handle);
    memset(w, 0, sizeof(*w));
    enum bg_result result = BG_INTERRUPTED;
    int fd = -1;

    pv_log_recent_playback(w->logbuf, sizeof(w->logbuf), SKIP_LOG_BYTES,
                           record_skip, w, should_stop);
    if (should_stop() || !tagcache_search(&tcs, tag_length))
        goto end;

    tagcache_get_marks(&marks);
    if (marks.commitid != header.marks.commitid ||
        marks.generation != generation ||
        tagcache_get_stat()->total_entries != header.marks.entries)
        goto finish;
    header.revision = tagcache_text_revision();
    int count = scan_tracks(w, &tcs, header.marks.entries, capacity,
                            marks.serial);
    if (count < 0)
        goto finish;
    count = genius_cluster(w->tracks, count, w->choices, should_stop);
    if (count < 0)
        goto finish;
    header.count = count;

    fd = open(MIX_TEMP, O_CREAT | O_WRONLY | O_TRUNC, 0666);
    if (fd < 0 || write(fd, &header, sizeof(header)) != sizeof(header))
    {
        result = BG_FAILED;
        goto finish;
    }
    for (int i = 0; i < count; i++)
    {
        if (should_stop() || !make_mix(&w->output, &w->choices[i], &tcs))
            goto finish;
        if (write(fd, &w->output, sizeof(w->output)) != sizeof(w->output))
        {
            result = BG_FAILED;
            goto finish;
        }
    }
    if (should_stop() || update != database_update ||
        generation != tagcache_generation() ||
        header.marks.commitid != tagcache_commit_id())
        goto finish;

    int synced = fsync(fd);
    int closed = close(fd);
    fd = -1;
    if (synced == 0 && closed == 0 && rename(MIX_TEMP, MIX_FILE) == 0)
    {
        published_update = update;
        result = BG_DONE;
    }
    else
        result = BG_FAILED;

finish:
    tagcache_search_finish(&tcs);
end:
    if (fd >= 0)
        close(fd);
    if (result != BG_DONE)
        remove(MIX_TEMP);
    core_put_data_pinned(w);
    core_free(handle);
    return result;
}

static struct bg_task genius_task = {
    .name = "Genius Mixes",
    .rank = BG_RANK_GENIUS,
    .read_marks = read_marks,
    .write_marks = write_marks,
    .artifact_ok = artifact_ok,
    .run = generate,
    .work_bytes = sizeof(struct work) +
                  ENTRY_LIMIT * sizeof(struct genius_track),
};

void genius_mixes_init(void)
{
    /* The cached entry ids are accepted after the worker checks its stamp. */
    published_update = UINT32_MAX;
    bg_task_init(&genius_task);
}

void genius_mixes_database_changed(void)
{
    database_update++;
    bg_task_update(&genius_task);
}

bool genius_mixes_current(const struct genius_catalog *catalog)
{
    return tagcache_is_usable() &&
           catalog->commit == tagcache_commit_id() &&
           catalog->generation == tagcache_generation() &&
           catalog->update == database_update;
}

bool genius_mixes_failed(void)
{
    return genius_task.failed;
}

bool genius_mixes_load(struct genius_catalog *catalog)
{
    uint32_t update = database_update;
    uint32_t generation = tagcache_generation();
    if (published_update != update || !tagcache_is_usable())
        return false;
    struct genius_header header;
    int fd = open(MIX_FILE, O_RDONLY);
    if (fd < 0)
        return false;
    bool ok = read(fd, &header, sizeof(header)) == sizeof(header) &&
              header.version == MIX_VERSION &&
              header.marks.commitid == tagcache_commit_id() &&
              header.marks.entries == tagcache_get_stat()->total_entries &&
              header.count >= 0 && header.count <= GENIUS_MAX_MIXES;
    if (ok)
    {
        size_t bytes = header.count * sizeof(catalog->mixes[0]);
        ok = read(fd, catalog->mixes, bytes) == (ssize_t)bytes;
    }
    close(fd);
    if (!ok)
        return false;

    for (int i = 0; i < header.count; i++)
    {
        struct genius_mix *mix = &catalog->mixes[i];
        if (mix->count < GENIUS_MIN_TRACKS || mix->count > GENIUS_MAX_TRACKS ||
            !memchr(mix->name, 0, sizeof(mix->name)))
            return false;
        for (int j = 0; j < mix->count; j++)
            if (mix->tracks[j] < 0 || mix->tracks[j] >= header.marks.entries)
                return false;
        for (int j = 0; j < GENIUS_COVERS; j++)
            if (mix->covers[j] < -1 || mix->covers[j] >= header.marks.entries)
                return false;
    }
    catalog->count = header.count;
    catalog->commit = header.marks.commitid;
    catalog->generation = generation;
    catalog->update = update;
    return genius_mixes_current(catalog);
}
