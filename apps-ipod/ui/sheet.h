/* Shared native sheet transitions. GPL version 2+. */
#ifndef PODBOX_UI_SHEET_H
#define PODBOX_UI_SHEET_H

#include "lcd.h"
#include "retained.h"

struct ui_sheet {
    struct ui_tween tween;
    struct ui_rect previous;
    unsigned trace;
    uint32_t requested_at;
    int light;
    fb_data tint;
    bool active, starting;
    /* Set before ui_sheet_begin() for the HUD's fade and scale instead of
     * the slide. */
    bool zoom;
};

void ui_sheet_begin(struct ui_sheet *sheet, bool open, const char *route);
void ui_sheet_cancel(struct ui_sheet *sheet);
/* The backdrop is a full panel. The body is packed in the size of its bounds.
 * Both stay live until drawing returns. A changed body can repaint at rest. */
void ui_sheet_paint(struct ui_sheet *sheet, const fb_data *backdrop,
                    const fb_data *body, struct ui_rect bounds, bool changed);
/* Same compositor for a viewport whose owner will present a rotated panel. */
void ui_sheet_paint_in(struct ui_sheet *sheet, const fb_data *backdrop,
                       const fb_data *body, struct ui_rect bounds, bool changed,
                       fb_data *output, int width, int height, void (*present)(void));
/* The HUD transition. `body` is the whole finished screen; its rows above
 * `top` cross-fade, and the panel at `bounds` fades in while it grows to
 * full size over the backdrop as it dims. Below `top`, `body` outside the
 * panel must be the backdrop as ui_sheet_dim() leaves it. */
void ui_sheet_paint_zoom(struct ui_sheet *sheet, const fb_data *backdrop,
                         const fb_data *body, struct ui_rect bounds, int top);
/* The backdrop of a finished sheet: `pixels` of `src`, dimmed, to `dst`. */
void ui_sheet_dim(fb_data *dst, const fb_data *src, int pixels);
int ui_sheet_timeout(const struct ui_sheet *sheet, int timeout);
/* Preserve any queued event for the screen that owns the next action. */
bool ui_sheet_wait(const struct ui_sheet *sheet);

#endif
