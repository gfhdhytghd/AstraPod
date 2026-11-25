/* pfgeom -- checks the carousel's occlusion cull, on the host.
 *
 * apps-ipod/screens/covers/carousel.c skips drawing the parts of a cover that
 * a nearer one already hides. That is a claim about projection geometry, and
 * the cost of getting it wrong is a stripe of stale framebuffer that no later
 * pass repairs -- on one album shape, at one point in one scroll, on hardware.
 *
 * So the claim is checked rather than argued about. This mirrors the carousel's
 * projection, its cull and its draw order, renders each frame twice -- once
 * with the cull and once without -- and compares which slide ended up on top of
 * every pixel. The two must agree exactly. It also counts the pixels saved,
 * which is the only reason the cull exists.
 *
 * See tools/pfgeom/README.md. Everything below the CAROUSEL MIRROR banner is a
 * copy of carousel.c and has to be updated with it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <limits.h>
#include <math.h>

#define MAX(a,b) ((a)>(b)?(a):(b))
#define MIN(a,b) ((a)<(b)?(a):(b))

/* ======================= CAROUSEL MIRROR ==============================
 * Kept identical to apps-ipod/screens/covers/carousel.c.
 * ====================================================================== */

typedef int32_t PFreal;

#define LCD_WIDTH  320
#define LCD_HEIGHT 240

#define PFREAL_SHIFT 10
#define PFREAL_ONE (1 << PFREAL_SHIFT)
#define PFREAL_HALF (PFREAL_ONE >> 1)
#define IANGLE_MAX 1024
#define IANGLE_MASK 1023

#define DISPLAY_HEIGHT (LCD_HEIGHT * 2 / 3)
#define DISPLAY_WIDTH MAX((LCD_HEIGHT / 2), (LCD_WIDTH * 2 / 5))
#define CAM_DIST 137
#define CAM_DIST_R (CAM_DIST << PFREAL_SHIFT)
#define DISPLAY_LEFT_R (PFREAL_HALF - LCD_WIDTH * PFREAL_HALF)
#define MAXSLIDE_LEFT_R (PFREAL_HALF - DISPLAY_WIDTH * PFREAL_HALF)

#define ALBUM_COVERS_NUM_SLIDES 6
#define MAX_SLIDES_COUNT 10

#define PF_CULL_MARGIN 1
#define PF_CULL_MIRROR(v) (LCD_WIDTH - 1 - (v))
#define PF_ROW_MARGIN 2

/* the settings the geometry reads */
static int set_center_margin = 20;      /* album covers center margin, 0..80 */
static int set_slide_tuck    = 32;      /* album covers slide tuck,    0..64 */

/* the viewport split, from init() */
static int pf_half_height, pf_lower_half;

/* A slide carries its own bitmap size here; on the device that comes from
 * surface(), and covers keep their aspect, so it genuinely varies. */
struct slide_data { int slide_index; int angle; PFreal cx, cy;
                    PFreal distance; int sw, sh; };
struct dim { int width, height; };

static struct slide_data left_slides[MAX_SLIDES_COUNT];
static struct slide_data right_slides[MAX_SLIDES_COUNT];
static struct slide_data center_slide;
static PFreal offsetX, stack_spacing;
static int slide_frame, target, center_index;

static inline PFreal fmul(PFreal a, PFreal b) { return (a*b) >> PFREAL_SHIFT; }
static inline PFreal fmuln(PFreal a, PFreal b, int ps1, int ps2)
{ return ((a >> ps1) * (b >> ps2)) >> (PFREAL_SHIFT - ps1 - ps2); }

static const char clz_lut[16] = { 4,3,2,2,1,1,1,1,0,0,0,0,0,0,0,0 };
static inline int clz(uint32_t v)
{
    int r = 28;
    if (v >= 0x10000) { v >>= 16; r -= 16; }
    if (v & 0xff00)   { v >>= 8;  r -= 8;  }
    if (v & 0xf0)     { v >>= 4;  r -= 4;  }
    return r + clz_lut[v];
}
static inline int allowed_shift(int32_t val)
{ uint32_t uval = val ^ (val >> 31); return clz(uval) - 1; }
/* Not in the mirror: counts divisions by zero, which on the device are a
 * crash, and steps round them so the sweep can go on. */
static long long fdiv_zero;
static inline PFreal fdiv(PFreal num, PFreal den)
{
    int shift = allowed_shift(num);
    shift = MIN(PFREAL_SHIFT, shift);
    num <<= shift;
    den >>= PFREAL_SHIFT - shift;
    if (!den) { fdiv_zero++; den = 1; }
    return num / den;
}
static inline int div_cam_dist(int n)
{
    if (CAM_DIST != 137 || n < 0)
        return n / CAM_DIST;
    return (int)(((uint64_t)(unsigned)n * 0xef2eb720u) >> 39);
}
static inline int div_by_recip(unsigned int num, unsigned int den,
                               unsigned int recip)
{
    unsigned int q = ((uint64_t)num * recip) >> 32;

    if ((q + 1) * den <= num)
        q++;
    return q;
}
#define fmin(a,b) (((a) < (b)) ? (a) : (b))
#define fmax(a,b) (((a) > (b)) ? (a) : (b))
#define fabsr(a) ((a) < 0 ? -(a) : (a))

