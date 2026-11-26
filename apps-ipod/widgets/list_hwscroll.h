/* A list frame as a panel hardware-scroll frame. GPL version 2+. */
#ifndef PODBOX_LIST_HWSCROLL_H
#define PODBOX_LIST_HWSCROLL_H

#include <stdbool.h>
#include "ui/hwscroll.h"

/* Rows of the whole width a frame's bands may add up to: about what the
 * panel's blanking and scan leave time to write in one refresh. */
#define LIST_HWSCROLL_ROWS 136

/* What two frames of one list share. The area runs from `area_top` to the
 * bottom of a `width` x `height` screen; within it only `content`, the
 * whole rows, moves with the list. */
struct list_hwscroll_layout {
    int width, height, area_top;
    struct ui_rect content;
    struct ui_rect scrollbar;  /* over the content; width 0 without one */
    struct ui_rect notes;      /* follows the selection; height 0 without */
    int pitch, span;           /* row height, rows a selection covers */
};

/* One frame. Item i sits at content.y - top + i * pitch. */
struct list_hwscroll_pose {
    int top;
    struct ui_rect marker;     /* the selection marker or bar as drawn */
    int lit;                   /* the first item in selected text */
    int selected;
};

/* The frame that turns what `shown` left on the panel into `next`. False
 * when the content moves its whole height or more, or the changed rows
 * need more than UI_HWSCROLL_BANDS bands or LIST_HWSCROLL_ROWS rows. */
bool list_hwscroll_plan(const struct list_hwscroll_layout *layout,
                        const struct list_hwscroll_pose *shown,
                        const struct list_hwscroll_pose *next,
                        struct ui_hwscroll_frame *frame);

#endif
