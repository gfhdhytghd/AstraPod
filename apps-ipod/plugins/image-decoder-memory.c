/* Freestanding image decoder memory calls. GNU GPL version 2 or later. */
#include "plugin.h"

void *memcpy(void *dst, const void *src, size_t bytes)
{
    return rb->memcpy(dst, src, bytes);
}

void *memset(void *dst, int value, size_t bytes)
{
    return rb->memset(dst, value, bytes);
}
