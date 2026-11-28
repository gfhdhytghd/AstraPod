/* SPDX-License-Identifier: GPL-2.0-or-later
 * Cluster track metadata and sequence mixes without database or UI state.
 */
#include <stdlib.h>
#include <string.h>
#include "genius_model.h"

static uint32_t normalized_hash(const char *name)
{
    uint32_t hash = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++)
    {
        unsigned c = *p;
        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c >= 128)
            hash = (hash ^ c) * 16777619u;
    }
    return hash == 2166136261u ? 0 : hash;
}

uint32_t genius_person(const char *name)
{
    uint32_t hash = normalized_hash(name);
    if (hash == normalized_hash("Untagged") ||
        hash == normalized_hash("Unknown Artist") ||
        hash == normalized_hash("Various Artists") ||
        hash == normalized_hash("Various") || hash == normalized_hash("VA"))
        return 0;
    return hash;
}

uint32_t genius_genre(const char *name)
{
    /* Exact normalized aliases keep such genres as Folk Rock and Folk Metal
     * in their own musical families. Unlisted genres keep their own keys. */
    static const char *const families[][10] = {
        {"Rock", "Classic Rock", "Alternative Rock", "Indie Rock",
         "Alternative", "Hard Rock", "Progressive Rock", "Britpop"},
        {"Metal", "Heavy Metal", "Death Metal", "Black Metal", "Folk Metal",
         "Thrash Metal", "Power Metal", "Progressive Metal", "Nu Metal"},
        {"Hip Hop", "Rap", "Hip Hop Rap", "Trap", "Boom Bap"},
        {"Electronic", "Electronica", "Dance", "EDM", "House", "Techno",
         "Trance", "Drum and Bass", "Dubstep"},
        {"Jazz", "Bebop", "Swing", "Smooth Jazz", "Fusion", "Vocal Jazz"},
        {"R&B", "Rhythm and Blues", "Soul", "Neo Soul", "Funk", "Motown"},
        {"Classical", "Baroque", "Chamber Music", "Opera", "Orchestral"},
        {"Folk", "Folk Rock", "Singer Songwriter", "Traditional Folk"},
        {"Pop", "Dance Pop", "Synth Pop", "Indie Pop", "Electropop"},
        {"Country", "Americana", "Bluegrass", "Alt Country"},
    };
    uint32_t hash = normalized_hash(name);
    if (hash == normalized_hash("iTunes U"))
        return GENIUS_NON_MUSIC;
    if (hash == normalized_hash("Untagged") ||
        hash == normalized_hash("Unknown Genre"))
        return 0;
    for (unsigned i = 0; i < sizeof(families) / sizeof(families[0]); i++)
        for (unsigned j = 0; j < 10 && families[i][j]; j++)
            if (hash == normalized_hash(families[i][j]))
                return i + 1;
    return hash;
}

unsigned genius_era(long year)
{
    if (year < 1900 || year > 2199)
        return 0;
    return (year - 1900) / 15 + 1;
}

static unsigned bounded(long value, unsigned limit)
{
    if (value < 0)
        return 0;
    return (unsigned long)value > limit ? limit : (unsigned)value;
}

unsigned genius_affinity(long plays, long rating, long last, long serial,
                          long skips, long length)
{
    unsigned score = 32 + 4 * bounded(plays, 40) + 8 * bounded(rating, 10);
    if (last > 0 && last <= serial)
        score += 16 - bounded((serial - last) / 8, 16);
    if (length >= 90000 && length <= 600000)
        score += 12;
    unsigned penalty = 5 * bounded(skips, 30);
    return score > penalty ? score - penalty : 1;
}

static int compare_tracks(const void *a, const void *b)
{
    const struct genius_track *x = a, *y = b;
    if (x->genre != y->genre)
        return x->genre < y->genre ? -1 : 1;
    if (x->era != y->era)
        return (int)x->era - y->era;
    if (x->credit != y->credit)
        return x->credit < y->credit ? -1 : 1;
    return x->id < y->id ? -1 : x->id > y->id;
}

