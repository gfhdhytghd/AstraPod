/* Foreground lease on the codec IRAM window. GNU GPL version 2 or later. */
#ifndef ROCK_IRAM_H
#define ROCK_IRAM_H

#include <stdbool.h>
#include <stdint.h>
#include "config.h"

#if defined(ASTRAPOD_ROCK_COMPAT) && defined(IPOD_6G)
#define HAVE_ROCK_IRAM_LEASE

bool rock_iram_acquire(void);
bool rock_iram_drain_playback(void);
bool rock_iram_release(void);
bool rock_iram_playback_blocked(void);
bool rock_iram_codec_begin(void);
void rock_iram_codec_end(void);
bool rock_iram_defer_usb(intptr_t seqnum);
bool rock_iram_take_usb(intptr_t *seqnum);
void rock_iram_finish_usb(void);
#else
static inline bool rock_iram_playback_blocked(void) { return false; }
static inline bool rock_iram_codec_begin(void) { return true; }
static inline void rock_iram_codec_end(void) {}
static inline bool rock_iram_defer_usb(intptr_t seqnum)
{
    (void)seqnum;
    return false;
}
#endif

#endif
