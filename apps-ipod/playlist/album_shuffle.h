/* Album shuffle order and playlist control records. GPL version 2+. */
#ifndef ALBUM_SHUFFLE_H
#define ALBUM_SHUFFLE_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

struct album_shuffle_source {
    void *data;
    void (*filename)(void *data, int index, char *path, int size);
    void (*tags)(void *data, const char *path, const char **album,
                 const char **artist, int *disc, int *track);
};

struct album_shuffle_track {
    uint64_t album;
    const struct album_shuffle_source *source;
    int index;
    int disc;
    int track;
};

/* The track table has count entries. Reading it resolves the canonical
 * order once, so a repeat can shuffle the same groups without rescanning. */
void album_shuffle_read(const struct album_shuffle_source *source,
                        int count, struct album_shuffle_track *tracks);

/* `order` and `groups` each have count slots. Keep is the source index
 * to play first, or -1 for a fresh pass. */
void album_shuffle_order(const struct album_shuffle_track *tracks,
                         int count, int keep, unsigned seed,
                         uint16_t *groups, uint16_t *order);

/* Update a canonical table's source positions for the new live order.
 * `positions` is scratch space with count slots. */
void album_shuffle_reindex(struct album_shuffle_track *tracks, int count,
                           uint16_t *positions, const uint16_t *order);

/* Store source positions in control-file comments, followed by a commit
 * record. A torn append without that record leaves the old order in force. */
bool album_shuffle_save(int fd, const uint16_t *order, int count,
                        unsigned seed);
bool album_shuffle_load(int fd, off_t offset, off_t command,
                        uint16_t *order, int count);

#endif
