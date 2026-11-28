/***************************************************************************
 * PodBox Fruit Company Ultra page motion. GPL version 2+.
 *
 * Fruit Company slides pages side by side. Under Ultra they stack: the
 * page on the right, always the deeper one, lies over the page on the left,
 * which moves a third as fast, darkens as it is covered and takes a soft
 * shadow from the upper page's edge. Between two split menus the rows and
 * the pane each stack the same way inside their own column, at half the
 * pace, and a status bar that stays in place hands its title over with a
 * short slide. At shutdown the screen shrinks a little as it fades.
 *
 * Parts: the stacked pages; the title; the slide's timing; the shutdown
 * fade.
 ****************************************************************************/

#include <string.h>
#include "config.h"
#include "system.h"
#include "menu_pane.h"
#include "retained.h"
#include "ultra.h"
#include "ultra_scene.h"

/* The lower page travels this many 256ths of the upper page's distance and
 * loses up to DIM_STEPS 16ths of its light under it. */
#define PARALLAX 85
#define DIM_STEPS 5
#define SHADOW_W 12
/* How far a title travels as it hands over, and the widest gap inside
 * one. */
#define TITLE_SHIFT 24
#define TITLE_GAP 6
/* The slide lasts this many sixths of the base theme's, the shutdown fade
 * this many halves, and the screen shrinks by up to FADE_SHRINK 256ths as
 * it fades. */
#define SLIDE_SIXTHS 7
#define FADE_HALVES 3
#define FADE_SHRINK 16

/* ---- the stacked pages ---- */

/* The shadow's brightness by pixel pair, from its outer pair to the edge,
 * in 256ths: 256 - 100 d^2 with d the share of the way to the edge. */
static const uint8_t shadow_ramp[SHADOW_W / 2] = {
    253, 245, 231, 212, 187, 156,
};

/* A pixel pair shifted down by `s` keeps each channel's own bits under
 * this mask. */
static inline __attribute__((always_inline)) uint32_t dim_mask(unsigned s)
{
    return s == 2 ? 0x39e739e7 : s == 3 ? 0x18e318e3 : 0x08610861;
}

/* `n` pixel pairs of `src`, less their copies shifted down by `a` and, if
 * it is not 0, by `b`. */
static inline __attribute__((always_inline))
void dim_run(ultra_pair *out, const ultra_pair *src, int n,
             unsigned a, unsigned b)
{
#define DIM(v) ((v) - (((v) >> a) & dim_mask(a)) - \
                (b ? ((v) >> b) & dim_mask(b) : 0))
    for (; n >= 4; n -= 4, out += 4, src += 4)
    {
        uint32_t v0 = src[0], v1 = src[1], v2 = src[2], v3 = src[3];
        out[0] = DIM(v0);
        out[1] = DIM(v1);
        out[2] = DIM(v2);
        out[3] = DIM(v3);
    }
    for (; n > 0; n--, src++)
        *out++ = DIM(*src);
#undef DIM
}

/* `n` pixel pairs of `src` with `step` 16ths of their light taken away,
 * by subtracting shifted copies. This is the slide's inner loop. */
static ULTRA_ARM __attribute__((noinline, optimize("O2")))
void dim_pairs(ultra_pair *out, const ultra_pair *src, int n, int step)
{
    switch (step)
    {
    case 0:
        memcpy(out, src, n * sizeof(*out));
        break;
    case 1:
        dim_run(out, src, n, 4, 0);
        break;
    case 2:
        dim_run(out, src, n, 3, 0);
        break;
    case 3:
        dim_run(out, src, n, 3, 4);
        break;
    case 4:
        dim_run(out, src, n, 2, 0);
        break;
    default:
        dim_run(out, src, n, 2, 4);
        break;
    }
}

/* Rows [y0, y1) of a `width`-wide region: `r` from column `p` on, over `l`,
 * which has receded under it. Pointers are at the region's first column;
 * `p` is even. */
