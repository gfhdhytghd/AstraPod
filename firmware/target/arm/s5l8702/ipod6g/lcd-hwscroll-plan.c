/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "lcd-hwscroll-plan.h"

static int min_int(int a, int b)
{
    return a < b ? a : b;
}

int hwscroll_ring_map(const struct hwscroll_ring *ring, int y)
{
    if (y < ring->top)
        return y;
    return ring->top + (y - ring->top + ring->offset) % ring->height;
}

int hwscroll_ring_run(const struct hwscroll_ring *ring, int y, int rows)
{
    if (!ring->offset)
        return rows;
    if (y < ring->top)
        return min_int(rows, ring->top - y);
    return min_int(rows, ring->top + ring->height -
                         hwscroll_ring_map(ring, y));
}

int hwscroll_plan_segments(const struct hwscroll_ring *ring,
                           const struct lcd_hwscroll_band *band, int count,
                           struct hwscroll_segment *segment)
{
    int order[LCD_HWSCROLL_BANDS];
    for (int i = 0; i < count; i++)
    {
        int j = i;
        for (; j > 0; j--)
        {
            const struct lcd_hwscroll_band *a = &band[order[j - 1]];
            if (a->y < band[i].y ||
                (a->y == band[i].y && a->width <= band[i].width))
                break;
            order[j] = order[j - 1];
        }
        order[j] = i;
    }

    int segments = 0;
    unsigned int pixel = 0;
    for (int i = 0; i < count; i++)
    {
        const struct lcd_hwscroll_band *b = &band[order[i]];
        for (int y = b->y, left = b->height; left > 0;)
        {
            if (segments == HWSCROLL_SEGMENTS)
                return -1;
            int rows = hwscroll_ring_run(ring, y, left);
            segment[segments++] = (struct hwscroll_segment){
                .x = b->x, .y = y, .width = b->width, .rows = rows,
                .panel_y = hwscroll_ring_map(ring, y), .pixel = pixel,
            };
            pixel += (unsigned int)rows * b->width;
            y += rows;
            left -= rows;
        }
    }
    return segments;
}

static int scan(const struct hwscroll_timing *timing, int y)
{
    return timing->high + (timing->period - timing->high) * y / HWSCROLL_ROWS;
}

bool hwscroll_plan_window(const struct hwscroll_timing *timing, int top,
                          const struct hwscroll_segment *segment, int count,
                          int *open, int *close)
{
    /* 0x37 lands before the scan reaches the area whether it applies at
     * once or at the end of the TE pulse. */
    int last = min_int(timing->high, scan(timing, top)) - timing->guard -
               timing->command;
    int elapsed = timing->command;
    for (int i = 0; i < count; i++)
    {
        const struct hwscroll_segment *s = &segment[i];
        uint32_t pixels = (uint32_t)s->width * s->rows;
        int cost = ((uint32_t)timing->full * pixels +
                    HWSCROLL_ROWS * HWSCROLL_COLUMNS - 1) /
                   (HWSCROLL_ROWS * HWSCROLL_COLUMNS);
        int first_row = (cost + s->rows - 1) / s->rows;
        elapsed += timing->setup;
        /* Both are linear in the row, so the end rows bound the rest. */
        last = min_int(last, scan(timing, s->y) - timing->guard -
                             elapsed - first_row);
        last = min_int(last, scan(timing, s->y + s->rows - 1) -
                             timing->guard - elapsed - cost);
        elapsed += cost;
    }
    *open = timing->open;
    *close = last;
    return last >= timing->open;
}

uint32_t hwscroll_plan_rise(uint32_t rise, uint32_t period, uint32_t from)
{
    int32_t ahead = (int32_t)(from - rise);
    int32_t step = (int32_t)period;
    int32_t k = ahead >= 0 ? (ahead + step - 1) / step : -(-ahead / step);
    return rise + (uint32_t)(k * step);
}
