/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Translucent rounded shapes with vertical gradients, blended into native
 * pixels row by row.
 ****************************************************************************/

/* Every shape is one coverage function: full inside, a supersampled corner
 * mask in each rounded corner, nothing outside. A corner's mask is built the
 * first time its radius is used and kept, so a repaint costs one blend per
 * pixel and a table lookup per corner pixel. Fills walk whole rows -- the
 * gradient gives one colour per row -- and outlines and shadows walk only
 * the rows and columns their ring can touch. */

#include <stdbool.h>
#include "draw/gloss.h"

#define MASK_SIZE (GLOSS_MAX_RADIUS * GLOSS_MAX_RADIUS)
#define FULL 256

static uint8_t masks[GLOSS_MAX_RADIUS + 1][MASK_SIZE];
static uint16_t masks_ready;

/* Coverage of the top-left quadrant of a circle of radius r, centred on the
 * quadrant's inner corner, 4 x 4 samples a pixel, 0 to 255. */
static const uint8_t *corner_mask(int r)
{
    uint8_t *mask = masks[r];
    if (masks_ready & (1u << r))
        return mask;
    int limit = 64 * r * r;
    for (int i = 0; i < r; i++)
        for (int j = 0; j < r; j++)
        {
            int count = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++)
                {
                    int dx = 8 * r - (8 * j + 2 * sx + 1);
                    int dy = 8 * r - (8 * i + 2 * sy + 1);
                    count += dx * dx + dy * dy <= limit;
                }
            mask[i * r + j] = count * 255 / 16;
        }
    masks_ready |= 1u << r;
    return mask;
}

static int fit_radius(int r, int w, int h)
{
    r = r < w / 2 ? r : w / 2;
    r = r < h / 2 ? r : h / 2;
    r = r < GLOSS_MAX_RADIUS ? r : GLOSS_MAX_RADIUS;
    return r > 0 ? r : 0;
}

/* Coverage, 0 to FULL, of pixel (cx, cy) of a w x h shape whose top
 * corners have radius rt and bottom corners rb. */
static unsigned cover(int w, int h, int rt, int rb, int cx, int cy)
{
    if (cx < 0 || cy < 0 || cx >= w || cy >= h)
        return 0;
    int r = cy < rt ? rt : cy >= h - rb ? rb : 0;
    if (!r)
        return FULL;
    int i = cy < rt ? cy : h - 1 - cy;
    int j = cx < r ? cx : cx >= w - r ? w - 1 - cx : -1;
    if (j < 0)
        return FULL;
    unsigned v = corner_mask(r)[i * r + j];
    return v + (v >> 7);
}

