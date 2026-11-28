/* PodBox Fruit Company Ultra Cover Flow. GPL version 2+.
 *
 * What Fruit Company Ultra adds to the stock Cover Flow wherever it moves:
 *   - reflections with a smooth falloff and a faint floor gloss, built once
 *     per cover like stock's
 *   - depth light: side covers fall toward the ground with distance, along
 *     each column, through the renderer's existing per-pixel fade
 *   - the scroll's landing on a short overshoot (stock_motion.h)
 *   - covers flying in from depth on entry and receding on leave
 *   - the album turn's lamp: shade and a sweeping highlight, over a still
 *     copy of the covers behind it
 *   - Now Playing's sheet over a receding, dimmed Cover Flow
 * The carousel calls in only behind ultra_flow_on. */
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "system.h"
#include "core_alloc.h"
#include "ui/native_theme_runtime.h"
#include "ui/ultra.h"
#include "reflection.h"
#include "stock_color.h"
#include "ultra_flow.h"

#define ONE UI_ULTRA_ONE

bool ultra_flow_on;

bool ultra_flow_sync(void)
{
    bool on = ui_theme_ultra();
    if (on == ultra_flow_on)
        return false;
    ultra_flow_on = on;
    return true;
}

/* Reflection: the art keeps REFL_ART/256 at the seam and falls as the
 * square of the distance left, a glossier floor than stock's long tail.
 * Its first REFL_GLOSS_ROWS/16 also take up to REFL_GLOSS/256 of the
 * colour opposite the floor: the contact sheen where cover meets floor. */
#define REFL_ART 160
#define REFL_GLOSS 40
#define REFL_GLOSS_ROWS 1

void ultra_reflection_build_rows(fb_data *out, const fb_data *cover, int size,
                                 int stride, fb_data bg, int first, int end)
{
    int rows = reflection_rows(size);
    int gloss_rows = MAX(1, rows * REFL_GLOSS_ROWS / 16);
    int br = bg >> 11, bgr = (bg >> 5) & 63, bb = bg & 31;
    /* Opposite the floor: black under a light floor, white under a dark. */
    int sheen = br + bgr / 2 + bb > 46 ? 0 : 1;
    for (int y = first; y < end; y++)
    {
        int left = 256 - y * 256 / rows;
        int lit = MAX(0, 256 - y * 256 / gloss_rows);
        unsigned a = REFL_ART * left * left >> 16;
        unsigned g = REFL_GLOSS * lit >> 8;
        const fb_data *src = cover + size - 1 - y;
        for (int x = 0; x < size; x++, src += stride)
        {
            unsigned c = *src;
            int r = ((c >> 11) * a + br * (256 - a)) >> 8;
            int gr = (((c >> 5) & 63) * a + bgr * (256 - a)) >> 8;
            int b = ((c & 31) * a + bb * (256 - a)) >> 8;
            r += ((31 * sheen - r) * (int)g) >> 8;
            gr += ((63 * sheen - gr) * (int)g) >> 8;
            b += ((31 * sheen - b) * (int)g) >> 8;
            out[x * rows + y] = r << 11 | gr << 5 | b;
        }
    }
}

/* Flight. A cover enters from FLY_DEPTH units deeper, FLY_DROP lower and
 * FLY_OUT + FLY_SPREAD per slot farther out, its wall turned FLY_TURN
 * degrees toward the viewer, on ui_ultra_back() (the turn on
 * ui_ultra_out(), which cannot pass the wall's edge-on stop). Walls start
 * ENTER_STAGGER_US apart outward from the centre; a leave runs the same
 * offsets on a cubic ease-in, outermost first. */
#define FLY_DEPTH 150
#define FLY_DROP 14
#define FLY_OUT 26
#define FLY_SPREAD 12
#define FLY_TURN 58
#define FLY_FOG 230
#define ENTER_US 340000u
#define ENTER_STAGGER_US 30000u
#define LEAVE_US 210000u
#define LEAVE_STAGGER_US 12000u
#define STAGGER_SLOTS 4

uint32_t ultra_flow_length(bool leave)
{
    return leave ? LEAVE_US + STAGGER_SLOTS * LEAVE_STAGGER_US
                 : ENTER_US + STAGGER_SLOTS * ENTER_STAGGER_US;
}

static int progress(uint32_t elapsed, uint32_t delay, uint32_t length)
{
    if (elapsed <= delay)
        return 0;
    elapsed -= delay;
    return elapsed >= length ? ONE : (int)(elapsed * ONE / length);
}

/* How far a cover still is from its place, Q10: ONE before the entry and
 * after the leave. `turn` asks for the curve that never overshoots. */
