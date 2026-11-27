/* PodBox Fruit Company Ultra menu pane motion. GPL version 2+. */
#ifndef PODBOX_UI_ULTRA_PANE_H
#define PODBOX_UI_ULTRA_PANE_H

#include <stdbool.h>
#include "retained.h"

/* Keep the pane on screen in `pane` to dissolve from, before a new row's
 * pane is painted over it. False without the memory, and the new pane then
 * replaces the old at once. */
bool ultra_pane_hold(struct ui_rect pane);
/* The new pane is painted: show the held one again and start dissolving to
 * the new. Returns the rectangle drawn. */
struct ui_rect ultra_pane_begin(void);
bool ultra_pane_active(void);
/* Advance the dissolve, and return the rectangle drawn. */
struct ui_rect ultra_pane_step(void);
/* Mix again what a repaint of `clip` replaced with the new pane. */
void ultra_pane_repaint(struct ui_rect clip);
/* Kernel ticks until the dissolve next draws. */
int ultra_pane_deadline(void);
/* Drop the dissolve where it stands; the pane's own painter takes over. */
void ultra_pane_cancel(void);

#endif
