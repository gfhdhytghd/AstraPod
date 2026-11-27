/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "config.h"
#include "reflection.h"
#include "stock_color.h"

/* Stock starts at 0.6, steps toward zero over the reflection's height and
 * raises the result to 1.4 before quantizing the background weight to
 * 32nds. Tabulate the resulting art weights for its 64-row texture. */
static const unsigned char fade[64] = {
    16, 16, 15, 15, 15, 14, 14, 14, 13, 13, 13, 13, 12, 12, 12, 11,
    11, 11, 10, 10, 10,  9,  9,  9,  9,  8,  8,  8,  8,  7,  7,  7,
     6,  6,  6,  6,  5,  5,  5,  5,  4,  4,  4,  4,  4,  3,  3,  3,
     3,  3,  2,  2,  2,  2,  2,  2,  1,  1,  1,  1,  1,  1,  1,  1,
};

void reflection_build(fb_data *out, const fb_data *cover, int size,
                      int stride, fb_data bg)
{
    int rows = reflection_rows(size);
    for (int x = 0; x < size; x++, cover += stride)
        for (int y = 0; y < rows; y++)
            *out++ = stock_color_mix(cover[size - 1 - y], bg,
                                     32 - fade[y * 64 / rows]);
}