static const short sin_tab[] = {
        0,   100,   200,   297,   392,   483,   569,   650,
      724,   792,   851,   903,   946,   980,  1004,  1019,
     1024,  1019,  1004,   980,   946,   903,   851,   792,
      724,   650,   569,   483,   392,   297,   200,   100,
        0,  -100,  -200,  -297,  -392,  -483,  -569,  -650,
     -724,  -792,  -851,  -903,  -946,  -980, -1004, -1019,
    -1024, -1019, -1004,  -980,  -946,  -903,  -851,  -792,
     -724,  -650,  -569,  -483,  -392,  -297,  -200,  -100,
        0
};

static inline PFreal fsin(int iangle)
{
    iangle &= IANGLE_MASK;
    int i = (iangle >> 4);
    PFreal p = sin_tab[i], q = sin_tab[i+1], g = (q - p);
    return p + g * (iangle - i*16)/16;
}
static inline PFreal fcos(int iangle) { return fsin(iangle + (IANGLE_MAX >> 2)); }

static struct dim slide_dim;
static struct dim *surface(struct slide_data *s)
{
    slide_dim.width = s->sw;
    slide_dim.height = s->sh;
    return &slide_dim;
}

#define PF_SLOT_X      (1046 * PFREAL_ONE / 10)
#define PF_STACK_PITCH (282 * PFREAL_ONE / 10)
#define PF_MARGIN_REST 20
#define PF_TUCK_REST   32

static void recalc_offsets(void)
{
    offsetX = PF_SLOT_X + (set_center_margin
                           - PF_MARGIN_REST + PF_TUCK_REST
                           - set_slide_tuck)
                          * PFREAL_ONE;
    stack_spacing = PF_STACK_PITCH;
}

#define FLOW_FLOOR      200
#define FLOW_GAIN       5700
#define FLOW_CAP        25
#define FLOW_TURN_RATE  8
#define FLOW_LEAD_MAX   11
#define FLOW_STEP_US    31250
#define FLOW_TURN_EPS   256
#define PF_SIDE_ANGLE   255

#define FLOW_RING 16
static struct flow_turn
{
    int index;
    int turn;
} flow_turns[FLOW_RING];

static int flow_chase(int from, int to, uint32_t dt_us)
{
    int d = to - from;
    int dist = d < 0 ? -d : d;
    int64_t rate = (int64_t)FLOW_FLOOR * 65536 + (int64_t)FLOW_GAIN * dist;
    int64_t adv = MIN(rate, (int64_t)FLOW_CAP * 1000 * 65536)
                * dt_us / 1000000000;

    if (adv >= dist)
        return to;
    if (adv < 1)
        adv = 1;
    return d < 0 ? from - (int)adv : from + (int)adv;
}

static int flow_lead(void)
{
    int d = (target << 16) - slide_frame;

    return slide_frame + MAX(-65536, MIN(d, 65536));
}

static int flow_rest_turn(int index, int lead)
{
    int r = (index << 16) - lead;
    int a = MIN(r < 0 ? -r : r, 65536);

    return r < 0 ? a : -a;
}

static struct flow_turn *flow_turn_of(int index, int lead)
{
    struct flow_turn *t = &flow_turns[index & (FLOW_RING - 1)];

    if (t->index != index)
    {
        t->index = index;
        t->turn = flow_rest_turn(index, lead);
    }
    return t;
}

static bool flow_turn_step(uint32_t dt_us)
{
    int lead = flow_lead();
    int lo = ((slide_frame + 0x8000) >> 16) - ALBUM_COVERS_NUM_SLIDES;
    int hi = lo + 2 * ALBUM_COVERS_NUM_SLIDES;
    int64_t x = (int64_t)FLOW_TURN_RATE * 65536 * dt_us / 1000000;
    int a = (int)(x - (x * x >> 17));
    bool rest = true;
    int i;

    for (i = 0; i < FLOW_RING; i++)
        if (flow_turns[i].index < lo || flow_turns[i].index > hi)
            flow_turns[i].index = INT_MIN;

    for (i = lo; i <= hi; i++)
    {
        int goal = flow_rest_turn(i, lead);
        struct flow_turn *t = flow_turn_of(i, lead);

        t->turn += (int)((int64_t)(goal - t->turn) * a >> 16);
        if (abs(goal - t->turn) > FLOW_TURN_EPS)
            rest = false;
    }
    return rest;
}

static PFreal flow_cx(int r)
{
    int a = r < 0 ? -r : r;
    PFreal x = (a <= 65536) ? fmul(offsetX, a >> 6)
             : offsetX + fmul(stack_spacing, (a - 65536) >> 6);

    return r < 0 ? -x : x;
}

static void flow_place(struct slide_data *si, int index)
{
    int turn = flow_turn_of(index, flow_lead())->turn;

    si->slide_index = index;
    si->cx = flow_cx((index << 16) - slide_frame);
    si->angle = (turn * PF_SIDE_ANGLE + (turn < 0 ? -32768 : 32768)) / 65536;
    si->cy = 0;
    si->distance = 0;
}

static void coverflow_place(void)
{
    int i;

    flow_place(&center_slide, center_index);
    for (i = 0; i < ALBUM_COVERS_NUM_SLIDES; i++) {
        flow_place(&left_slides[i], center_index - 1 - i);
        flow_place(&right_slides[i], center_index + 1 + i);
    }
}

static void coverflow_idle(void)
{
    int i;

    slide_frame = center_index << 16;
    for (i = 0; i < FLOW_RING; i++)
        flow_turns[i].index = INT_MIN;
    for (i = -ALBUM_COVERS_NUM_SLIDES; i <= ALBUM_COVERS_NUM_SLIDES; i++)
        flow_turn_of(center_index + i, slide_frame);
    coverflow_place();
}