static int away(bool leave, int rel, uint32_t elapsed, bool turn)
{
    int slot = MIN(abs(rel), STAGGER_SLOTS);
    if (leave)
    {
        int u = progress(elapsed, (STAGGER_SLOTS - slot) * LEAVE_STAGGER_US,
                         LEAVE_US);
        return ONE - ui_ultra_out(ONE - u);
    }
    int u = progress(elapsed, slot * ENTER_STAGGER_US, ENTER_US);
    return ONE - (turn ? ui_ultra_out(u) : ui_ultra_back(u));
}

struct stock_pose ultra_flow_pose(bool leave, int rel, struct stock_pose p,
                                  uint32_t elapsed)
{
    int side = (rel > 0) - (rel < 0);
    int w = away(leave, rel, elapsed, false);
    int slot = MIN(abs(rel), STAGGER_SLOTS);
    /* units * Q16 * w / ONE */
    p.x += side * (FLY_OUT + FLY_SPREAD * slot) * w * (STOCK_ONE / ONE);
    p.y += FLY_DROP * w * (STOCK_ONE / ONE);
    p.z -= FLY_DEPTH * w * (STOCK_ONE / ONE);
    if (side)
        p.yaw -= side * FLY_TURN * away(leave, rel, elapsed, true) *
                 (STOCK_ONE / ONE);
    return p;
}

/* The scroll lands on ui_ultra_back() over STOCK_ULTRA_SCROLL_US: as quick
 * off the mark as stock's quintic, then a slot's few per cent past the
 * target and back. */
int ultra_flow_scroll(uint32_t elapsed)
{
    if (elapsed >= STOCK_ULTRA_SCROLL_US)
        return STOCK_ONE;
    unsigned u = elapsed / (STOCK_ULTRA_SCROLL_US / 32) * (ONE / 32) +
                 elapsed % (STOCK_ULTRA_SCROLL_US / 32) * (ONE / 32) /
                 (STOCK_ULTRA_SCROLL_US / 32);
    return ui_ultra_back(u) * (STOCK_ONE / ONE);
}

/* Depth light: a cover loses SIDE_FOG/256 by the time it is SIDE_REACH
 * units from the centre, and each column DEPTH_FOG/256 of a level per
 * pixel it is shorter than the centre cover. */
#define SIDE_FOG 30
#define SIDE_REACH 100
#define DEPTH_FOG 150

int ultra_flow_fog(const struct stock_flow *f, int album, uint32_t now,
                   int32_t x)
{
    int distance = MIN(abs(x) >> 16, SIDE_REACH);
    int fog = distance * SIDE_FOG / SIDE_REACH;
    if (f->phase == STOCK_ULTRA_ENTER || f->phase == STOCK_ULTRA_LEAVE)
        fog += away(f->phase == STOCK_ULTRA_LEAVE, album - f->selected,
                    now - f->start_us, true) * FLY_FOG / ONE;
    return fog;
}

unsigned ultra_flow_light(int fog, int height, int centre)
{
    int drop = fog;
    if (height < centre)
        drop += (centre - height) * DEPTH_FOG >> 8;
    if (drop <= 0)
        return 256;
    return drop >= 256 ? 0 : 256 - drop;
}

/* The lamp: edge-on loses LAMP_SHADE/256, and the receding edge a further
 * LAMP_RECEDE/256 in proportion to the turn. The highlight, LAMP_SPEC/256
 * of white at its peak and LAMP_WIDTH/65536 of the face to either side,
 * is brightest at 45 degrees of each quarter turn and crosses the face
 * from near edge to far as that quarter proceeds. */
#define LAMP_SHADE 120
#define LAMP_RECEDE 56
#define LAMP_SPEC 120
#define LAMP_WIDTH 15000

void ultra_lamp_aim(struct ultra_lamp *lamp, struct stock_pose pose,
                    const struct stock_edge edge[2])
{
    int yaw = abs(pose.yaw);
    int s = abs(stock_sin(yaw));
    int c = abs(stock_sin(90 * STOCK_ONE - yaw));
    int quarter = yaw % (90 * STOCK_ONE);
    bool left_first = edge[0].x <= edge[1].x;
    const struct stock_edge *left = &edge[!left_first];
    const struct stock_edge *right = &edge[left_first];
    lamp->shade = (STOCK_ONE - c) * LAMP_SHADE >> 16;
    lamp->recede = s * LAMP_RECEDE >> 16;
    lamp->spec = (int)((int64_t)s * c >> 15) * LAMP_SPEC >> 16;
    lamp->spec_at = -LAMP_WIDTH + (int)((int64_t)quarter *
                    (STOCK_ONE + 2 * LAMP_WIDTH) / (90 * STOCK_ONE));
    lamp->far_left = left->depth > right->depth;
}

