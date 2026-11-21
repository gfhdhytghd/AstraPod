/* Checked native module loading. GNU GPL version 2 or later. */
#ifndef LC_ROCK_CHECKED_H
#define LC_ROCK_CHECKED_H

#include <stdbool.h>
#include <stddef.h>

void lc_commit_image(void);

typedef bool (*lc_validate_image)(const void *header, const unsigned char *arena,
                                  size_t arena_size, size_t file_size);
void *lc_open_checked(const char *path, unsigned char *arena, size_t arena_size,
                      void *header, size_t header_size, lc_validate_image validate);
#endif