/* flow_update() for one refresh of dt_us, with move_center() reduced to
 * the index it sets. True once the covers have come to rest. */
static bool flow_refresh(uint32_t dt_us)
{
    int to = target << 16;
    bool rest = false;
    uint32_t left = dt_us;

    while (left > 0)
    {
        uint32_t step = MIN(left, (uint32_t)FLOW_STEP_US);

        slide_frame = flow_chase(slide_frame, to, step);
        rest = flow_turn_step(step) && slide_frame == to;
        left -= step;
    }
    center_index = (slide_frame + 0x8000) >> 16;
    if (rest)
        coverflow_idle();
    else
        coverflow_place();
    return rest;
}

/* flow_click() */
static void flow_click(int direction)
{
    int to = target + direction;
    int ahead = (to << 16) - slide_frame;

    if ((direction < 0 ? -ahead : ahead) > FLOW_LEAD_MAX << 16)
        return;
    target = to;
}

static int slide_x_range(struct slide_data *slide, int *x0, int *x1)
{
    struct dim *bmp = surface(slide);
    PFreal cosr, sinr, zo, xs, xp;
    int sw, lo, hi;

    if (!bmp || slide->angle > 255 || slide->angle < -255)
        return 0;

    sw = bmp->width;
    cosr = fcos(slide->angle);
    sinr = fsin(slide->angle);
    zo = PFREAL_ONE * slide->distance
       - fmuln(MAXSLIDE_LEFT_R, fabsr(sinr), PFREAL_SHIFT - 2, 0);

    xs = -sw * PFREAL_HALF + PFREAL_HALF;
    xp = fdiv(CAM_DIST * (slide->cx + fmul(xs, cosr)),
              CAM_DIST_R + zo + fmul(xs, sinr));
    lo = (fmax(DISPLAY_LEFT_R, xp) - DISPLAY_LEFT_R + PFREAL_ONE - 1)
         >> PFREAL_SHIFT;

    xs = sw * PFREAL_HALF;
    xp = fdiv(CAM_DIST * (slide->cx + fmul(xs, cosr)),
              CAM_DIST_R + zo + fmul(xs, sinr));
    hi = (xp - DISPLAY_LEFT_R + PFREAL_ONE - 1) >> PFREAL_SHIFT;

    if (lo < 0) lo = 0;
    if (hi > LCD_WIDTH) hi = LCD_WIDTH;
    *x0 = lo; *x1 = hi;
    return lo < hi;
}

static int slide_dy_at(struct slide_data *slide, int x)
{
    struct dim *bmp = surface(slide);
    PFreal cosr, sinr, zo, xp, xsnum, xsden, xs;
    PFreal slide_left;

    if (!bmp)
        return 0;

    cosr = fcos(slide->angle);
    sinr = fsin(slide->angle);
    zo = PFREAL_ONE * slide->distance
       - fmuln(MAXSLIDE_LEFT_R, fabsr(sinr), PFREAL_SHIFT - 2, 0);

    xp = DISPLAY_LEFT_R + x * PFREAL_ONE;
    xsnum = CAM_DIST * (slide->cx - xp)
          - fmuln(xp, zo, PFREAL_SHIFT - 2, 0);
    xsden = fmuln(xp, sinr, PFREAL_SHIFT - 2, 0) - CAM_DIST * cosr;
    xs = fdiv(xsnum, xsden);

    slide_left = -bmp->width * PFREAL_HALF + PFREAL_HALF;
    if (xs < slide_left)
        xs = slide_left;

    return (CAM_DIST_R + zo + fmul(xs, sinr)) / CAM_DIST;
}

/* the every-column version, which is what the sampled one is checked against */
static int exhaustive;
static int slide_voff(int sh, PFreal cy);

static int slide_covers(struct slide_data *near, struct slide_data *far,
                        int a, int b)
{
    struct dim n, f;
    n = *surface(near); f = *surface(far);
    if (b < a || n.height != f.height || near->cy != far->cy)
        return 0;
    int split = pf_half_height - slide_voff(n.height, near->cy);
    int half = MIN(split, n.height - split);
    int points = exhaustive ? b - a + 1 : 3;
    for (int i = 0; i < points; i++) {
        int x = exhaustive ? a + i : (i == 0 ? a : (i == 1 ? (a+b)/2 : b));
        int fd = slide_dy_at(far, x);
        if (!near->angle && !near->distance) {
            if (fd < PFREAL_ONE) return 0;
        } else {
            int nd = slide_dy_at(near, x);
            if (half <= 0 || fd <= nd ||
                (int64_t)half * PFREAL_ONE * (fd-nd) < (int64_t)nd*fd)
                return 0;
        }
    }
    return 1;
}

