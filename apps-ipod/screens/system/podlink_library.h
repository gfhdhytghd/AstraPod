/* PodLink library commands. GPL version 2 or later. */
#ifndef PODLINK_LIBRARY_H
#define PODLINK_LIBRARY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool podlink_library_active(void);
void podlink_library_close(void);
const char *podlink_library_command(const unsigned char *data, size_t size,
                                   bool *refresh_pending, unsigned char *binary,
                                   unsigned int *binary_size, uint32_t *raw);

#endif
