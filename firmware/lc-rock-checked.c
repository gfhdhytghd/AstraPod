/* Checked native module loading. GNU GPL version 2 or later. */
#include <stdint.h>
#include <string.h>
#include "system.h"
#include "file.h"
#include "lc-rock-checked.h"

void *lc_open_checked(const char *path, unsigned char *arena, size_t arena_size,
                      void *header, size_t header_size, lc_validate_image validate)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;

    off_t length = ffilesize(fd);
    /* Copy the validated header from the same read that was checked. Read the
     * rest using the same descriptor, with a destination and size we own. */
    bool okay = length >= 0 && (uintmax_t)length <= arena_size &&
                (size_t)length >= header_size &&
                read(fd, header, header_size) == (ssize_t)header_size &&
                validate(header, arena, arena_size, length);
    if (okay)
    {
        size_t rest = length - header_size;
        okay = read(fd, arena + header_size, rest) == (ssize_t)rest;
        if (okay)
            memcpy(arena, header, header_size);
    }
    close(fd);
    if (!okay)
        return NULL;

    lc_commit_image();
    return arena;
}