static void cull_side(struct slide_data *side, const int *alpha,
                      int center_alpha, int *edge_out, int right)
{
    struct slide_data *cover[ALBUM_COVERS_NUM_SLIDES + 1];
    int cover_lo[ALBUM_COVERS_NUM_SLIDES + 1];
    int cover_hi[ALBUM_COVERS_NUM_SLIDES + 1];
    int n_cover = 0;
    int i, k, x0, x1, lo, hi;
    int edge = 0, span = LCD_WIDTH;

    if (center_alpha > 0 && slide_x_range(&center_slide, &x0, &x1))
    {
        lo = (right ? x0 : LCD_WIDTH - x1) + PF_CULL_MARGIN;
        hi = (right ? x1 : LCD_WIDTH - x0) - PF_CULL_MARGIN;
        if (hi > lo)
        {
            cover[0] = &center_slide;
            cover_lo[0] = lo;
            cover_hi[0] = hi;
            n_cover = 1;
            span = lo;
            edge = hi;
        }
    }

    for (i = 0; i < ALBUM_COVERS_NUM_SLIDES; i++)
    {
        edge_out[i] = right ? 0 : LCD_WIDTH;

        if (alpha[i] <= 0)
            continue;
        if (!slide_x_range(&side[i], &x0, &x1))
            continue;
        lo = right ? x0 : LCD_WIDTH - x1;
        hi = right ? x1 : LCD_WIDTH - x0;

        if (n_cover && lo - PF_CULL_MARGIN >= span)
        {
            int reached = lo - PF_CULL_MARGIN;

            for (k = 0; k < n_cover; k++)
            {
                int last;

                if (cover_hi[k] <= reached)
                    continue;
                if (cover_lo[k] > reached)
                    break;

                last = MIN(cover_hi[k], MIN(hi, edge)) - 1;
                if (last < reached)
                    break;
                if (!slide_covers(cover[k], &side[i],
                                  right ? reached : PF_CULL_MIRROR(last),
                                  right ? last : PF_CULL_MIRROR(reached)))
                    break;
                reached = last + 1;
            }
            if (reached > lo)
                edge_out[i] = right ? reached : LCD_WIDTH - reached;
        }

        lo += PF_CULL_MARGIN;
        hi -= PF_CULL_MARGIN;
        if (hi <= lo)
            continue;

        if (lo > edge)
        {
            n_cover = 0;
            span = lo;
            edge = hi;
        }
        else if (hi > edge)
        {
            edge = hi;
            span = MIN(span, lo);
        }
        else
            continue;

        cover[n_cover] = &side[i];
        cover_lo[n_cover] = lo;
        cover_hi[n_cover] = hi;
        n_cover++;
    }
}

static int slide_voff(int sh, PFreal cy)
{
    int voff = ((pf_half_height + pf_lower_half) - sh) / 2;

    if (cy > 0)
        voff += cy >> PFREAL_SHIFT;
    if (voff > pf_half_height)
        voff = pf_half_height;
    return voff;
}

static void slide_rows(struct slide_data *slide, int *y0, int *y1)
{
    struct dim *bmp = surface(slide);
    int voff, sh;

    if (!bmp)
    {
        *y0 = pf_half_height + pf_lower_half;
        *y1 = 0;
        return;
    }
    sh = bmp->height;
    voff = slide_voff(sh, slide->cy);
    *y0 = MAX(0, voff - PF_ROW_MARGIN);
    *y1 = MIN(pf_half_height + pf_lower_half, voff + sh + PF_ROW_MARGIN);
}

/* carousel.c's cover_rows() without the union against the previous frame --
 * what the clear and the flush are sized from. */
static void cover_rows(int *y0, int *y1)
{
    int lo, hi, a, b, i;

    slide_rows(&center_slide, &lo, &hi);
    for (i = 0; i < ALBUM_COVERS_NUM_SLIDES; i++) {
        slide_rows(&left_slides[i], &a, &b);
        lo = MIN(lo, a); hi = MAX(hi, b);
        slide_rows(&right_slides[i], &a, &b);
        lo = MIN(lo, a); hi = MAX(hi, b);
    }
    *y0 = lo; *y1 = hi;
}

/* ======================= END CAROUSEL MIRROR =========================== */

/* Stands in for the framebuffer: which slide wrote each pixel last. Two renders
 * of the same frame have to agree on this everywhere -- the topmost writer is
 * what decides the colour, so anything else is a visible artefact. */
#define FBH 256
static short fb_top[LCD_WIDTH * FBH];
static short fb_ref[LCD_WIDTH * FBH];
static int fb_mark_id;

/* render_slide_clipped()'s column walk, counting and stamping instead of
 * blending. Everything that decides *which* pixels are written is copied; the
 * blend itself is not, because the cull cannot change a colour, only whether a
 * pixel is reached. */
