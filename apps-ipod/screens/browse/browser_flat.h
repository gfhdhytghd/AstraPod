/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to browser_flat.c.
 ****************************************************************************/
#ifndef _BROWSER_FLAT_H
#define _BROWSER_FLAT_H

#include "files/file_index.h"

/* Every document, image or video on the player, by 'kind', in one flat list.
 * Owns the screen until the user leaves. Returns a GO_TO_* code. */
int browser_flat(enum flat_kind kind);

/* global_settings.sort_documents and sort_images */
enum flat_sort {
    FLAT_SORT_NAME = 0,
    FLAT_SORT_NAME_REVERSE,
    FLAT_SORT_NEWEST,
};

#endif /* _BROWSER_FLAT_H */
