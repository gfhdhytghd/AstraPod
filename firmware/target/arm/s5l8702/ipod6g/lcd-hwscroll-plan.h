/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LCD_HWSCROLL_PLAN_H
#define LCD_HWSCROLL_PLAN_H

/* The arithmetic of a hardware-scroll frame. No target headers, so
 * tools/tests/lcd_hwscroll_plan_test.c builds it on the host. */

#include <stdbool.h>
#include <stdint.h>
#include "lcd-hwscroll.h"

#define HWSCROLL_ROWS     240
#define HWSCROLL_COLUMNS  320
/* A band inside the area crosses the ring's wrap at most once. */
#define HWSCROLL_SEGMENTS (2 * LCD_HWSCROLL_BANDS)

/* Screen row r >= top shows panel memory row
 * top + (r - top + offset) % height; rows above top are identity. */
struct hwscroll_ring {
    int top, height, offset;
};

/* One transfer: a screen rectangle that is contiguous in panel memory. */
struct hwscroll_segment {
    int x, y, width, rows;
    int panel_y;
    unsigned int pixel;    /* its first pixel in the stage buffer */
};

/* Microseconds. The scan reaches row y at
 * high + (period - high) * y / HWSCROLL_ROWS after the TE rise. */
struct hwscroll_timing {
    int period, high;
    int open;      /* earliest start after the rise */
    int guard;     /* kept before the scan reaches a written row */
    int command;   /* 0x33 and 0x37 */
    int setup;     /* addressing one segment */
    int full;      /* DMA of the whole screen, slow bound */
};

int hwscroll_ring_map(const struct hwscroll_ring *ring, int y);
int hwscroll_ring_run(const struct hwscroll_ring *ring, int y, int rows);

/* Bands in order of first screen row, narrower first on a tie, each split
 * where the ring wraps; their pixels follow one another in the stage.
 * Returns the segment count, or -1 past HWSCROLL_SEGMENTS. */
int hwscroll_plan_segments(const struct hwscroll_ring *ring,
                           const struct lcd_hwscroll_band *band, int count,
                           struct hwscroll_segment *segment);

/* The start of the commands, after the rise, that writes every segment
 * row before the next scan reads it and sends 0x37 inside the TE pulse:
 * [*open, *close]. False when that interval is empty. */
bool hwscroll_plan_window(const struct hwscroll_timing *timing, int top,
                          const struct hwscroll_segment *segment, int count,
                          int *open, int *close);

/* The first rise + k * period, for any integer k, at or after `from`. */
uint32_t hwscroll_plan_rise(uint32_t rise, uint32_t period, uint32_t from);

#endif
