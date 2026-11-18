/* Shared native sheet transitions. GPL version 2+. */
#ifndef PODBOX_UI_SHEET_H
#define PODBOX_UI_SHEET_H

#include "lcd.h"
#include "retained.h"

struct ui_sheet {
    struct ui_tween tween;
    struct ui_rect previous;
    unsigned trace;
    int light;
    bool active;
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
int ui_sheet_timeout(const struct ui_sheet *sheet, int timeout);
/* Preserve any queued event for the screen that owns the next action. */
bool ui_sheet_wait(const struct ui_sheet *sheet);

#endif
