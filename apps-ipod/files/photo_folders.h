/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to photo_folders.c: the pictures copied to the player as files,
 * grouped by the folder that holds them.
 ****************************************************************************/
#ifndef _PHOTO_FOLDERS_H
#define _PHOTO_FOLDERS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PHOTO_FOLDER_LIMIT 2048

struct photo_file {
    uint32_t name;          /* offset of its file name in names */
    uint32_t size;
    uint32_t mtime;         /* local time, as the directory entry gives it */
    uint16_t folder;
    /* The rest is the thumbnail cache's (photo_thumbs.c) */
    uint8_t thumb_w;        /* the picture's extent inside its thumbnail */
    uint8_t thumb_h;
    int32_t thumb;          /* its record on disk */
    int16_t ram;            /* its slot in memory, or -1 */
    uint16_t pad;
};

struct photo_folder {
    uint32_t path;          /* offset of its path in names, "/" for the root */
    uint32_t label;         /* offset of the name shown for it */
    int first;              /* its files in files[], in name order */
    int count;
};

/* Albums are the folders in label order; files run album by album. The root
 * is no album: its pictures are in files[] ahead of the first album's. */
struct photo_folders {
    struct photo_folder *folders;
    int album_count;
    struct photo_file *files;
    int file_count;
    char *names;
};

/* Walks the disk into buf and returns the bytes kept from its start.
 * Returns zero if checkpoint() cancels the walk or buf is too small.
 * skip_full_resolution leaves out the folder iTunes copies a Photo
 * Database's originals to. */
size_t photo_folders_scan(struct photo_folders *pf, void *buf, size_t size,
                          bool skip_full_resolution,
                          bool (*checkpoint)(void));

/* A file's whole path, false when it does not fit */
bool photo_folders_path(const struct photo_folders *pf, int file, char *buf,
                        size_t size);

static inline const char *photo_folders_label(const struct photo_folders *pf,
                                              int album)
{
    return pf->names + pf->folders[album].label;
}

#endif /* _PHOTO_FOLDERS_H */
