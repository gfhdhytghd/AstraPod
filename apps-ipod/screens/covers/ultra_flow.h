/* PodBox Fruit Company Ultra Cover Flow. GPL version 2+. */
#ifndef COVERS_ULTRA_FLOW_H
#define COVERS_ULTRA_FLOW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "lcd.h"
#include "stock_motion.h"
#include "stock_color.h"

/* ui_theme_ultra() as of the last ultra_flow_sync(). The loader thread
 * reads it, so it must not ask the theme itself. */
extern bool ultra_flow_on;
/* Refresh ultra_flow_on; true when it changed, and with it the look of
 * every reflection already built. */
bool ultra_flow_sync(void);

/* reflection_build() with a smooth falloff and a faint gloss on the floor
 * just below the cover. Same arguments and layout. */
void ultra_reflection_build(fb_data *out, const fb_data *cover, int size,
                            int stride, fb_data bg);

/* How much a cover at world x, or in flight, is taken toward the ground,
 * Q8. ultra_flow_light() adds the per-column depth to it. */
int ultra_flow_fog(const struct stock_flow *f, int album, uint32_t now,
                   int32_t x);
/* The Q8 light of a column `height` pixels tall: 256 for the centre
 * cover at rest, whose columns are `centre` pixels tall. */
unsigned ultra_flow_light(int fog, int height, int centre);

/* A light in stock_color lanes: a texel keeps a/32 of itself and gains
 * `add`, the target colour's share with the rounding. */
struct ultra_lit {
    uint32_t a, add;
};

static inline struct ultra_lit ultra_lit(unsigned light, fb_data target)
{
    unsigned a = light >> 3;
    return (struct ultra_lit){a, stock_color_lanes(target) * (32 - a) +
                                 0x02008010u};
}

static inline uint32_t ultra_lit_lanes(uint32_t lanes,
                                       const struct ultra_lit *lit)
{
    return ((lanes * lit->a + lit->add) >> 5) & STOCK_COLOR_LANES;
}

static inline fb_data ultra_lit_pixel(fb_data c, const struct ultra_lit *lit)
{
    uint32_t lanes = ultra_lit_lanes(stock_color_lanes(c), lit);
    return lanes | lanes >> 16;
}

/* `n` texels of the column at `src`, from Q10 row `p` by `dp`, lit, to
 * `dst` and on `stride` apart. Returns `p` after the last. */
unsigned ultra_lit_run(const fb_data *src, fb_data *dst, int stride, int n,
                       unsigned p, int dp, const struct ultra_lit *lit);

/* The album turn's lamp for one pose: shade toward black as the face
 * turns edge-on, and a highlight that sweeps across it mid-turn. */
struct ultra_lamp {
    int shade, recede, spec, spec_at;
    bool far_left;
};
void ultra_lamp_aim(struct ultra_lamp *lamp, struct stock_pose pose,
                    const struct stock_edge edge[2]);
/* The light of the column `t` (Q16) of the way from the face's left
 * edge; false when the column is drawn unlit. */
bool ultra_lamp_column(const struct ultra_lamp *lamp, int t,
                       struct ultra_lit *lit);

/* The covers behind the album turn while the row holds still: drawn once,
 * then copied back each frame. ultra_still_restore() copies the `pixels`
 * at `at` back when it holds the copy named `key`; ultra_still_keep()
 * takes one, and does nothing when there is no memory for it. */
bool ultra_still_restore(fb_data *at, size_t pixels, uint32_t key);
void ultra_still_keep(const fb_data *at, size_t pixels, uint32_t key);
void ultra_still_drop(void);

/* Now Playing rising over (or sinking off) Cover Flow at `frame` of
 * `frames`: the page eases in at a slight scale while the covers recede
 * and dim behind a soft shadow. The first rising and last sinking frame
 * are `back`, the last rising and first sinking frame `front`, exactly. */
void ultra_sheet_compose(fb_data *out, const fb_data *back,
                         const fb_data *front, unsigned frame,
                         unsigned frames, bool rising);

#endif