/* `color` (0xRRGGBB) over native pixel `d` at `alpha` of FULL. */
static inline fb_data blend(fb_data d, uint32_t color, unsigned alpha)
{
    int r = ((d >> 11) << 3) | (d >> 13);
    int g = (((d >> 5) & 63) << 2) | ((d >> 9) & 3);
    int b = ((d & 31) << 3) | ((d >> 2) & 7);
    r += ((int)((color >> 16) & 255) - r) * (int)alpha >> 8;
    g += ((int)((color >> 8) & 255) - g) * (int)alpha >> 8;
    b += ((int)(color & 255) - b) * (int)alpha >> 8;
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

/* The same over a run of `count` pixels, at the display's precision: red
 * and blue sit in separate 16-bit lanes of one word, so a pixel costs two
 * multiplies. */
static void blend_run(fb_data *d, int count, uint32_t color, unsigned alpha)
{
    if (alpha >= FULL)
        alpha = FULL;
    unsigned c = ((color >> 8) & 0xf800) | ((color >> 5) & 0x07e0) |
                 ((color & 255) >> 3);
    unsigned keep = FULL - alpha;
    uint32_t rb_c = ((((uint32_t)c & 0xf800) << 5) | (c & 31)) * alpha;
    unsigned g_c = ((c >> 5) & 63) * alpha;
    for (int x = 0; x < count; x++)
    {
        unsigned p = d[x];
        uint32_t rb = ((((uint32_t)p & 0xf800) << 5) | (p & 31)) * keep + rb_c;
        unsigned g = ((p >> 5) & 63) * keep + g_c;
        rb >>= 8;
        d[x] = ((rb >> 5) & 0xf800) | ((g >> 8) << 5) | (rb & 31);
    }
}

static uint32_t mix(uint32_t a, uint32_t b, int num, int den)
{
    uint32_t out = 0;
    for (int shift = 0; shift < 32; shift += 8)
    {
        int ca = (a >> shift) & 255, cb = (b >> shift) & 255;
        out |= (uint32_t)(ca + (cb - ca) * num / den) << shift;
    }
    return out;
}

static uint32_t fill_row(const struct gloss_fill *fill, int row, int h)
{
    if (row == 0)
        return fill->top_line;
    if (row == h - 1)
        return fill->bottom_line;
    return h > 3 ? mix(fill->body_top, fill->body_bottom, row - 1, h - 3)
                 : fill->body_top;
}

static void put(const struct gloss_surface *s, int x, int y, uint32_t argb,
                unsigned coverage)
{
    if (x < s->clip_x || y < s->clip_y || x >= s->clip_x + s->clip_w ||
        y >= s->clip_y + s->clip_h)
        return;
    unsigned alpha = ((argb >> 24) + (argb >> 31)) * coverage >> 8;
    if (alpha)
    {
        fb_data *p = s->pixels + y * s->stride + x;
        *p = blend(*p, argb, alpha);
    }
}

void gloss_rect(const struct gloss_surface *s, int x, int y, int w, int h,
                int rt, int rb, const struct gloss_fill *fill)
{
    rt = fit_radius(rt, w, h);
    rb = fit_radius(rb, w, h);
    int top = s->clip_y > y ? s->clip_y - y : 0;
    int bottom = s->clip_y + s->clip_h - y < h ? s->clip_y + s->clip_h - y : h;
    int left = s->clip_x > x ? s->clip_x - x : 0;
    int right = s->clip_x + s->clip_w - x < w ? s->clip_x + s->clip_w - x : w;
    for (int cy = top; cy < bottom; cy++)
    {
        uint32_t argb = fill_row(fill, cy, h);
        unsigned alpha = (argb >> 24) + (argb >> 31);
        if (!alpha)
            continue;
        int r = cy < rt ? rt : cy >= h - rb ? rb : 0;
        int inner_left = left > r ? left : r;
        int inner_right = right < w - r ? right : w - r;
        fb_data *row = s->pixels + (y + cy) * s->stride + x;
        for (int cx = left; cx < inner_left && cx < right; cx++)
            row[cx] = blend(row[cx], argb,
                            alpha * cover(w, h, rt, rb, cx, cy) >> 8);
        if (inner_right > inner_left)
            blend_run(row + inner_left, inner_right - inner_left, argb, alpha);
        for (int cx = inner_right > left ? inner_right : left; cx < right;
             cx++)
            row[cx] = blend(row[cx], argb,
                            alpha * cover(w, h, rt, rb, cx, cy) >> 8);
    }
}

/* The ring between the shape and the shape inset by one pixel, each pixel
 * weighted by `keep` of FULL, where `keep` excludes a second shape. */
static void ring(const struct gloss_surface *s, int x, int y, int w, int h,
                 int r, uint32_t argb, int ex, int ey, int ew, int eh, int er)
{
    r = fit_radius(r, w, h);
    int inner_r = r > 0 ? r - 1 : 0;
    for (int cy = 0; cy < h; cy++)
    {
        bool edge = cy <= r || cy >= h - 1 - r;
        for (int cx = 0; cx < w; cx++)
        {
            if (!edge && cx > 0 && cx < w - 1)
                cx = w - 1;
            else if (edge && cy > 0 && cy < h - 1 && cx > r && cx < w - 1 - r)
                cx = w - 1 - r;
            int c = (int)cover(w, h, r, r, cx, cy) -
                    (int)cover(w - 2, h - 2, inner_r, inner_r, cx - 1, cy - 1);
            if (c <= 0)
                continue;
            if (ew)
                c = c * (FULL - (int)cover(ew, eh, er, er, x + cx - ex,
                                           y + cy - ey)) >> 8;
            put(s, x + cx, y + cy, argb, c);
        }
    }
}

void gloss_outline(const struct gloss_surface *s, int x, int y, int w, int h,
                   int r, uint32_t argb)
{
    ring(s, x, y, w, h, r, argb, 0, 0, 0, 0, 0);
}

void gloss_shadow(const struct gloss_surface *s, int x, int y, int w, int h,
                  int r, int depth, int drop, unsigned alpha)
{
    r = fit_radius(r, w, h);
    for (int k = 1; k <= depth; k++)
    {
        /* Falls off as the square of the distance, as a soft blur does. */
        unsigned a = alpha * (depth - k + 1) * (depth - k + 1) /
                     (depth * depth);
        ring(s, x - k, y - k + drop, w + 2 * k, h + 2 * k,
             r + k <= GLOSS_MAX_RADIUS ? r + k : GLOSS_MAX_RADIUS,
             a << 24, x, y, w, h, r);
    }
    /* The ring nearest the shape, where the drop uncovers it. */
    ring(s, x, y + drop, w, h, r, alpha << 24, x, y, w, h, r);
}