static ULTRA_ARM void stack_rows(fb_data *out, const fb_data *l,
                                 const fb_data *r, int width, int p,
                                 int y0, int y1)
{
    int covered = width - p;
    int shift = (covered * PARALLAX >> 8) & ~1;
    int step = (2 * DIM_STEPS * covered + width) / (2 * width);
    unsigned light = UI_FULL_OPACITY - 16 * step;
    int edge = covered ? MAX(0, p - SHADOW_W) / 2 : p / 2;
    uint8_t shade[SHADOW_W / 2];
    for (int x = edge; x < p / 2; x++)
        shade[x - edge] = ((light * shadow_ramp[SHADOW_W / 2 - (p / 2 - x)]
                            >> 8) + 4) >> 3;
    for (int y = y0; y < y1; y++)
    {
        int i = y * LCD_WIDTH;
        const ultra_pair *src = (const ultra_pair *)(l + i + shift);
        ultra_pair *row = (ultra_pair *)(out + i);
        dim_pairs(row, src, edge, step);
        for (int x = edge; x < p / 2; x++)
            row[x] = ultra_dim_pair(src[x], shade[x - edge]);
        memcpy(out + i + p, r + i, covered * sizeof(*out));
    }
}

/* ---- the title ---- */

/* `alpha` 256ths of pixel `c` over `under`, where `c` differs from the
 * bar's own colour `bg`. */
static inline fb_data over(fb_data under, fb_data c, fb_data bg,
                           unsigned alpha)
{
    return c == bg || !alpha ? under : ultra_mix32(c, under, (alpha + 4) >> 3);
}

static unsigned clamp_alpha(int alpha)
{
    return alpha < 0 ? 0 : alpha > UI_FULL_OPACITY ? UI_FULL_OPACITY : alpha;
}

/* Whether column `x` of either bar is drawn on. The bar's own colour in
 * each row is that of its first column. */
static bool inked(const fb_data *old, const fb_data *now, int x, int rows)
{
    for (int i = 0; i < rows * LCD_WIDTH; i += LCD_WIDTH)
        if (old[i + x] != now[i] || now[i + x] != now[i])
            return true;
    return false;
}

/* The top `rows` of a `width`-wide bar, `f` 256ths of the way from the
 * title in `old` to the one in `now`. The old title leaves towards `dir`
 * and the new one arrives from the other side; the rest of the bar is
 * `now`'s. A title is the columns where the bars differ, widened over any
 * letters the two titles share. */
static void retitle(fb_data *out, const fb_data *old, const fb_data *now,
                    int width, int rows, int f, int dir)
{
    int d0 = width, d1 = -1;
    for (int y = 0; y < rows; y++)
    {
        const fb_data *a = old + y * LCD_WIDTH, *b = now + y * LCD_WIDTH;
        memcpy(out + y * LCD_WIDTH, b, width * sizeof(*out));
        for (int x = 1; x < width; x++)
            if (a[x] != b[x])
            {
                d0 = MIN(d0, x);
                d1 = MAX(d1, x);
            }
    }
    if (d1 < d0)
        return;
    for (int x = d0 - 1; x > 0 && x >= d0 - TITLE_GAP; x--)
        if (inked(old, now, x, rows))
            d0 = x;
    for (int x = d1 + 1; x < width && x <= d1 + TITLE_GAP; x++)
        if (inked(old, now, x, rows))
            d1 = x;
    /* The rows either title is drawn on; the rest are `now`'s already. */
    int y0 = rows, y1 = 0;
    for (int y = 0; y < rows; y++)
        for (int x = d0; x <= d1; x++)
        {
            int i = y * LCD_WIDTH;
            if (old[i + x] != now[i] || now[i + x] != now[i])
            {
                y0 = MIN(y0, y);
                y1 = y + 1;
                break;
            }
        }
    int gone = dir * (TITLE_SHIFT * f >> 8);
    int coming = -dir * (TITLE_SHIFT * (UI_FULL_OPACITY - f) >> 8);
    unsigned old_alpha = clamp_alpha(UI_FULL_OPACITY - f * 8 / 5);
    unsigned new_alpha = clamp_alpha((f * 8 - 3 * UI_FULL_OPACITY) / 5);
    int x0 = MAX(0, d0 - TITLE_SHIFT), x1 = MIN(width, d1 + 1 + TITLE_SHIFT);
    for (int y = y0; y < y1; y++)
    {
        int i = y * LCD_WIDTH;
        fb_data bg = now[i];
        for (int x = x0; x < x1; x++)
        {
            fb_data c = bg;
            int from = x - gone, to = x - coming;
            if (from >= d0 && from <= d1)
                c = over(c, old[i + from], bg, old_alpha);
            if (to >= d0 && to <= d1)
                c = over(c, now[i + to], bg, new_alpha);
            out[i + x] = c;
        }
    }
}

