/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LCD_VSYNC_H
#define LCD_VSYNC_H

#include <stdbool.h>
#include <stdint.h>
#include "config.h"

#if defined(IPOD_6G) && (CONFIG_PLATFORM & PLATFORM_NATIVE) && \
    !defined(BOOTLOADER)
#define HAVE_LCD_VSYNC

/* The panel refresh period in 1/256 microseconds and the USEC_TIMER time of a
 * TE rise. False while the panel's TE is not measured. *edge_known is false
 * when no rise is recent enough to predict the phase from. */
bool lcd_vsync_timing(uint32_t *period_q8, uint32_t *edge, bool *edge_known);

/* Sleeps the calling thread until USEC_TIMER reaches deadline. Wakes from
 * the user timer, or rounds up to kernel ticks while another owner has it. */
void lcd_vsync_sleep_until(uint32_t deadline);

/* The last TE-synchronized transfer: how long it waited for its window and
 * how long the last DMA took, in microseconds. *count advances per transfer. */
void lcd_vsync_last_transfer(uint32_t *wait, uint32_t *dma, unsigned int *count);

/* Async LCD scheduling. The next slot is a USEC_TIMER deadline. Phase and
 * the calling thread's copy time are UINT32_MAX when unavailable. Outputs
 * may be NULL. False means asynchronous submission is inactive. */
bool lcd_vsync_async_timing(uint32_t *busy, uint32_t *phase,
                           uint32_t *next_slot, uint32_t *copy);
#endif

#endif
