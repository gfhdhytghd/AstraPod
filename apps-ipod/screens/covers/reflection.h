/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Cover Flow reflections as the stock firmware draws them: a quad half the
 * cover's height directly below it, carrying the cover's lower half mirrored
 * and faded toward the clear colour. The fade is baked into the texture, so
 * a reflection is built once per cover and drawn like the cover itself. */
#ifndef COVERS_REFLECTION_H
#define COVERS_REFLECTION_H

#include "lcd.h"

/* Rows of the reflection of a cover `size` texels tall. */
static inline int reflection_rows(int size)
{
    return size / 2;
}

/* Write the reflection of the square cover `size` texels on a side whose
 * first column starts at `cover`, columns `stride` texels apart, into `out`:
 * `size` columns of reflection_rows(size) texels, column-major, blended over
 * `bg`. Row 0 mirrors the cover's bottom row. */
void reflection_build(fb_data *out, const fb_data *cover, int size,
                      int stride, fb_data bg);

#endif