void ultra_scene_compose(const struct ui_scene *scene, fb_data *out,
                         const fb_data *back, const fb_data *front,
                         const fb_data *chrome, int offset)
{
    const int half = MENU_PANE_X;
    bool front_right = offset >= 0;
    const fb_data *l = front_right ? back : front;
    const fb_data *r = front_right ? front : back;
    /* The upper page's left edge. */
    int p = front_right ? offset : LCD_WIDTH + offset;
    /* A panel's split flags are those of the last page slide. */
    bool panel = scene->backdrop_handle > 0;
    bool l_split = !panel &&
        (front_right ? scene->back_split : scene->front_split);
    bool r_split = !panel &&
        (front_right ? scene->front_split : scene->back_split);
    bool target_right = front_right == scene->target_front;
    /* How far the slide has gone towards its destination, in 256ths. */
    int f = (target_right ? LCD_WIDTH - p : p) * UI_FULL_OPACITY / LCD_WIDTH;
    const fb_data *from = target_right ? l : r;

    if (scene->full_screen || l_split != r_split)
    {
        stack_rows(out, l, r, LCD_WIDTH, p, 0, LCD_HEIGHT);
        return;
    }
    int top = scene->pan_top;
    int width = l_split ? half : LCD_WIDTH;
    if (l_split)
    {
        int q = (p / 2) & ~1;
        stack_rows(out, l, r, half, q, top, LCD_HEIGHT);
        /* The pane runs up beside the bar. */
        stack_rows(out + half, l + half, r + half, half, q, 0, LCD_HEIGHT);
    }
    else
        stack_rows(out, l, r, LCD_WIDTH, p, top, LCD_HEIGHT);
    if (top)
        retitle(out, from, chrome, width, top, f, target_right ? -1 : 1);
}

/* ---- the slide's timing ---- */

unsigned ultra_scene_frames(unsigned frames)
{
    return (frames * SLIDE_SIXTHS + 5) / 6;
}

/* A cubic ease-out with a quarter of constant speed: the page leaves at
 * once and lands without a bounce, still moving two pixels or more each
 * frame. It lands a frame early, so the page at rest shows as soon as the
 * motion reaches it. */
int ultra_scene_sample(const struct ui_tween *tween, unsigned frame)
{
    if (frame + 1 >= tween->frames)
        return tween->to;
    unsigned u = frame * UI_ULTRA_ONE / (tween->frames - 1);
    int eased = (ui_ultra_out(u) * 3 + (int)u) / 4;
    return ui_ultra_mix(tween->from, tween->to, eased);
}

/* ---- the shutdown fade ---- */

unsigned ultra_scene_fade_frames(unsigned frames)
{
    return frames * FADE_HALVES / 2;
}

/* The screen shrinks about its centre by up to FADE_SHRINK 256ths as it
 * darkens, into black. A column map, made once a frame, picks each output
 * column's source. */
ULTRA_ARM void ultra_scene_fade(fb_data *out, const fb_data *source,
                                int level)
{
    int gone = UI_FULL_OPACITY - level;
    /* The source step per output pixel, Q16. */
    uint32_t step = (uint32_t)((1ull << 32) /
        (UI_FULL_OPACITY * UI_FULL_OPACITY - FADE_SHRINK * gone));
    int w = (int)(((uint64_t)LCD_WIDTH << 16) / step) & ~1;
    int h = (int)(((uint64_t)LCD_HEIGHT << 16) / step);
    int x0 = ((LCD_WIDTH - w) / 2) & ~1, y0 = (LCD_HEIGHT - h) / 2;
    unsigned light = ((unsigned)level + 4) >> 3;
    uint16_t columns[LCD_WIDTH];
    for (int x = 0; x < w; x++)
        columns[x] = (uint16_t)((x * step + step / 2) >> 16);
    memset(out, 0, y0 * LCD_WIDTH * sizeof(*out));
    for (int y = y0; y < y0 + h; y++)
    {
        fb_data *row = out + y * LCD_WIDTH;
        const fb_data *src = source +
            (((y - y0) * step + step / 2) >> 16) * LCD_WIDTH;
        memset(row, 0, x0 * sizeof(*row));
        for (int x = 0; x < w; x++)
            row[x0 + x] = ultra_dim32(src[columns[x]], light);
        memset(row + x0 + w, 0, (LCD_WIDTH - x0 - w) * sizeof(*row));
    }
    memset(out + (y0 + h) * LCD_WIDTH, 0,
           (LCD_HEIGHT - y0 - h) * LCD_WIDTH * sizeof(*out));
}
