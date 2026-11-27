/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to file_index.c.
 ****************************************************************************/
#ifndef _FILE_INDEX_H
#define _FILE_INDEX_H

#include <stdbool.h>
#include "system/bg_task.h"

/* The lists the walk writes */
enum flat_kind {
    FLAT_DOCUMENTS = 0,
    FLAT_IMAGES,
    FLAT_VIDEOS,        /* empty without a video player: see filetypes.h */
    FLAT_KINDS
};

/* Register the task that keeps the document, image and video lists current.
 * Call once at startup, before bg_task_start(). */
void file_index_init(void);

/* Path of the list file for one class, for path_list_load(). Absent until a
 * walk has completed. */
/* Each line is the file's time in eight hex digits, a tab, and its path */
const char *file_index_list(enum flat_kind kind);

/* True while a walk is running. */
bool file_index_is_busy(void);

/* One word for what the walk is doing, for the status screen. */
const char *file_index_state(void);

/* Paths recorded in one list, zero until a pass has run this boot. They climb
 * as the walk goes, which is the only progress it can report: it has no idea
 * how many files it is about to see. */
int file_index_count(enum flat_kind kind);

/* Where the walk currently is, "" when it is not running. */
const char *file_index_activity(void);

/* The standard Rebuild/Update triggers. Both mean the same thing here: the
 * walk rewrites every list from scratch, so there is no partial update to
 * distinguish. */
extern struct bg_task file_index_task;

#endif /* _FILE_INDEX_H */
