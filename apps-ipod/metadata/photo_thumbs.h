/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to photo_thumbs.c: thumbnails and screen-size copies of the
 * pictures photo_folders.c finds, made in the background.
 ****************************************************************************/
#ifndef _PHOTO_THUMBS_H
#define _PHOTO_THUMBS_H

#include <stdbool.h>
#include <stddef.h>
#include "lcd.h"
#include "files/photo_folders.h"

#define PHOTO_THUMB_EDGE 64

enum photo_thumb_state {
    PHOTO_THUMB_READY,
    PHOTO_THUMB_PENDING,
    PHOTO_THUMB_BROKEN,     /* no reader decodes it */
};

/* The memory photo_thumbs_open() wants */
size_t photo_thumbs_bytes(void);

/* Loads the catalogue and cache index, then makes missing thumbnails.
 * The worker calls load(data) to fill pf. All storage must last until close. */
bool photo_thumbs_open(struct photo_folders *pf, void *buf, size_t size,
                       bool (*load)(void *), void *data);
bool photo_thumbs_ready(void);
/* From load(): yield and stop if Photos is closing or USB needs the disk. */
bool photo_thumbs_checkpoint(void);
void photo_thumbs_close(void);

/* The files of the album on screen, [lo, hi), and those of them in view,
 * [vis_lo, vis_hi). Those in view are made first, then their neighbours,
 * then the album, then every file. */
void photo_thumbs_focus(int lo, int hi, int vis_lo, int vis_hi);

/* A file's thumbnail: PHOTO_THUMB_EDGE square, the picture centred on black.
 * The pointer lasts until the next call. */
enum photo_thumb_state photo_thumbs_tile(int file, const fb_data **tile);

/* Moves whenever a thumbnail in view is made */
unsigned photo_thumbs_generation(void);

/* Whether a thumbnail in view is still to be made */
bool photo_thumbs_pending(void);

/* Fills pixels, a whole screen, with the file fitted to it: at once when the
 * copy is ready, else after asking for it and waiting up to `wait` ticks.
 * PENDING when it is not ready by then; photo_thumbs_screen_take() collects
 * it when it is. */
enum photo_thumb_state photo_thumbs_screen(int file, fb_data *pixels,
                                           long wait);
enum photo_thumb_state photo_thumbs_screen_take(int file, fb_data *pixels);

/* Readies a file's screen copy ahead of photo_thumbs_screen() */
void photo_thumbs_prefetch(int file);

/* Fills pixels with the file's thumbnail enlarged to fit the screen. False
 * when it has none. */
bool photo_thumbs_preview(int file, fb_data *pixels);

#endif /* _PHOTO_THUMBS_H */
