/* Native retained list renderer. GPL version 2+. */
#ifndef PODBOX_LIST_RENDER_H
#define PODBOX_LIST_RENDER_H

#include "list.h"

void list_render_init(void);
bool list_render_index_action(struct gui_synclist *list, int *action,
                              bool wheel, int steps);
void list_render_draw(struct gui_synclist *list);
void list_render_settle(const struct gui_synclist *list);
void list_render_retarget(const struct gui_synclist *list, int selection, int top,
                          int offset, bool wheel);
void list_render_forget(const struct gui_synclist *list);
int list_render_row_height(const struct gui_synclist *list, enum screen_type screen);
int list_render_visible_rows(const struct gui_synclist *list, enum screen_type screen);
bool list_render_motion_due(const struct gui_synclist *list);
int list_render_timeout(const struct gui_synclist *list, int timeout);

#endif
