/* Native retained list renderer. GPL version 2+. */
#ifndef PODBOX_LIST_RENDER_H
#define PODBOX_LIST_RENDER_H

#include "list.h"

void list_render_init(void);
/* A wheel event of `positions` raw wheel positions, signed. True when the
 * letter jump moved the selection and drew the list; otherwise *rows is how
 * far the selection moves, 0 for not at all. */
bool list_render_wheel(struct gui_synclist *list, int positions, int *rows);
void list_render_draw(struct gui_synclist *list);
/* A frame drawn because the selection moved: the list's contents are as
 * the last frame left them. */
void list_render_draw_moved(struct gui_synclist *list);
void list_render_settle(const struct gui_synclist *list);
void list_render_retarget(const struct gui_synclist *list, int selection, int top,
                          int offset, bool wheel);
void list_render_child_input(const struct gui_synclist *list);
bool list_render_child_current(unsigned token);
void list_render_forget(const struct gui_synclist *list);
int list_render_row_height(const struct gui_synclist *list, enum screen_type screen);
int list_render_visible_rows(const struct gui_synclist *list, enum screen_type screen);
bool list_render_motion_due(const struct gui_synclist *list);
/* Draw what list_render_motion_due() found due, and only that. */
void list_render_draw_due(struct gui_synclist *list);
int list_render_timeout(const struct gui_synclist *list, int timeout);

#endif