bool ultra_lamp_column(const struct ultra_lamp *lamp, int t,
                       struct ultra_lit *lit)
{
    int depth = lamp->far_left ? STOCK_ONE - t : t;
    int drop = lamp->shade + (lamp->recede * depth >> 16);
    int gap = abs((STOCK_ONE - depth) - lamp->spec_at);
    int lift = gap < LAMP_WIDTH ?
               lamp->spec * (LAMP_WIDTH - gap) / LAMP_WIDTH : 0;
    int net = lift - drop;
    if (net > -8 && net < 8)
        return false;
    *lit = ultra_lit(256 - MIN(abs(net), 248), net > 0 ? 0xffff : 0);
    return true;
}

__attribute__((optimize("O2")))
unsigned ultra_lit_run(const fb_data *src, fb_data *dst, int stride, int n,
                       unsigned p, int dp, const struct ultra_lit *lit)
{
    const struct ultra_lit l = *lit;
#if defined(IPOD_6G) && defined(__arm__) && !defined(__thumb__) && !defined(SIMULATOR)
    /* draw_run()'s halfword texel address, with the light between load
     * and store: nine instructions a pixel. MLA's Rd and Rm must differ
     * on ARMv5, so the texel is its Rs. */
    if (n >= 8)
    {
        unsigned at = ((uintptr_t)src >> 1) + (p >> 10);
        unsigned frac = p << 22;
        unsigned blocks = n >> 3;
        unsigned v;
        p += (n & ~7) * (unsigned)dp;
        n &= 7;
        asm volatile(
            "1:\n"
            ".rept 8\n"
            "ldrh %[v], [%[at], %[at]]\n"
            "adds %[frac], %[frac], %[inc]\n"
            "adc %[at], %[at], %[whole]\n"
            "orr %[v], %[v], %[v], lsl #16\n"
            "and %[v], %[v], %[mask]\n"
            "mla %[v], %[a], %[v], %[add]\n"
            "and %[v], %[mask], %[v], lsr #5\n"
            "orr %[v], %[v], %[v], lsr #16\n"
            "strh %[v], [%[dst]], %[stride]\n"
            ".endr\n"
            "subs %[blocks], %[blocks], #1\n"
            "bne 1b"
            : [at] "+&r"(at), [frac] "+&r"(frac), [dst] "+&r"(dst),
              [blocks] "+&r"(blocks), [v] "=&r"(v)
            : [inc] "r"((unsigned)dp << 22), [whole] "r"(dp >> 10),
              [stride] "r"(stride * (int)sizeof(*dst)),
              [mask] "r"(STOCK_COLOR_LANES), [a] "r"(l.a), [add] "r"(l.add)
            : "cc", "memory");
    }
#endif
    for (; n > 0; n--, dst += stride, p += dp)
        *dst = ultra_lit_pixel(src[p >> 10], &l);
    return p;
}

static int still_handle;
static size_t still_pixels;
static uint32_t still_key;

bool ultra_still_restore(fb_data *at, size_t pixels, uint32_t key)
{
    if (still_handle <= 0 || still_pixels != pixels || still_key != key)
        return false;
    const fb_data *copy = core_get_data_pinned(still_handle);
    memcpy(at, copy, pixels * sizeof(*at));
    core_put_data_pinned((void *)copy);
    return true;
}

void ultra_still_keep(const fb_data *at, size_t pixels, uint32_t key)
{
    if (still_pixels != pixels)
        ultra_still_drop();
    if (still_handle <= 0)
        still_handle = core_alloc_free_space(pixels * sizeof(*at));
    if (still_handle <= 0)
        return;
    fb_data *copy = core_get_data_pinned(still_handle);
    memcpy(copy, at, pixels * sizeof(*at));
    core_put_data_pinned(copy);
    still_pixels = pixels;
    still_key = key;
}

void ultra_still_drop(void)
{
    if (still_handle > 0)
        core_free(still_handle);
    still_handle = 0;
    still_pixels = 0;
}

/* The sheet: Now Playing grows from SHEET_FRONT_SHRINK below full size as
 * it rises, Cover Flow shrinks by SHEET_BACK_SHRINK about the centre and
 * keeps (32 - SHEET_DIM)/32 of its light, and SHEET_SHADOW rows above the
 * page's edge darken toward it, to half at the edge. Coverage runs on
 * ui_ultra_out() rising and its mirror sinking. */