static long render_slide_clipped(struct slide_data *slide, int x_from, int x_to)
{
    struct dim *bmp = surface(slide);
    long px = 0;

    if (!bmp || slide->angle > 255 || slide->angle < -255)
        return 0;
    if (x_from >= x_to)
        return 0;

    const int sw = bmp->width, sh = bmp->height;
    const PFreal slide_left = -sw * PFREAL_HALF + PFREAL_HALF;
    const int w = LCD_WIDTH;

    PFreal cosr = fcos(slide->angle);
    PFreal sinr = fsin(slide->angle);
    PFreal zo = PFREAL_ONE * slide->distance
              - fmuln(MAXSLIDE_LEFT_R, fabsr(sinr), PFREAL_SHIFT - 2, 0);

    PFreal xs = slide_left, xsnum, xsnumi, xsden, xsdeni;
    PFreal xp = fdiv(CAM_DIST * (slide->cx + fmul(xs, cosr)),
                     (CAM_DIST_R + zo + fmul(xs, sinr)));
    int xi = (fmax(DISPLAY_LEFT_R, xp) - DISPLAY_LEFT_R + PFREAL_ONE - 1)
             >> PFREAL_SHIFT;
    xp = DISPLAY_LEFT_R + xi * PFREAL_ONE;
    if (xi >= w)
        return 0;
    xsnum = CAM_DIST * (slide->cx - xp)
          - fmuln(xp, zo, PFREAL_SHIFT - 2, 0);
    xsden = fmuln(xp, sinr, PFREAL_SHIFT - 2, 0) - CAM_DIST * cosr;
    xs = fdiv(xsnum, xsden);
    xsnumi = -CAM_DIST_R - zo;
    xsdeni = sinr;

    int x, dy = PFREAL_ONE;
    const int half_height = pf_half_height, lower_half = pf_lower_half;
    const int perspective = (zo != 0 || slide->angle != 0);
    int voff = ((half_height + lower_half) - sh) / 2;
    if (slide->cy > 0) voff += slide->cy >> PFREAL_SHIFT;
    if (voff > half_height) voff = half_height;
    const int p_start_upper = (half_height - 1 - voff) * PFREAL_ONE;
    const int p_start_lower = (half_height - voff) * PFREAL_ONE;

    for (x = xi; x < x_to; x++) {
        if (xs < slide_left) xs = slide_left;
        int column = (unsigned)(xs - slide_left) >> PFREAL_SHIFT;
        if (column >= sw) break;
        if (x < x_from) {
            int k = x_from - x;
            if (perspective) {
                xsnum += k * xsnumi; xsden += k * xsdeni;
                xs = fdiv(xsnum, xsden);
            } else
                xs += k * PFREAL_ONE;
            x = x_from - 1;
            continue;
        }
        if (perspective) dy = div_cam_dist(CAM_DIST_R + zo + fmul(xs, sinr));

        if (slide->angle) {
            int split = MIN(p_start_lower, sh * PFREAL_ONE);
            for (int side = 0; side < 2; side++) {
                int extent = side ? sh * PFREAL_ONE - split : split;
                int full = extent / dy;
                unsigned int recip = 0xffffffffu / dy;
                int cover = (((uint64_t)(extent % dy) * recip >> 24) + 2) & 0x1fc;
                int limit = side ? lower_half : half_height;
                for (int k=0; k<limit && k<full+(cover>0); k++) {
                    int y = side ? half_height+k : half_height-1-k;
                    short *pixel = &fb_top[y*LCD_WIDTH+x];
                    /* Keep a fingerprint of the background under a partial row. */
                    *pixel = k==full && cover<256 ?
                        (short)(1 + (((unsigned short)*pixel * 31u + fb_mark_id) % 32766)) :
                        fb_mark_id;
                    px++;
                }
            }
        } else
        {
            int p = p_start_upper;
            int plim = MAX(0, p - (half_height - 1) * dy);
            int y = half_height - 1;
            while (p >= plim) {
                px++;
                if (y >= 0 && y < FBH) fb_top[y*LCD_WIDTH + x] = fb_mark_id;
                p -= dy; y--;
            }
            p = p_start_lower;
            plim = MIN(sh * PFREAL_ONE, p + lower_half * dy);
            y = half_height;
            while (p < plim) {
                px++;
                if (y >= 0 && y < FBH) fb_top[y*LCD_WIDTH + x] = fb_mark_id;
                p += dy; y++;
            }
        }
        if (perspective) { xsnum += xsnumi; xsden += xsdeni; xs = fdiv(xsnum, xsden); }
        else xs += PFREAL_ONE;
    }
    return px;
}

/* coverflow_render(), with the cull switchable off. */
static long render_frame(int with_cull)
{
    const int n = ALBUM_COVERS_NUM_SLIDES;
    int alpha[MAX_SLIDES_COUNT];
    int left_to[MAX_SLIDES_COUNT], right_from[MAX_SLIDES_COUNT];
    int index;
    long px = 0;

    for (index = 0; index < n; index++)
        alpha[index] = 256;

    if (with_cull) {
        cull_side(left_slides, alpha, 256, left_to, 0);
        cull_side(right_slides, alpha, 256, right_from, 1);
    } else {
        for (index = 0; index < n; index++)
            { left_to[index] = LCD_WIDTH; right_from[index] = 0; }
    }

    for (index = n - 1; index >= 0; index--) {
        fb_mark_id = 1 + index;
        px += render_slide_clipped(&left_slides[index], 0, left_to[index]);
        fb_mark_id = 11 + index;
        px += render_slide_clipped(&right_slides[index],
                                   right_from[index], LCD_WIDTH);
    }
    fb_mark_id = 21;
    px += render_slide_clipped(&center_slide, 0, LCD_WIDTH);
    return px;
}

/* The motion's state at one refresh, recorded from the scenarios below and
 * played back under every configuration: the motion does not depend on the
 * settings that move the slots. */
struct pose
{
    int frame, target, center;
    struct flow_turn turns[FLOW_RING];
    int scenario;
};
#define MAX_POSES 8192
static struct pose poses[MAX_POSES];
static int n_poses;

static void record(int scenario)
{
    struct pose *q;

    if (n_poses >= MAX_POSES)
        return;
    q = &poses[n_poses++];
    q->frame = slide_frame;
    q->target = target;
    q->center = center_index;
    memcpy(q->turns, flow_turns, sizeof flow_turns);
    q->scenario = scenario;
}

static void pose(const struct pose *q)
{
    slide_frame = q->frame;
    target = q->target;
    center_index = q->center;
    memcpy(flow_turns, q->turns, sizeof flow_turns);
    coverflow_place();
}

/* One refresh of the panel at 66.9 Hz. */
#define REFRESH_US 14950

/* Clicks at `rate` a second for `secs`, `dir` at a time, then nothing until
 * the covers rest; `back_after` > 0 reverses the clicks after that long. */
