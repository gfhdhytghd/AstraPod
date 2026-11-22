/* Foreground hardware decoder lease. GNU GPL version 2 or later. */
#ifndef ASTRAPOD_ROCK_H264_H
#define ASTRAPOD_ROCK_H264_H

#include "config.h"
#include "hw_h264.h"

#ifdef HAVE_HW_H264
extern const struct hw_h264_api rock_h264_api;
#define ROCK_H264_API (&rock_h264_api)
void rock_h264_begin(void *scratch, size_t capacity);
void rock_h264_end(void);
#else
#define ROCK_H264_API NULL
static inline void rock_h264_begin(void *scratch, size_t capacity)
{
    (void)scratch;
    (void)capacity;
}
static inline void rock_h264_end(void) {}
#endif

#endif
