/* SPDX-License-Identifier: GPL-2.0-or-later
 * Build with apps-ipod/database/genius_model.c and -Iapps-ipod/database.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "genius_model.h"

static bool abort_now(void)
{
    return true;
}

static struct genius_track track(int id, uint32_t genre, int era,
                                 uint32_t artist, unsigned plays)
{
    return (struct genius_track){
        .id = id, .genre = genre, .era = era,
        .artist = artist, .credit = artist, .album = 1 + id / 2,
        .affinity = genius_affinity(plays, 0, 0, 0, 0, 180000),
    };
}

static void check_sequence(const struct genius_choice *mix,
                           const struct genius_track *original, int n)
{
    const struct genius_track *first = &original[mix->ids[0]];
    assert(mix->count >= GENIUS_MIN_TRACKS && mix->count <= GENIUS_MAX_TRACKS);
    assert(mix->artist_count >= 2 && mix->artist_count <= GENIUS_ARTISTS);
    unsigned char *seen = calloc(n, 1);
    assert(seen);
    for (int i = 0; i < mix->count; i++)
    {
        int id = mix->ids[i];
        assert(id >= 0 && id < n && !seen[id]++);
        const struct genius_track *t = &original[id];
        assert(t->genre == first->genre && t->era == first->era);
        if (i > 0)
        {
            const struct genius_track *last = &original[mix->ids[i - 1]];
            assert(last->artist != t->artist && last->credit != t->credit);
        }
    }
    for (int i = 0; i < GENIUS_COVERS; i++)
    {
        int id = mix->covers[i];
        if (id < 0)
            continue;
        assert(id < n && seen[id]);
        for (int j = 0; j < i; j++)
            assert(original[mix->covers[j]].album != original[id].album);
    }
    free(seen);
}

static void small_library(void)
{
    struct genius_track tracks[8], original[8];
    struct genius_choice mixes[GENIUS_MAX_MIXES] = {0};
    uint32_t genre = genius_genre("Indie Rock");
    for (int i = 0; i < 8; i++)
        original[i] = track(i, genre, genius_era(2008), i / 4 + 1u, i / 4);
    memcpy(tracks, original, sizeof(tracks));
    assert(genius_cluster(tracks, 0, mixes, NULL) == 0);
    assert(genius_cluster(tracks, 4, mixes, NULL) == 0);
    assert(genius_cluster(tracks, 5, mixes, NULL) == 0);
    assert(genius_cluster(tracks, 8, mixes, abort_now) == -1);
    assert(genius_cluster(tracks, 8, mixes, NULL) == 1);
    assert(mixes[0].count == 8);
    check_sequence(&mixes[0], original, 8);
    assert(original[mixes[0].artists[0]].credit == 2);
    for (int i = 0; i < 8; i++)
    {
        tracks[i] = original[i];
        tracks[i].era = 1 + i / 4;
    }
    assert(genius_cluster(tracks, 8, mixes, NULL) == 1);
    assert(mixes[0].count == 8);

    /* A sparse library with several genres can still support an era mix. */
    for (int i = 0; i < 8; i++)
    {
        tracks[i] = original[i];
        tracks[i].genre = i + 1;
    }
    assert(genius_cluster(tracks, 8, mixes, NULL) == 1);
    assert(mixes[0].count == 8);

    /* The same lead artist and artist aliases cannot alternate. */
    for (int i = 0; i < 8; i++)
    {
        tracks[i] = original[i];
        tracks[i].credit = 1;
        tracks[i].use_albumartist = true;
    }
    assert(genius_cluster(tracks, 8, mixes, NULL) == 0);
}

static void large_library(int n)
{
    struct genius_track *forward = malloc(n * sizeof(*forward));
    struct genius_track *reverse = malloc(n * sizeof(*reverse));
    struct genius_track *original = malloc(n * sizeof(*original));
    struct genius_choice a[GENIUS_MAX_MIXES] = {0};
    struct genius_choice b[GENIUS_MAX_MIXES] = {0};
    assert(forward && reverse && original);
    for (int i = 0; i < n; i++)
    {
        unsigned family = i % 16 < 8 ? 1 : 2;
        original[i] = track(i, family, 1 + (i / 16) % 8,
                            1 + i % 8, i % 31);
        forward[i] = reverse[n - 1 - i] = original[i];
    }
    clock_t start = clock();
    assert(genius_cluster(forward, n, a, NULL) == GENIUS_MAX_MIXES);
    double elapsed = (double)(clock() - start) / CLOCKS_PER_SEC;
    assert(genius_cluster(reverse, n, b, NULL) == GENIUS_MAX_MIXES);
    assert(memcmp(a, b, sizeof(a)) == 0);
    for (int i = 0; i < GENIUS_MAX_MIXES; i++)
        check_sequence(&a[i], original, n);
    printf("%d tracks: %d mixes, deterministic, %.3f s in host model\n",
           n, GENIUS_MAX_MIXES, elapsed);
    free(original);
    free(reverse);
    free(forward);
}

int main(void)
{
    assert(genius_genre("Hip-Hop") == genius_genre("rap"));
    assert(genius_genre("Alt. Country") == genius_genre("Bluegrass"));
    assert(genius_genre("Classic Rock") != genius_genre("Folk Metal"));
    assert(genius_genre("<Untagged>") == 0);
    assert(genius_genre("iTunesU") == GENIUS_NON_MUSIC);
    assert(genius_person("Various Artists") == 0);
    assert(genius_person("  CoLDPlay ") == genius_person("Coldplay"));
    assert(genius_era(1994) == genius_era(2004));
    assert(genius_era(1994) != genius_era(2005));
    assert(genius_affinity(6, 1, 60, 60, 0, 180000) >
           genius_affinity(2, 0, 1, 200, 4, 2000));
    assert(genius_affinity(0, 0, 0, 0, 6, 180000) <
           genius_affinity(0, 0, 0, 0, 0, 180000));
    small_library();
    large_library(2000);
    large_library(20000);
    return 0;
}
