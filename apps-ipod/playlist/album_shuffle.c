/* Album shuffle order and playlist control records. GPL version 2+. */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "file.h"
#include "strnatcmp.h"
#include "playlist/album_shuffle.h"

/* Paths on the player are case-insensitive. A separator byte distinguishes
 * an album/artist pair from a directory, and separates the two tag fields. */
static uint64_t hash_text(uint64_t hash, const char *text, size_t length)
{
    for (size_t i = 0; i < length; i++)
    {
        hash ^= tolower((unsigned char)text[i]);
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static uint64_t album_key(const char *path, const char *album,
                          const char *artist)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    if (album && album[0])
        hash = hash_text(hash, album, strlen(album) + 1);

    if (album && album[0] && artist && artist[0])
        return hash_text(hash, artist, strlen(artist) + 1);

    /* When an album has no album artist, its directory also groups
     * compilations whose individual tracks have different artists. */
    const char *slash = strrchr(path, '/');
    return hash_text(hash, path, slash ? (size_t)(slash - path) : 0);
}

static const char *basename_of(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static int compare_tracks(const void *a, const void *b)
{
    const struct album_shuffle_track *left = a, *right = b;
    if (left->album != right->album)
        return left->album < right->album ? -1 : 1;
    if (left->disc != right->disc)
        return left->disc < right->disc ? -1 : 1;
    if (left->track != right->track)
        return left->track < right->track ? -1 : 1;

    char left_path[MAX_PATH], right_path[MAX_PATH];
    const struct album_shuffle_source *source = left->source;
    source->filename(source->data, left->index, left_path, sizeof(left_path));
    source->filename(source->data, right->index, right_path, sizeof(right_path));
    int cmp = strnatcasecmp(basename_of(left_path), basename_of(right_path));
    if (cmp)
        return cmp;
    cmp = strcasecmp(left_path, right_path);
    return cmp ? cmp : left->index - right->index;
}

void album_shuffle_read(const struct album_shuffle_source *source, int count,
                        struct album_shuffle_track *tracks)
{
    for (int i = 0; i < count; i++)
    {
        char path[MAX_PATH];
        const char *album = NULL, *artist = NULL;
        int disc = 0, track = 0;
        source->filename(source->data, i, path, sizeof(path));
        source->tags(source->data, path, &album, &artist, &disc, &track);
        tracks[i] = (struct album_shuffle_track) {
            .album = album_key(path, album, artist),
            .source = source, .index = i,
            .disc = disc > 0 ? disc : 1,
            .track = track > 0 ? track : INT32_MAX,
        };
    }

    qsort(tracks, count, sizeof(*tracks), compare_tracks);
}

void album_shuffle_order(const struct album_shuffle_track *tracks,
                         int count, int keep, unsigned seed,
                         uint16_t *groups, uint16_t *order)
{
    int group_count = 0, keep_group = -1;
    for (int i = 0; i < count; i++)
    {
        if (i == 0 || tracks[i].album != tracks[i - 1].album)
            groups[group_count++] = i;
        if (tracks[i].index == keep)
            keep_group = groups[group_count - 1];
    }

    srand(seed);
    for (int i = group_count - 1; i > 0; i--)
    {
        int j = rand() % (i + 1);
        uint16_t tmp = groups[i];
        groups[i] = groups[j];
        groups[j] = tmp;
    }

    if (keep_group >= 0)
    {
        int i = 0;
        while (groups[i] != keep_group)
            i++;
        groups[i] = groups[0];
        groups[0] = keep_group;
    }

    int out = 0;
    for (int group = 0; group < group_count; group++)
    {
        int start = groups[group];
        if (start == keep_group)
            order[out++] = keep;
        while (start < count)
        {
            if (tracks[start].index != keep)
                order[out++] = tracks[start].index;
            if (start + 1 == count ||
                tracks[start].album != tracks[start + 1].album)
                break;
            start++;
        }
    }
}

void album_shuffle_reindex(struct album_shuffle_track *tracks, int count,
                           uint16_t *positions, const uint16_t *order)
{
    for (int i = 0; i < count; i++)
        positions[order[i]] = i;
    for (int i = 0; i < count; i++)
        tracks[i].index = positions[tracks[i].index];
}

/* One row fits comfortably in the ordinary playlist command buffer. */
#define ORDER_ROW_TRACKS 64
#define ORDER_SEEN 0x8000u

bool album_shuffle_save(int fd, const uint16_t *order, int count, unsigned seed)
{
    off_t offset = lseek(fd, 0, SEEK_END);
    if (offset < 0)
        return false;

    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < count;)
    {
        char row[2 + 4 * ORDER_ROW_TRACKS];
        int n = 0;
        row[n++] = '#';
        for (int end = i + ORDER_ROW_TRACKS; i < count && i < end; i++)
            for (int shift = 12; shift >= 0; shift -= 4)
                row[n++] = hex[(order[i] >> shift) & 0xf];
        row[n++] = '\n';
        if (write(fd, row, n) != n)
            goto failed;
    }

    /* Write the commit record with a single write as well. */
    char command[40];
    int length = snprintf(command, sizeof(command), "B:%u:%ld\n",
                          seed, (long)offset);
    if (write(fd, command, length) == length)
        return true;

failed:
    ftruncate(fd, offset);
    return false;
}

bool album_shuffle_load(int fd, off_t offset, off_t command,
                        uint16_t *order, int count)
{
    off_t saved = lseek(fd, 0, SEEK_CUR);
    const int rows = (count + ORDER_ROW_TRACKS - 1) / ORDER_ROW_TRACKS;
    if (saved < 0 || offset < 0 || offset > command ||
        command - offset != 4 * count + 2 * rows ||
        lseek(fd, offset, SEEK_SET) != offset)
        return false;

    bool valid = false;
    for (int i = 0; i < count;)
    {
        char row[2 + 4 * ORDER_ROW_TRACKS];
        int entries = count - i;
        if (entries > ORDER_ROW_TRACKS)
            entries = ORDER_ROW_TRACKS;
        int length = 2 + 4 * entries;
        if (read(fd, row, length) != length ||
            row[0] != '#' || row[length - 1] != '\n')
            goto out;

        const char *p = row + 1;
        for (int j = 0; j < entries; j++)
        {
            unsigned index = 0;
            for (int digit = 0; digit < 4; digit++, p++)
            {
                if (*p >= '0' && *p <= '9')
                    index = index * 16 + *p - '0';
                else if (*p >= 'a' && *p <= 'f')
                    index = index * 16 + *p - 'a' + 10;
                else
                    goto out;
            }
            if (index >= (unsigned)count)
                goto out;
            order[i++] = index;
        }
    }

    /* Check for duplicate positions before touching the live playlist. */
    for (int i = 0; i < count; i++)
    {
        unsigned index = order[i] & ~ORDER_SEEN;
        if (order[index] & ORDER_SEEN)
            goto out;
        order[index] |= ORDER_SEEN;
    }
    for (int i = 0; i < count; i++)
        order[i] &= ~ORDER_SEEN;
    valid = true;

out:
    return lseek(fd, saved, SEEK_SET) == saved && valid;
}
