/* PodBox Fruit Company Ultra: Now Playing motion. GPL version 2+. */
#ifndef PODBOX_UI_ULTRA_WPS_H
#define PODBOX_UI_ULTRA_WPS_H

#include <stdbool.h>
#include <stdint.h>
#include "retained.h"

enum ultra_curve { ULTRA_OUT, ULTRA_BACK, ULTRA_SPRING, ULTRA_SMOOTH };
enum ultra_fade_slot { ULTRA_FADE_TEXT, ULTRA_FADE_COVER, ULTRA_FADE_COUNT };
/* Why the progress fill's width changed: playback, a new track, a seek's
 * preview or the seek landing. */
enum ultra_fill {
    ULTRA_FILL_PLAY, ULTRA_FILL_TRACK, ULTRA_FILL_SEEK, ULTRA_FILL_LAND
};

/* A view property driven along one of the Ultra curves. */
struct ultra_glide {
    struct ui_view *view;
    uint32_t start;
    int from, to;
    uint8_t property, frames, curve;
    bool active;
};

/* A cross-dissolve of a screen area from the pixels it showed to the ones
 * the page paints next. `skip` stays live: it always shows the new pixels.
 * With a slide, each band of rows that changed moves as it dissolves. */
struct ultra_fade {
    struct ui_rect area, skip;
    uint32_t start;
    int handle, shown, slide;
    uint8_t frames;
    bool active, fresh;
};

struct ultra_wps {
    struct ultra_glide fill, row;
    struct ultra_fade fade[ULTRA_FADE_COUNT];
    /* The bar the gloss crosses, its centre on screen and the fill's
     * width. */
    struct ui_rect gloss_box;
    uint32_t gloss_start;
    int gloss_x, gloss_width;
    unsigned trace;
    bool on, live, gloss, gloss_pending, boosted, seeking;
};

/* Start a glide of `property` from its value now to `to`. */
void ultra_wps_glide(struct ultra_glide *glide, struct ui_page *page,
                     struct ui_view *view, int property, int from, int to,
                     unsigned frames, enum ultra_curve curve, uint32_t now);
/* The progress fill's new width, which may glide there. */
void ultra_wps_fill(struct ultra_wps *u, struct ui_page *page,
                    struct ui_view *fill, int width, enum ultra_fill why,
                    uint32_t now);
/* Slide the volume and progress row, `in` towards the volume bar. */
void ultra_wps_row(struct ultra_wps *u, struct ui_page *page,
                   struct ui_view *row, bool in, uint32_t now);
/* Take the area's pixels on screen now as the old side of a dissolve. */
void ultra_wps_fade(struct ultra_wps *u, enum ultra_fade_slot slot,
                    struct ui_rect area, struct ui_rect skip, int slide,
                    unsigned frames);
void ultra_wps_step(struct ultra_wps *u, struct ui_page *page, uint32_t now);
/* Paint the page's damage, compose the dissolves and, with `submit`,
 * send it all to the LCD. */
void ultra_wps_draw(struct ultra_wps *u, struct ui_page *page, bool submit);
/* The travelling gloss over the fill's pixels in `clip`; `bar` is the
 * fill's screen bounds. */
void ultra_wps_paint_gloss(const struct ultra_wps *u, struct ui_rect bar,
                           struct ui_rect clip);
bool ultra_wps_busy(const struct ultra_wps *u);
void ultra_wps_close(struct ultra_wps *u);

#endif
