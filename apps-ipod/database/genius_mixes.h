/* SPDX-License-Identifier: GPL-2.0-or-later
 * On-player Genius Mixes snapshots and database update hooks.
 */
#ifndef GENIUS_MIXES_H
#define GENIUS_MIXES_H

#include "genius_model.h"

struct genius_mix {
    char name[192];
    int32_t count;
    int32_t tracks[GENIUS_MAX_TRACKS];
    int32_t covers[GENIUS_COVERS];
};

struct genius_catalog {
    int32_t count;
    int32_t commit;
    uint32_t generation;
    uint32_t update;
    struct genius_mix mixes[GENIUS_MAX_MIXES];
};

/* Load a snapshot for the current database. An empty snapshot is valid. */
bool genius_mixes_load(struct genius_catalog *catalog);
bool genius_mixes_current(const struct genius_catalog *catalog);
bool genius_mixes_failed(void);
void genius_mixes_init(void);
void genius_mixes_database_changed(void);

#endif
