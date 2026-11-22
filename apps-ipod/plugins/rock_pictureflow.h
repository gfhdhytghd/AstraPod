/* Read-only library and art services for PictureFlow. GPL-2.0-or-later. */
#ifndef ASTRAPOD_ROCK_PICTUREFLOW_H
#define ASTRAPOD_ROCK_PICTUREFLOW_H

#include "database/tagcache.h"
#include "file.h"
#include "rbpaths.h"

#define ROCK_PF_CACHE_DIR ROCKBOX_DIR "/pictureflow"

struct bitmap;
struct dim;
struct db_summary_t;
struct album_data;
struct custom_format;
struct mp3entry;

enum { ROCK_PF_DB_BUSY = -5 };

struct rock_pf_track {
    char path[MAX_PATH];
    char title[TAGCACHE_BUFSZ];
    int disc;
    int track;
};

int rock_pf_db_open(struct db_summary_t *summary, void *buffer, size_t bytes);
bool rock_pf_db_current(const struct db_summary_t *summary);
char *rock_pf_db_name(const struct db_summary_t *summary, int tag, long seek,
                      char *buffer, size_t bytes);

/* A successful open must be paired with tagcache_search_finish(). */
bool rock_pf_tracks_open(const struct db_summary_t *summary,
                         const struct album_data *album,
                         struct tagcache_search *search);
bool rock_pf_track_next(struct tagcache_search *search,
                        struct rock_pf_track *track);
int rock_pf_art(const struct db_summary_t *summary,
                const struct album_data *album, struct bitmap *bitmap,
                int bytes, int format, const struct custom_format *output);
ssize_t rock_pf_cache_read(const char *name, struct dim *size,
                           void *pixels, size_t capacity);
void rock_pf_cache_write(const char *name, const struct dim *size,
                         const void *pixels, size_t bytes);
bool rock_pf_confirm_play(void);
bool rock_pf_track_info(struct mp3entry *id3);
bool rock_pf_catalog(const char *path, int attr, bool new_playlist,
                     int (*append)(const char *, bool));

#endif
