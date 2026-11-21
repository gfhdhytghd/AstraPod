/* PodBox retained UI. GPL version 2+. */
#ifndef PODBOX_UI_RETAINED_H
#define PODBOX_UI_RETAINED_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "animation.h"
#include "lcd.h"

#define UI_FULL_OPACITY 256
#define UI_NAV_DEPTH 8

struct ui_rect {
    int x, y, width, height;
};

enum ui_property {
    UI_X, UI_Y, UI_WIDTH, UI_HEIGHT,
    UI_OFFSET_X, UI_OFFSET_Y, UI_OPACITY, UI_BRIGHTNESS,
    UI_CLIP_X, UI_CLIP_Y, UI_CLIP_WIDTH, UI_CLIP_HEIGHT,
    UI_SCROLL_OFFSET, UI_SELECTION,
    UI_PROPERTY_COUNT
};

struct ui_view;
struct ui_view_frame {
    struct ui_rect bounds, clip;
    int opacity, brightness;
    int scroll_offset, selection;
};

struct ui_view_ops {
    void (*paint)(const struct ui_view *, const struct ui_view_frame *);
    void (*layout)(struct ui_view *, const struct ui_rect *parent);
    bool (*action)(struct ui_view *, int action);
    /* Optional scroll physics. Use ui_page_set to invalidate changed geometry. */
    void (*step)(struct ui_view *, uint32_t clock);
};

struct ui_view {
    struct ui_view *parent, *first_child, *next_sibling;
    const struct ui_view_ops *ops;
    void *data;
    int value[UI_PROPERTY_COUNT];
};

/* Tracks are owned by the page's controller. It must cancel them before
 * disposing of the page or any attached view. */
struct ui_track {
    struct ui_track *next;
    struct ui_view *view;
    enum ui_property property;
    struct ui_tween tween;
};

struct ui_page {
    struct ui_view *root, *focus;
    struct ui_rect damage;
    struct ui_track *tracks;
    unsigned generation;
};

enum ui_presentation {
    UI_PAGE, UI_SHEET, UI_POPUP
};

struct ui_nav_entry {
    struct ui_page *page;
    enum ui_presentation presentation;
};

struct ui_nav {
    struct ui_nav_entry entry[UI_NAV_DEPTH];
    unsigned depth;
    struct ui_rect damage;
    /* Optional compositor observer. Called while both page owners are alive. */
    void (*changed)(struct ui_nav *, struct ui_page *from,
                    struct ui_page *to, enum ui_presentation, bool entering);
};

struct ui_layer {
    int handle, width, height;
    size_t offset_pixels;
};

/* Bounds are relative to the parent. Paint receives screen coordinates. */
void ui_view_init(struct ui_view *, struct ui_rect bounds,
                  const struct ui_view_ops *, void *data);
void ui_view_append(struct ui_view *parent, struct ui_view *child);
void ui_retained_invalidate(void);
void ui_page_init(struct ui_page *, struct ui_view *root);
void ui_page_layout(struct ui_page *);
void ui_page_invalidate(struct ui_page *, struct ui_rect screen_rect);
void ui_page_set(struct ui_page *, struct ui_view *, enum ui_property, int value);
void ui_page_tween(struct ui_page *, struct ui_track *, struct ui_view *,
                   enum ui_property, int target, unsigned frames,
                   bool ease_in, uint32_t clock);
void ui_page_cancel_tracks(struct ui_page *);
void ui_page_step(struct ui_page *, uint32_t clock);
/* Repaint dirty nodes to the default framebuffer. Only draw submits to LCD.
 * A painter must clip its writes to frame.clip and must not submit itself. */
struct ui_rect ui_page_paint(struct ui_page *);
void ui_page_draw(struct ui_page *);
bool ui_page_dispatch(struct ui_page *, int action);

/* Navigation handles full pages and translucent overlays. Zero-initialize
 * the stack and keep each page alive until it has been popped. */
bool ui_nav_push(struct ui_nav *, struct ui_page *, enum ui_presentation);
struct ui_page *ui_nav_pop(struct ui_nav *);
void ui_nav_draw(struct ui_nav *);
bool ui_nav_dispatch(struct ui_nav *, int action);

/* Optional cached RGB565 view layer. Callers acquire pixels only while pinned.
 * A borrowed layer may name a packed region in its owner's handle. Only
 * release a handle once, after all views referring to it leave the tree. */
bool ui_layer_alloc(struct ui_layer *, int width, int height);
void ui_layer_free(struct ui_layer *);
void ui_layer_paint(const struct ui_view *, const struct ui_view_frame *);
void ui_blit_dim(fb_data *dst, const fb_data *src, int pixels, unsigned brightness);
void ui_blit_tint(fb_data *dst, const fb_data *src, int pixels, unsigned brightness,
                  fb_data tint);
/* RGB565 source weights are in [0, 256] and sum to at most 256. */
void ui_blit_mix(fb_data *dst, const fb_data *above, const fb_data *below, int pixels,
                 unsigned above_weight, unsigned below_weight);

#endif
