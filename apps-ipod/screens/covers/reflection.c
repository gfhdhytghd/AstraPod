/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "config.h"
#include "reflection.h"

/* The stock reflection's opacity in 32nds, one entry per row of its
 * 64-row texture, read from the stock firmware's frames. */
static const unsigned char fade[64] = {
    15, 15, 15, 15, 14, 14, 14, 13, 13, 13, 13, 12, 12, 12, 11, 11,
    11, 10, 10, 10,  9,  9,  9,  9,  8,  8,  8,  8,  7,  7,  7,  6,
     6,  6,  6,  5,  5,  5,  5,  4,  4,  4,  4,  4,  3,  3,  3,  3,
     3,  2,  2,  2,  2,  2,  2,  1,  1,  1,  1,  1,  1,  1,  1,  0,
};

/* Blend at 8 bits a channel and truncate once, as stock composes. */
static inline fb_data blend(fb_data texel, unsigned alpha, unsigned r0,
                            unsigned g0, unsigned b0)
{
    unsigned r = RGB_UNPACK_RED(texel), g = RGB_UNPACK_GREEN(texel),
             b = RGB_UNPACK_BLUE(texel);
    return LCD_RGBPACK((r * alpha + r0 * (32 - alpha)) >> 5,
                       (g * alpha + g0 * (32 - alpha)) >> 5,
                       (b * alpha + b0 * (32 - alpha)) >> 5);
}

void reflection_build(fb_data *out, const fb_data *cover, int size,
                      int stride, fb_data bg)
{
    int rows = reflection_rows(size);
    unsigned r0 = RGB_UNPACK_RED(bg), g0 = RGB_UNPACK_GREEN(bg),
             b0 = RGB_UNPACK_BLUE(bg);
    for (int x = 0; x < size; x++, cover += stride)
        for (int y = 0; y < rows; y++)
            *out++ = blend(cover[size - 1 - y], fade[y * 64 / rows],
                           r0, g0, b0);
}
