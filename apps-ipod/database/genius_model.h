/* SPDX-License-Identifier: GPL-2.0-or-later
 * Metadata-only Genius Mixes clustering and sequencing.
 */
#ifndef GENIUS_MODEL_H
#define GENIUS_MODEL_H

#include <stdbool.h>
#include <stdint.h>

#define GENIUS_MAX_MIXES 12
#define GENIUS_MAX_TRACKS 120
#define GENIUS_MIN_TRACKS 6
#define GENIUS_COVERS 4
#define GENIUS_ARTISTS 4
#define GENIUS_NON_MUSIC UINT32_MAX

struct genius_track {
    int32_t id;
    uint32_t genre, artist, credit, album;
    uint16_t affinity;
    uint8_t era;
    bool use_albumartist;
};

struct genius_choice {
    int32_t ids[GENIUS_MAX_TRACKS];
    int32_t covers[GENIUS_COVERS];
    int32_t artists[GENIUS_ARTISTS];
    unsigned albumartist_mask;
    int count, artist_count;
};

uint32_t genius_genre(const char *name);
uint32_t genius_person(const char *name);
unsigned genius_era(long year);
unsigned genius_affinity(long plays, long rating, long last, long serial,
                          long skips, long length);
/* The input is reordered and used as scratch. Return -1 when interrupted. */
int genius_cluster(struct genius_track *tracks, int count,
                   struct genius_choice *mixes, bool (*stop)(void));

#endif
