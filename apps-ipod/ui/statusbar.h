/* Native status bar service. GPL version 2+. */
#ifndef PODBOX_STATUSBAR_H
#define PODBOX_STATUSBAR_H

#include <stdbool.h>
#include <stddef.h>
#include "config.h"
#include "draw/icon.h"
#include "draw/screen_access.h"

void sb_init(void) INIT_ATTR;
void sb_update(enum screen_type screen, bool force);
void sb_settle(enum screen_type screen);
void sb_invalidate(void);
int sb_visible_height(void);
int sb_title_font(void);
int sb_motion_timeout(int timeout);
void sb_motion_presented(void);
void sb_set_update_delay(int delay);
bool sb_background_busy(void);
bool sb_show_disk_activity(void);
bool sb_show_background_tasks(void);
bool sb_show_working(void);

/* Returns whether chrome owns the title for this screen. Persistent titles
 * belong to their page. Transient titles are cleared at viewport transitions. */
bool sb_set_title_text(const char *title, enum themable_icons icon,
                       enum screen_type screen);
bool sb_set_persistent_title(const char *title, enum themable_icons icon,
                             enum screen_type screen);
bool sb_get_persistent_title(enum screen_type screen, char *text, size_t size,
                             enum themable_icons *icon);
/* Copy the current title and return whether it is persistent. */
bool sb_get_title(enum screen_type screen, char *text, size_t size,
                   enum themable_icons *icon);
void sb_clear_transient_title(enum screen_type screen);

/* Applies native list/content geometry and theme colours to a prepared
 * fullscreen viewport. Fullscreen owners use the same colour roles. */
void sb_apply_viewport(struct viewport *vp, enum screen_type screen, bool chrome);

#endif