struct cluster {
    struct genius_track *tracks;
    int count;
    uint32_t score;
};

static void offer(struct cluster *best, struct genius_track *tracks, int count)
{
    uint32_t score = 0;
    for (int i = 0; i < count; i++)
        score += tracks[i].affinity;

    for (int i = 0; i < GENIUS_MAX_MIXES; i++)
    {
        if (score <= best[i].score)
            continue;
        memmove(&best[i + 1], &best[i],
                (GENIUS_MAX_MIXES - i - 1) * sizeof(*best));
        best[i] = (struct cluster){tracks, count, score};
        break;
    }
}

/* Both tags can identify the performer: "A feat. B" can share credit with A,
 * and a track by A can also appear under another album artist. */
static bool same_artist(const struct genius_track *a,
                        const struct genius_track *b)
{
    return a->artist == b->artist || a->credit == b->credit ||
           a->artist == b->credit || a->credit == b->artist;
}

static bool diverse(const struct genius_track *tracks, int count)
{
    if (count < GENIUS_MIN_TRACKS)
        return false;
    /* The third track away from a dominant performer permits at least six
     * interleaved tracks. The sequencing pass checks both performer tags. */
    int others = 0;
    for (int i = 1; i < count; i++)
        if (!same_artist(tracks, &tracks[i]) && ++others == 3)
            return true;
    return false;
}

static uint32_t scramble(uint32_t value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    return value ^ (value >> 16);
}

static uint32_t artist_score(const struct genius_track *tracks, int count,
                             uint32_t artist)
{
    uint32_t score = 0;
    for (int i = 0; i < count; i++)
        if (tracks[i].credit == artist)
            score += tracks[i].affinity;
    return score;
}

static bool sequence(struct cluster *group, struct genius_choice *mix,
                     bool (*stop)(void))
{
    struct genius_track *tracks = group->tracks;
    uint32_t seed = scramble(tracks[0].genre ^ tracks[0].era ^ group->count);
    int n = 0;
    while (n < group->count && n < GENIUS_MAX_TRACKS)
    {
        if (stop && stop())
            return false;
        int pick = -1, best_spacing = -1;
        uint32_t best_key = UINT32_MAX;
        for (int i = n; i < group->count; i++)
        {
            const struct genius_track *t = &tracks[i];
            if (n && same_artist(t, &tracks[n - 1]))
                continue;
            int spacing = 0;
            if (n < 2 || !same_artist(t, &tracks[n - 2]))
                spacing += 1;
            if (!n || !t->album || t->album != tracks[n - 1].album)
                spacing += 2;
            uint32_t key = scramble(seed ^ (uint32_t)t->id) /
                           (32 + t->affinity);
            if (spacing > best_spacing ||
                (spacing == best_spacing && key < best_key))
            {
                best_spacing = spacing;
                best_key = key;
                pick = i;
            }
        }
        if (pick < 0)
            break;
        struct genius_track chosen = tracks[pick];
        tracks[pick] = tracks[n];
        tracks[n] = chosen;
        n++;
    }

    /* Greedy choice can leave one artist at the tail. Fit those tracks into
     * earlier gaps when both neighbours permit it. */
    for (int i = n; i < group->count && n < GENIUS_MAX_TRACKS; i++)
    {
        if (stop && !(i % 32) && stop())
            return false;
        struct genius_track chosen = tracks[i];
        int position = -1, best_spacing = -1;
        for (int j = 0; j < n; j++)
        {
            if (same_artist(&chosen, &tracks[j]) ||
                (j && same_artist(&chosen, &tracks[j - 1])))
                continue;
            int spacing = (chosen.album != tracks[j].album) +
                          (!j || chosen.album != tracks[j - 1].album);
            if (spacing > best_spacing)
            {
                position = j;
                best_spacing = spacing;
            }
            if (spacing == 2)
                break;
        }
        if (position < 0)
            continue;
        tracks[i] = tracks[n];
        memmove(&tracks[position + 1], &tracks[position],
                (n - position) * sizeof(*tracks));
        tracks[position] = chosen;
        n++;
    }

    mix->count = n;
    for (int i = 0; i < n; i++)
        mix->ids[i] = tracks[i].id;
    unsigned cover_count = 0;
    uint32_t cover_albums[GENIUS_COVERS];
    for (int i = 0; i < GENIUS_COVERS; i++)
        mix->covers[i] = -1;
    for (int i = 0; i < n && cover_count < GENIUS_COVERS; i++)
    {
        unsigned j = 0;
        while (j < cover_count && cover_albums[j] != tracks[i].album)
            j++;
        if (j == cover_count && tracks[i].album)
        {
            cover_albums[cover_count] = tracks[i].album;
            mix->covers[cover_count++] = tracks[i].id;
        }
    }

    uint32_t credited[GENIUS_ARTISTS] = {0};
    int names = n >= 48 ? 4 : n >= 24 ? 3 : 2;
    mix->artist_count = 0;
    mix->albumartist_mask = 0;
    for (int name = 0; name < names; name++)
    {
        if (stop && stop())
            return false;
        int pick = -1;
        uint32_t best_score = 0;
        for (int i = 0; i < n; i++)
        {
            uint32_t artist = tracks[i].credit;
            int j = 0;
            while (j < name && credited[j] != artist)
                j++;
            if (j < name)
                continue;
            /* Rank the credits that actually made the mix. The albumartist
             * credit keeps featured tracks with the artist who led them. */
            uint32_t score = artist_score(tracks, n, artist);
            if (score > best_score)
            {
                best_score = score;
                pick = i;
            }
        }
        if (pick < 0)
            break;
        credited[name] = tracks[pick].credit;
        mix->artists[name] = tracks[pick].id;
        mix->albumartist_mask |= (unsigned)tracks[pick].use_albumartist << name;
        mix->artist_count++;
    }
    return true;
}

