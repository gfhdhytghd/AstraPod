/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Read-only record access for the Classic iTunesDB and Photo Database.
 ****************************************************************************/
#ifndef STOCK_DB_H
#define STOCK_DB_H

#include <stdbool.h>
#include <stdint.h>
#include "file.h"

#define STOCK_PHOTOS "/Photos/Photo Database"
#define STOCK_VIDEO_PATH "/iPod_Control/Music/"

enum stock_video_kind {
    STOCK_MOVIE = 2,
    STOCK_MUSIC_VIDEO = 32,
    STOCK_TV_SHOW = 64,
    STOCK_VIDEO_PLAYLISTS = 128,
};

struct stock_db {
    int fd;
    off_t start;
    off_t end;
    bool temporary;
};

struct stock_record {
    off_t at;
    off_t body;
    off_t end;
    uint32_t count;
};

bool stock_artwork_open(struct stock_db *db);
bool stock_db_open(struct stock_db *db, bool photos);
/* The size of the iTunes library's file, 0 for none: it changes when
 * iTunes writes a different library */
off_t stock_db_size(void);
void stock_db_close(struct stock_db *db);
bool stock_db_list(const struct stock_db *db, unsigned section,
                   const char *tag, struct stock_record *list);
bool stock_record_read(const struct stock_db *db, off_t at, off_t end,
                       const char *tag, struct stock_record *record);
bool stock_db_u32(const struct stock_db *db, off_t at, uint32_t *value);
bool stock_db_photo_text(const struct stock_db *db, const struct stock_record *r,
                         char *buf, size_t size);
bool stock_db_music_text(const struct stock_db *db, const struct stock_record *r,
                         char *buf, size_t size);
bool stock_music_path(char *colon_path);
bool stock_video_path(char *colon_path);
bool stock_photo_pixels(const struct stock_db *db, off_t at, unsigned width,
                        unsigned height, void *pixels);
int stock_artwork_pixels(const struct stock_db *db, off_t at, void *pixels);
/* Image `at`'s largest thumbnail as an `edge`-pixel square in `pixels`:
 * the middle of the 320-pixel one, or the 128-pixel one widened. False
 * when it has neither. */
bool stock_artwork_large(const struct stock_db *db, off_t at, void *pixels,
                         int edge);
#endif
