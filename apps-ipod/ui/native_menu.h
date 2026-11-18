/* Native Scrim Settings renderer. GPL version 2+. */
#ifndef PODBOX_NATIVE_MENU_H
#define PODBOX_NATIVE_MENU_H

#include <stdbool.h>

struct gui_synclist;
struct menu_item_ex;

/* Begin before the owning menu pushes its normal theme frame. */
bool native_menu_begin(struct gui_synclist *, const struct menu_item_ex *,
                       const char *title);
bool native_menu_update(struct gui_synclist *, const struct menu_item_ex *,
                        const char *title);
void native_menu_suspend(struct gui_synclist *);
void native_menu_end(struct gui_synclist *);
bool native_menu_active(const struct gui_synclist *);
bool native_menu_draw(struct gui_synclist *);
void native_menu_tick(const struct gui_synclist *);
int native_menu_lines(const struct gui_synclist *);
int native_menu_status_height(void);
bool native_menu_button(struct gui_synclist *, int *action);

#endif
