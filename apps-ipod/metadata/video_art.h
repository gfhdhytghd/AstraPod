/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to video_art.c: the cover thumbnails of the videos in the
 * video index.
 ****************************************************************************/
#ifndef _VIDEO_ART_H
#define _VIDEO_ART_H

#include <stdbool.h>
#include <stdint.h>
#include "lcd.h"                        /* fb_data */
#include "system/bg_task.h"

/* The edge of the square a thumbnail is fitted inside */
#define VIDEO_ART_DIM 55

/* Register the task. Call once at startup, before bg_task_start(). */
void video_art_init(void);

/* Changes whenever a thumbnail is written, for a list showing the
 * placeholder to know a cover may now be there */
unsigned video_art_generation(void);

/* The thumbnail named by art_key (struct video_entry's), centred in a
 * dim x dim square of `pixels` over `background`. False while it has not
 * been made, or when the cover could not be read. */
bool video_art_load(uint64_t art_key, int dim, fb_data *pixels,
                    fb_data background);

/* The pass: a walk that changed the video index sends it an update, and
 * it makes whichever thumbnails are missing and removes the ones no video
 * names any more. */
extern struct bg_task video_art_task;

#endif /* _VIDEO_ART_H */
