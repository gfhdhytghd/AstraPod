/* AstraPod playback thread priorities. GPL version 2+. */
#ifndef ASTRA_PLAYBACK_PRIORITY_H
#define ASTRA_PLAYBACK_PRIORITY_H

#include "config.h"
#include "thread.h"

/* Keep ordinary decode and file refill ahead of the boosted UI (14).
 * PCM's existing low-watermark boosts can raise decode further. */
#ifdef IPOD_6G
#define PRIORITY_PLAYBACK_REFILL (PRIORITY_USER_INTERFACE - 3)
#define PRIORITY_PLAYBACK_READY  PRIORITY_PLAYBACK_REFILL
#else
#define PRIORITY_PLAYBACK_REFILL PRIORITY_BUFFERING
#define PRIORITY_PLAYBACK_READY  PRIORITY_PLAYBACK
#endif

#endif
