/* PodBox Fruit Company Ultra page motion. GPL version 2+. */
#ifndef PODBOX_UI_ULTRA_SCENE_H
#define PODBOX_UI_ULTRA_SCENE_H

#include <stdint.h>
#include "lcd.h"
#include "scene.h"

#ifdef __thumb__
#define ULTRA_ARM __attribute__((target("arm")))
#else
#define ULTRA_ARM
#endif

typedef uint32_t __attribute__((may_alias)) ultra_pair;

/* `weight` 32nds of `a` over `b`; the RGB565 lanes share one multiply. */
static inline __attribute__((always_inline))
fb_data ultra_mix32(fb_data a, fb_data b, unsigned weight)
{
    const uint32_t mask = 0x07e0f81f;
    uint32_t fa = (a | ((uint32_t)a << 16)) & mask;
    uint32_t fb = (b | ((uint32_t)b << 16)) & mask;
    uint32_t c = (fb + (((fa - fb) * weight) >> 5)) & mask;
    return c | (c >> 16);
}

/* Two pixels in one word: one pixel's red and blue with the other's green,
 * and the rest shifted down five, each lane with room above it for a
 * product of 32. */
#define ULTRA_EVEN 0x07e0f81fu
#define ULTRA_ODD 0x07c0f83fu

/* Pixel pair `c` at `weight` 32nds of its brightness. */
static inline __attribute__((always_inline))
uint32_t ultra_dim_pair(uint32_t c, unsigned weight)
{
    uint32_t e = ((c & ULTRA_EVEN) * weight >> 5) & ULTRA_EVEN;
    uint32_t o = (((c >> 5) & ULTRA_ODD) * weight >> 5) & ULTRA_ODD;
    return e | o << 5;
}

/* `weight` 32nds of pixel pair `a` over pair `b`. */
static inline __attribute__((always_inline))
uint32_t ultra_mix_pair(uint32_t a, uint32_t b, unsigned weight)
{
    uint32_t be = b & ULTRA_EVEN, bo = (b >> 5) & ULTRA_ODD;
    uint32_t e = (be + ((((a & ULTRA_EVEN) - be) * weight) >> 5)) &
                 ULTRA_EVEN;
    uint32_t o = (bo + (((((a >> 5) & ULTRA_ODD) - bo) * weight) >> 5)) &
                 ULTRA_ODD;
    return e | o << 5;
}

/* `c` at `weight` 32nds of its brightness. */
static inline __attribute__((always_inline))
fb_data ultra_dim32(fb_data c, unsigned weight)
{
    const uint32_t mask = 0x07e0f81f;
    uint32_t v = (c | ((uint32_t)c << 16)) & mask;
    v = ((v * weight) >> 5) & mask;
    return v | (v >> 16);
}

/* A page slide's duration under Ultra, from the base theme's. */
unsigned ultra_scene_frames(unsigned frames);
/* The slide's offset at `frame`, before it is made even. */
int ultra_scene_sample(const struct ui_tween *tween, unsigned frame);
/* Compose one frame of a page slide: the page on the right lies over the
 * one on the left, which recedes and darkens under its shadow. `chrome` is
 * the page the status bar shows at rest. */
void ultra_scene_compose(const struct ui_scene *scene, fb_data *out,
                         const fb_data *back, const fb_data *front,
                         const fb_data *chrome, int offset);
/* The shutdown fade's duration under Ultra, from the base theme's. */
unsigned ultra_scene_fade_frames(unsigned frames);
/* The shutdown fade: `source` shrinks and darkens into `out`, `level` of
 * UI_FULL_OPACITY still lit. */
void ultra_scene_fade(fb_data *out, const fb_data *source, int level);

#endif