static void scenario(int id, double rate, double secs, int dir,
                     double back_after)
{
    double t = 0, next = 0;
    int guard = 0;

    center_index = target = 500;
    coverflow_idle();
    for (;;) {
        t += REFRESH_US / 1e6;
        while (next < secs && t >= next) {
            flow_click(back_after > 0 && t > back_after ? -dir : dir);
            next += 1.0 / rate;
        }
        if (flow_refresh(REFRESH_US) && t >= secs)
            break;
        record(id);
        if (++guard > 2000)
            break;
    }
}

static const char *const SCENARIO_NAME[] = {
    "one click", "3 clicks/s", "10 clicks/s", "20 clicks/s",
    "48 clicks/s (wheel faster than the row)", "reversal at 20 clicks/s",
    "random clicks",
};
#define N_SCENARIOS 7

static void record_scenarios(void)
{
    unsigned seed = 777;

    scenario(0, 1, 0.01, 1, 0);
    scenario(1, 3, 1.5, 1, 0);
    scenario(2, 10, 1.5, -1, 0);
    scenario(3, 20, 1.5, 1, 0);
    scenario(4, 48, 1.5, -1, 0);
    scenario(5, 20, 1.6, 1, 0.8);

    center_index = target = 500;
    coverflow_idle();
    for (int f = 0; f < 1500; f++) {
        seed = seed * 1103515245u + 12345u;
        if ((seed >> 16) % 100 < 30)
            flow_click(((seed >> 8) & 1) ? 1 : -1);
        flow_refresh(REFRESH_US * (1 + (int)((seed >> 4) % 2)));
        record(6);
    }
}

/* Rows the frame in fb_top actually wrote to. */
static void written_rows(int *y0, int *y1)
{
    int y, x, lo = -1, hi = -1;

    for (y = 0; y < FBH; y++)
        for (x = 0; x < LCD_WIDTH; x++)
            if (fb_top[y*LCD_WIDTH + x]) {
                if (lo < 0) lo = y;
                hi = y + 1;
                break;
            }
    *y0 = (lo < 0) ? 0 : lo;
    *y1 = (hi < 0) ? 0 : hi;
}

/* Rows the clear and the flush are sized from must contain every row the render
 * wrote, or a cover comes out with a band of it missing. */
static long long rows_short;
static int rows_over_max;
static long long rows_band, rows_view, rows_frames;

static void check_rows(void)
{
    int by0, by1, wy0, wy1;

    cover_rows(&by0, &by1);
    written_rows(&wy0, &wy1);

    if (wy1 > wy0 && (wy0 < by0 || wy1 > by1)) {
        int over = MAX(by0 - wy0, wy1 - by1);
        rows_short++;
        if (over > rows_over_max) rows_over_max = over;
    }
    rows_band += (by1 > by0) ? by1 - by0 : 0;
    rows_view += pf_half_height + pf_lower_half;
    rows_frames++;
}

/* One frame, both ways round. Returns pixels the cull saved; *bad gets the
 * count of pixels the two renders disagree on, which must be zero. */
static long check_frame(const struct pose *p, long *plain, long long *bad)
{
    long culled;
    int i;

    pose(p);
    memset(fb_top, 0, sizeof fb_top);
    *plain = render_frame(0);
    memcpy(fb_ref, fb_top, sizeof fb_top);

    pose(p);
    memset(fb_top, 0, sizeof fb_top);
    culled = render_frame(1);
    check_rows();

    for (i = 0; i < LCD_WIDTH * FBH; i++)
        if (fb_ref[i] != fb_top[i]) (*bad)++;

    return *plain - culled;
}

/* The render divides by multiplying (div_cam_dist(), div_by_recip()), and a
 * quotient one out moves a row or a column, so both are checked against plain
 * division over everything the render can hand them -- and well past it:
 * dy's numerator stays under 2^19, a row count's under 2^18, and dy itself
 * between 1024 and about 1600. Returns the number of wrong quotients. */
static long long check_divisions(void)
{
    long long bad = 0;
    unsigned int n, d;

    for (n = 0; n < (1u << 24); n++)
        if (div_cam_dist(n) != (int)(n / CAM_DIST))
            bad++;
    for (d = 1; d < 4096; d++) {
        unsigned int recip = 0xffffffffu / d;
        for (n = 0; n < (1u << 18); n++)
            if (div_by_recip(n, d, recip) != (int)(n / d))
                bad++;
    }
    return bad;
}

/* -l: where each cover sits at rest with the default settings (centre margin
 * 20 from default-config.cfg, slide tuck 32), drawn alone: its columns, and
 * its height at the inner and outer one. */
static int column_height(int x)
{
    int y, n = 0;

    for (y = 0; y < FBH; y++)
        if (fb_top[y * LCD_WIDTH + x])
            n++;
    return n;
}

static void print_layout(void)
{
    set_center_margin = 20;
    set_slide_tuck = 32;
    pf_half_height = 120;
    pf_lower_half = 120;
    recalc_offsets();
    printf("first slot %.1f world, stack spacing %.1f world, side turn "
           "%.1f deg\n", offsetX / (double)PFREAL_ONE,
           stack_spacing / (double)PFREAL_ONE,
           PF_SIDE_ANGLE * 360.0 / IANGLE_MAX);
    for (int i = 0; i < ALBUM_COVERS_NUM_SLIDES; i++)
        right_slides[i].sw = right_slides[i].sh =
        left_slides[i].sw = left_slides[i].sh = 128;
    center_slide.sw = center_slide.sh = 128;
    center_index = target = 500;
    coverflow_idle();
    for (int i = -ALBUM_COVERS_NUM_SLIDES; i <= ALBUM_COVERS_NUM_SLIDES; i++) {
        struct slide_data *si = i == 0 ? &center_slide
                              : i < 0 ? &left_slides[-i - 1]
                              : &right_slides[i - 1];
        int x0 = -1, x1 = -1;

        memset(fb_top, 0, sizeof fb_top);
        fb_mark_id = 1;
        render_slide_clipped(si, 0, LCD_WIDTH);
        for (int x = 0; x < LCD_WIDTH; x++)
            if (column_height(x)) {
                if (x0 < 0) x0 = x;
                x1 = x + 1;
            }
        if (x0 < 0)
            printf("  slot %d: off screen\n", i);
        else
            printf("  slot %d: x %d-%d (%d wide), height %d inner, "
                   "%d outer\n", i, x0, x1, x1 - x0,
                   column_height(i < 0 ? x1 - 1 : x0),
                   column_height(i < 0 ? x0 : x1 - 1));
    }
}

