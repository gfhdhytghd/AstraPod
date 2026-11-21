/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PODLINK_PROFILE_H
#define PODLINK_PROFILE_H

#include <stdbool.h>
#include <stdint.h>

enum podlink_stage {
    PL_READ, PL_FILE_CRC, PL_FRAME_CRC, PL_MAILBOX_COPY,
    PL_RING_COPY, PL_DMA_COPY, PL_DMA_ACTIVE, PL_DMA_GAP, PL_STAGE_COUNT
};

struct podlink_stage_sample {
    uint32_t count, bytes, us, max_us;
};

#ifdef HAVE_USB_PODLINK
uint32_t podlink_profile_now(void);
void podlink_profile_add(enum podlink_stage stage, uint32_t start, unsigned bytes);
void podlink_profile_frame_crc(uint32_t start, unsigned bytes);
bool podlink_profile_start(bool fast);
/* Called with IRQs disabled, before dispatching the wake interrupt. */
void podlink_profile_idle(uint32_t start);
void podlink_profile_stop(struct podlink_stage_sample *out,
                          uint32_t *idle_us, uint32_t *wall_us);
#else
static inline uint32_t podlink_profile_now(void) { return 0; }
static inline void podlink_profile_add(enum podlink_stage stage, uint32_t start,
                                       unsigned bytes)
{
    (void)stage;
    (void)start;
    (void)bytes;
}
#endif

#endif