#define SHEET_FRONT_SHRINK 5243  /* 0.08, Q16 */
#define SHEET_BACK_SHRINK 4588   /* 0.07, Q16 */
#define SHEET_DIM 15
#define SHEET_SHADOW 12

static inline fb_data dim_pixel(fb_data c, unsigned light)
{
    uint32_t lanes = stock_color_lanes(c);
    lanes = (lanes * light >> 5) & STOCK_COLOR_LANES;
    return lanes | lanes >> 16;
}

/* Columns of a row scaled by 65536/inv about the centre column, and the
 * first column on the source: the span is symmetric. */
static int scale_map(int16_t *map, uint32_t inv)
{
    const int half = LCD_WIDTH / 2;
    int first = half;
    while (first > 0 && half + (((first - 1 - half) * (int)inv) >> 16) >= 0)
        first--;
    for (int x = first; x < LCD_WIDTH - first; x++)
        map[x] = LCD_WIDTH / 2 + (((x - LCD_WIDTH / 2) * (int)inv) >> 16);
    return first;
}

static void black(fb_data *out, int first, int end)
{
    if (end > first)
        memset(out + first, 0, (end - first) * sizeof(*out));
}

__attribute__((optimize("O2")))
static void back_span(fb_data *out, const fb_data *src, const int16_t *map,
                      int first, int end, unsigned light)
{
    for (int x = first; x < end; x++)
        out[x] = dim_pixel(src[map[x]], light);
}

__attribute__((optimize("O2")))
static void front_span(fb_data *out, const fb_data *src, const int16_t *map,
                       int first, int end)
{
    for (int x = first; x < end; x++)
        out[x] = src[map[x]];
}

/* Columns `from` to `to` of a row of the back, whose own columns start at
 * `edge`. */
static void back_part(fb_data *out, const fb_data *src, const int16_t *map,
                      int from, int to, int edge, unsigned light)
{
    int lo = MAX(from, edge), hi = MIN(to, LCD_WIDTH - edge);
    if (!src || lo >= hi)
    {
        black(out, from, to);
        return;
    }
    black(out, from, lo);
    back_span(out, src, map, lo, hi, light);
    black(out, hi, to);
}

void ultra_sheet_compose(fb_data *out, const fb_data *back,
                         const fb_data *front, unsigned frame,
                         unsigned frames, bool rising)
{
    unsigned u = MIN(frame, frames) * ONE / MAX(frames, 1u);
    int cover = rising ? ui_ultra_out(u) : ui_ultra_out(ONE - u);
    int offset = LCD_HEIGHT - (LCD_HEIGHT * cover + ONE / 2) / ONE;
    const size_t row = LCD_WIDTH * sizeof(*out);
    if (cover == 0)
    {
        memcpy(out, back, LCD_HEIGHT * row);
        return;
    }
    if (offset == 0 && cover == ONE)
    {
        memcpy(out, front, LCD_HEIGHT * row);
        return;
    }
    int16_t back_map[LCD_WIDTH], front_map[LCD_WIDTH];
    uint32_t back_scale = STOCK_ONE - SHEET_BACK_SHRINK * cover / ONE;
    uint32_t back_inv = UINT32_MAX / back_scale;
    uint32_t front_scale = STOCK_ONE -
                           SHEET_FRONT_SHRINK * (ONE - cover) / ONE;
    uint32_t front_inv = UINT32_MAX / front_scale;
    unsigned light = 32 - SHEET_DIM * cover / ONE;
    int back_first = scale_map(back_map, back_inv);
    int first = scale_map(front_map, front_inv);
    int end = LCD_WIDTH - first;
    for (int y = 0; y < LCD_HEIGHT; y++, out += LCD_WIDTH)
    {
        int sy = LCD_HEIGHT / 2 +
                 (((y - LCD_HEIGHT / 2) * (int)back_inv) >> 16);
        const fb_data *src = (unsigned)sy < LCD_HEIGHT ?
                             back + sy * LCD_WIDTH : NULL;
        if (y < offset)
        {
            int above = offset - y;
            unsigned shade = above >= SHEET_SHADOW ? light :
                light * (SHEET_SHADOW + above) / (2 * SHEET_SHADOW);
            back_part(out, src, back_map, 0, LCD_WIDTH, back_first, shade);
            continue;
        }
        back_part(out, src, back_map, 0, first, back_first, light / 2);
        back_part(out, src, back_map, end, LCD_WIDTH, back_first, light / 2);
        int fy = (int)((uint32_t)(y - offset) * front_inv >> 16);
        front_span(out, front + MIN(fy, LCD_HEIGHT - 1) * LCD_WIDTH,
                   front_map, first, end);
    }
}
