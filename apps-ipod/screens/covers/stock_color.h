/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef COVERS_STOCK_COLOR_H
#define COVERS_STOCK_COLOR_H

#include <stdint.h>

/* Stock Cover Flow blends RGB565 with five fractional bits. Keep the
 * channel products in separate lanes and round each contribution. */
#define STOCK_COLOR_LANES 0x07e0f81fu

static inline uint32_t stock_color_lanes(uint32_t color)
{
    return (color | (color << 16)) & STOCK_COLOR_LANES;
}

static inline uint16_t stock_color_mix(uint16_t a, uint16_t b, unsigned weight)
{
    const uint32_t bias = 0x01e07e0fu;
    uint32_t left = (stock_color_lanes(a) * (32 - weight) + bias) >> 5;
    uint32_t right = (stock_color_lanes(b) * weight + bias) >> 5;
    uint32_t color = (left + right) & STOCK_COLOR_LANES;
    return color | (color >> 16);
}

#endif
