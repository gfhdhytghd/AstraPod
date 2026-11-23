/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Who holds the core allocator, for an out-of-memory report. Each block is
 * named by its handle, size and callback table; the table's address resolves
 * to its owner with `nm rockbox.elf` from the same build, and a block with no
 * table is identified by its size alone.
 ****************************************************************************/
#ifndef _CORE_MAP_H
#define _CORE_MAP_H

#include <stddef.h>

/* One line for a panic message: free space, the largest hole, what cannot
 * be shrunk or moved, and the largest holders. Returns the length written. */
size_t core_map_summary(char *buf, size_t size);
/* Appends a full block list to .rockbox/logs/memory.log under `reason`.
 * Yields to write, so the caller must not hold a raw core pointer. */
void core_map_log(const char *reason);

#endif /* _CORE_MAP_H */