/* ---- The scene in 3D ----------------------------------------------------
 * The render is a painter: it draws the covers in a fixed order and the last
 * one over a pixel wins. Seen from above, each cover is a segment; the
 * painter is right in a column when the cover it left on top is the nearest
 * one the column's ray meets. These count the columns where it is not, and
 * the neighbouring covers whose segments cross -- which no draw order can
 * show correctly. Floating point, with square covers. */
struct seg { double x0, z0, x1, z1; int id; };

static struct seg segment(struct slide_data *s, int id)
{
    double a = s->angle * 2 * M_PI / IANGLE_MAX;
    double c = cos(a), sn = sin(a);
    double cx = s->cx / (double)PFREAL_ONE;
    double z = CAM_DIST + s->distance + 63.5 * fabs(sn);
    struct seg g = { cx - 63.5 * c, z - 63.5 * sn, cx + 64 * c, z + 64 * sn,
                     id };
    return g;
}

/* Depth at which the ray through screen column centre x meets g, or -1. */
static double ray_depth(const struct seg *g, double x)
{
    double t = (x + 0.5 - LCD_WIDTH / 2.0) / CAM_DIST;
    double dx = g->x1 - g->x0, dz = g->z1 - g->z0;
    double den = dx - t * dz;
    double u;

    if (fabs(den) < 1e-9)
        return -1;
    u = (t * g->z0 - g->x0) / den;
    if (u < 0 || u > 1)
        return -1;
    return g->z0 + u * dz;
}

