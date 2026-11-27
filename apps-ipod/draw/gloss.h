/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to gloss.c: translucent rounded shapes with vertical gradients,
 * blended into a buffer of native pixels -- glass panels, bevels, glossy
 * highlight bands and drop shadows.
 ****************************************************************************/

#ifndef _GLOSS_H_
#define _GLOSS_H_

#include <stdint.h>
#include "lcd.h"

/* A buffer of native pixels: `pixels` is pixel (0, 0), and only the clip
 * rectangle is written. */
struct gloss_surface {
    fb_data *pixels;
    int stride;
    int clip_x, clip_y, clip_w, clip_h;
};

/* A vertical fill in 0xAARRGGBB, alpha 0 clear and 255 opaque: a one-pixel
 * top line, the body blended evenly from top to bottom, and a one-pixel
 * bottom line. Shapes under three rows tall show only the lines. */
struct gloss_fill {
    uint32_t top_line, body_top, body_bottom, bottom_line;
};

/* Corner radii above this are reduced to it. */
#define GLOSS_MAX_RADIUS 12

/* Blend `fill` into a rectangle with smooth corners: radius `rt` for the top
 * two, `rb` for the bottom two. Radii too large for the box are reduced. */
void gloss_rect(const struct gloss_surface *s, int x, int y, int w, int h,
                int rt, int rb, const struct gloss_fill *fill);

/* Blend a one-pixel outline of the same shape with radius `r` in `argb`. */
void gloss_outline(const struct gloss_surface *s, int x, int y, int w, int h,
                   int r, uint32_t argb);

/* A soft shadow `depth` pixels deep around the shape, `drop` pixels lower,
 * fading out from `alpha` at its edge. The shape's own area is left as it
 * is, so it suits a translucent shape drawn over it afterwards. */
void gloss_shadow(const struct gloss_surface *s, int x, int y, int w, int h,
                  int r, int depth, int drop, unsigned alpha);

#endif /* _GLOSS_H_ */