static bool rank_clusters(struct genius_track *tracks, int count,
                          struct cluster *best, bool (*stop)(void))
{
    qsort(tracks, count, sizeof(*tracks), compare_tracks);
    for (int start = 0; start < count;)
    {
        if (stop && stop())
            return false;
        int end = start + 1;
        while (end < count && tracks[end].genre == tracks[start].genre)
            end++;

        bool eras = false;
        for (int first = start; first < end;)
        {
            int last = first + 1;
            while (last < end && tracks[last].era == tracks[first].era)
                last++;
            if (diverse(&tracks[first], last - first))
            {
                offer(best, &tracks[first], last - first);
                eras = true;
            }
            first = last;
        }
        if (!eras && diverse(&tracks[start], end - start))
            offer(best, &tracks[start], end - start);
        start = end;
    }

    return true;
}

int genius_cluster(struct genius_track *tracks, int count,
                   struct genius_choice *mixes, bool (*stop)(void))
{
    struct cluster best[GENIUS_MAX_MIXES] = {0};
    if (!rank_clusters(tracks, count, best, stop))
        return -1;
    if (count >= GENIUS_MIN_TRACKS && !best[0].count)
    {
        /* A sparse collection may support an era mix before it supports one
         * for a specific genre. The same spacing rules apply. */
        for (int i = 0; i < count; i++)
            tracks[i].genre = 0;
        if (!rank_clusters(tracks, count, best, stop))
            return -1;
    }

    int mixes_count = 0;
    for (int i = 0; i < GENIUS_MAX_MIXES && best[i].count; i++)
    {
        struct genius_choice *mix = &mixes[mixes_count];
        if (!sequence(&best[i], mix, stop))
            return -1;
        if (mix->count >= GENIUS_MIN_TRACKS && mix->artist_count >= 2)
            mixes_count++;
    }
    return mixes_count;
}