static bool segs_cross(const struct seg *a, const struct seg *b)
{
#define ORIENT(p, q, r) ((q##x - p##x) * (r##z - p##z) \
                         - (q##z - p##z) * (r##x - p##x))
    double ax = a->x0, az = a->z0, bx = a->x1, bz = a->z1;
    double cx = b->x0, cz = b->z0, dx = b->x1, dz = b->z1;
    double d1 = ORIENT(c, d, a), d2 = ORIENT(c, d, b);
    double d3 = ORIENT(a, b, c), d4 = ORIENT(a, b, d);
#undef ORIENT
    return d1 * d2 < -1e-6 && d3 * d4 < -1e-6;
}

static struct
{
    long long frames, crossed_frames, wrong_frames, wrong_cols;
    int wrong_max;
} scene[N_SCENARIOS];

/* After render_frame(0) of the pose: what the painter left on the middle row
 * against what is nearest. A column only counts where the two depths differ
 * by more than a world pixel, so covers meeting edge to edge do not. */
static void check_scene(int sc)
{
    struct seg g[2 * ALBUM_COVERS_NUM_SLIDES + 1];
    int n = 0, wrong = 0, crossed = 0;

    g[n++] = segment(&center_slide, 21);
    for (int k = 0; k < ALBUM_COVERS_NUM_SLIDES; k++) {
        g[n++] = segment(&left_slides[k], 1 + k);
        g[n++] = segment(&right_slides[k], 11 + k);
    }
    for (int a = 0; a < n; a++)
        for (int b = a + 1; b < n; b++)
            if (segs_cross(&g[a], &g[b]))
                crossed++;

    for (int x = 0; x < LCD_WIDTH; x++) {
        int top = fb_ref[pf_half_height * LCD_WIDTH + x];
        double best = -1, top_depth = -1;

        for (int k = 0; k < n; k++) {
            double d = ray_depth(&g[k], x);
            if (d < 0)
                continue;
            if (best < 0 || d < best)
                best = d;
            if (g[k].id == top)
                top_depth = d;
        }
        if (top_depth >= 0 && top_depth > best + 1)
            wrong++;
    }
    scene[sc].frames++;
    scene[sc].crossed_frames += crossed > 0;
    scene[sc].wrong_frames += wrong > 0;
    scene[sc].wrong_cols += wrong;
    if (wrong > scene[sc].wrong_max)
        scene[sc].wrong_max = wrong;
}

int main(int argc, char **argv)
{
    /* Cover shapes: covers are scaled into DISPLAY_WIDTH x DISPLAY_HEIGHT
     * keeping their aspect, so a square one comes out 128x128 and anything
     * else does not -- which is the case the cull's height test is for. */
    static const int SW_SET[] = { 128, 128, 128, 100,  80, 128 };
    static const int SH_SET[] = { 128,  64,  96, 128, 160,  40 };
    static const int TUCK[]   = { 0, 8, 32, 64 };          /* setting: 0..64 */
    static const int MARGIN[] = { 0, 20, 28, 80 };         /* setting: 0..80 */
    static const int SBS[]    = { 0, 20, 40 };             /* status bar      */
    static const int BAND[]   = { 0, 30, 49, 70 };         /* caption band    */
    const int PER_CONFIG = 160;

    long long saved = 0, plain_total = 0, bad = 0;
    long long frames = 0, configs = 0;
    unsigned seed = 12345;
    int verbose = (argc > 1 && !strcmp(argv[1], "-v"));

    if (argc > 1 && !strcmp(argv[1], "-l")) {
        print_layout();
        return 0;
    }

    long long div_bad = check_divisions();
    printf("division by multiplying: %lld wrong quotients%s\n", div_bad,
           div_bad ? "  *** FAILED ***" : "  (pass)");

    exhaustive = (argc > 1 && !strcmp(argv[1], "-x"));
    if (exhaustive)
        printf("comparing every column rather than sampling three\n");

    record_scenarios();
    printf("%d refreshes recorded from %d scenarios\n", n_poses, N_SCENARIOS);

    /* The scene at the first-boot settings, every recorded refresh. */
    set_center_margin = 20;
    set_slide_tuck = 32;
    pf_half_height = 120;
    pf_lower_half = 120;
    recalc_offsets();
    for (int k = 0; k < ALBUM_COVERS_NUM_SLIDES; k++)
        left_slides[k].sw = left_slides[k].sh =
        right_slides[k].sw = right_slides[k].sh = 128;
    center_slide.sw = center_slide.sh = 128;
    for (int q = 0; q < n_poses; q++) {
        pose(&poses[q]);
        memset(fb_top, 0, sizeof fb_top);
        render_frame(0);
        memcpy(fb_ref, fb_top, sizeof fb_top);
        check_scene(poses[q].scenario);
    }
    printf("\nthe scene, first-boot settings (refreshes with crossing "
           "covers; with the painter wrong somewhere, and its worst):\n");
    for (int sc = 0; sc < N_SCENARIOS; sc++)
        printf("  %-40s %5lld refreshes  crossing %4lld  painter wrong "
               "%4lld, worst %d columns\n", SCENARIO_NAME[sc],
               scene[sc].frames, scene[sc].crossed_frames,
               scene[sc].wrong_frames, scene[sc].wrong_max);

    /* The cull and the band, across the settings, a share of the recorded
     * refreshes each, a different share each time. */
    for (unsigned t = 0; t < sizeof TUCK / sizeof *TUCK; t++)
    for (unsigned g = 0; g < sizeof MARGIN / sizeof *MARGIN; g++)
    for (unsigned s = 0; s < sizeof SBS / sizeof *SBS; s++)
    for (unsigned b = 0; b < sizeof BAND / sizeof *BAND; b++)
    for (int shapes = 0; shapes < 3; shapes++)
    {
        int draw_h = (LCD_HEIGHT - SBS[s]) - BAND[b];
        if (draw_h < 40)
            continue;

        set_slide_tuck = TUCK[t];
        set_center_margin = MARGIN[g];
        pf_half_height = draw_h / 2;
        pf_lower_half = draw_h - pf_half_height;
        recalc_offsets();

        for (int k = 0; k < ALBUM_COVERS_NUM_SLIDES; k++) {
            int il = 0, ir = 0;
            if (shapes == 1) il = ir = 1;               /* all the same, wide */
            if (shapes == 2) {                          /* a mixture */
                seed = seed * 1103515245u + 12345u; il = (seed >> 16) % 6;
                seed = seed * 1103515245u + 12345u; ir = (seed >> 16) % 6;
            }
            left_slides[k].sw  = SW_SET[il]; left_slides[k].sh  = SH_SET[il];
            right_slides[k].sw = SW_SET[ir]; right_slides[k].sh = SH_SET[ir];
        }
        {
            int ic = (shapes == 2) ? ((seed >> 8) % 6) : (shapes == 1);
            center_slide.sw = SW_SET[ic]; center_slide.sh = SH_SET[ic];
        }

        for (int k = 0; k < PER_CONFIG; k++) {
            const struct pose *q =
                &poses[(configs * 7 + (long long)k * n_poses / PER_CONFIG)
                       % n_poses];
            long plain, was_bad = bad;

            saved += check_frame(q, &plain, &bad);
            plain_total += plain;
            frames++;
            if (bad != was_bad || verbose)
                printf("%s tuck=%d margin=%d sbs=%d band=%d shapes=%d "
                       "frame=%d target=%d: %lld stale pixels\n",
                       bad != was_bad ? "ARTEFACT" : "ok", TUCK[t],
                       MARGIN[g], SBS[s], BAND[b], shapes, q->frame,
                       q->target, bad - was_bad);
        }
        configs++;
    }

    printf("\n%lld configurations, %lld frames\n", configs, frames);
    printf("unculled  %lld pixels/frame\n", plain_total / frames);
    printf("culled    %lld pixels/frame  (%lld%% less)\n",
           (plain_total - saved) / frames, saved * 100 / plain_total);
    printf("stale pixels: %lld%s\n", bad, bad ? "  *** FAILED ***" : "  (pass)");
    printf("divisions by zero: %lld%s\n", fdiv_zero,
           fdiv_zero ? "  *** FAILED ***" : "  (pass)");

    /* The clear and the flush are sized from cover_rows() rather than the whole
     * viewport, so that band has to contain everything the render wrote. */
    printf("\ncover band  %lld%% of the drawable height "
           "(what the clear and flush cost instead of all of it)\n",
           rows_band * 100 / rows_view);
    printf("frames written outside the band: %lld, worst overshoot %d rows%s\n",
           rows_short, rows_over_max,
           rows_short ? "  *** FAILED ***" : "  (pass)");

    return (bad || rows_short || div_bad || fdiv_zero) ? 1 : 0;
}
