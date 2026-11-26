/* PodBox: the stock iPod Classic firmware's bitmaps. GPL version 2+. */
#ifndef PODBOX_UI_STOCK_ART_H
#define PODBOX_UI_STOCK_ART_H

#include <stdint.h>
#include <stdbool.h>
#include "lcd.h"
#include "rbpaths.h"
#include "retained.h"
#include "stock_art_ids.h"

/* A mask has no palette: its pixels are the alpha, 0 to 255, of a colour its
 * caller picks. An image's pixels index its ARGB8888 palette, a byte each up
 * to 256 colours and two bytes past that. The sizes are built in; palette
 * and pixels point into STOCK_ART_PACK once it is loaded, which every draw
 * below does on first use. Without the pack the art draws nothing. */
struct stock_art {
    uint16_t width, height;
    uint16_t colors;
    const uint32_t *palette;
    const void *pixels;
};

extern struct stock_art stock_art[STOCK_ART_COUNT];

#define STOCK_ART_PACK THEME_DIR "/stock-art.bin"

/* Load the pack if it is not loaded, and say whether the art is there. */
bool stock_art_ready(void);
/* Free the pack; the next draw loads it again. */
void stock_art_release(void);

/* A picture at 8 bits a channel, xRGB8888 rows of `width` pixels, whose
 * top-left pixel is screen (x, y). Stock composes at this precision and the
 * LCD keeps the top 5, 6 and 5 bits, so art that overlaps is composed here
 * and written to the framebuffer once. */
struct stock_canvas {
    uint32_t *pixels;
    int x, y, width, height;
};

/* Draw `id` with its top-left on screen (x, y), source alpha over. */
void stock_art_put(struct stock_canvas *canvas, enum stock_art_id id,
                   int x, int y);
/* Draw the `width` x `height` part of `id` at source (sx, sy) with its
 * top-left on screen (x, y). */
void stock_art_put_part(struct stock_canvas *canvas, enum stock_art_id id,
                        int sx, int sy, int width, int height, int x, int y);
/* Draw the mask `id` in `rgb` (0xRRGGBB) at screen (x, y). */
void stock_art_mask(struct stock_canvas *canvas, enum stock_art_id id,
                    int x, int y, uint32_t rgb);
/* Blend `rgb` over the canvas pixel at screen (x, y) with `alpha` of 255. */
void stock_canvas_blend(struct stock_canvas *canvas, int x, int y,
                        uint32_t rgb, unsigned alpha);
/* Draw `text` in the PodBox font `font` with its line's top-left on screen
 * (x, y), each glyph's coverage blending `rgb` over the canvas as stock
 * blends text: d * (255 - a) / 255 + rgb * a / 255, rounded down. */
void stock_canvas_text(struct stock_canvas *canvas, int font, const char *text,
                       int x, int y, uint32_t rgb);
/* Write the part of the canvas inside `clip` to the default framebuffer. */
void stock_canvas_show(const struct stock_canvas *canvas, struct ui_rect clip);

/* Draw `id` at screen (x, y), clipped to `clip`, straight into the default
 * framebuffer: each pixel is widened to 8 bits, blended and truncated. */
void stock_art_draw(enum stock_art_id id, int x, int y, struct ui_rect clip);
/* The same for the mask `id` in `rgb`. */
void stock_art_draw_mask(enum stock_art_id id, int x, int y, uint32_t rgb,
                         struct ui_rect clip);
/* While on, the two draws above and stock_art_row() blend as the stock LCD
 * lays its UI layer over its picture layer: the RGB565 pixels below are
 * widened with zero low bits and blended with alpha / 255. Stock's Now
 * Playing draws its controls on that layer over the cover. */
void stock_art_over_layer(bool on);
/* Blend `count` pixels of row `sy` of `id`, from column `sx`, over `out`, a
 * run of pixels in any framebuffer-format buffer. */
void stock_art_row(enum stock_art_id id, int sx, int sy, fb_data *out,
                   int count);

#endif
