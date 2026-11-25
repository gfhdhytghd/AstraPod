/* Retained Now Playing. GPL version 2+. */
#ifndef PODBOX_UI_NATIVE_WPS_H
#define PODBOX_UI_NATIVE_WPS_H

#include <stdbool.h>
#include "native_theme.h"
#include "lcd.h"

struct viewport;
struct ui_rect;
struct wps_state;

void native_wps_alloc(void);
int native_wps_art_slot(void);
/* UI thread only. The cache belongs to Now Playing, even while it is closed. */
int native_wps_background(int art, unsigned generation, enum ui_bg_kind mode, unsigned fg);
void native_wps_paint_background(const struct viewport *vp, int background, unsigned bg);
void native_wps_open(const struct wps_state *state);
bool native_wps_active(void);
bool native_wps_animating(void);
void native_wps_render(const struct wps_state *state, bool submit);
/* Paint the first Cover Flow handoff frame without submitting. Pending
 * artwork gets at most 150 ms while the preceding screen stays visible. */
void native_wps_paint_handoff(struct wps_state *state);
/* Actual last painted artwork, before the panel, text and meter overlays.
 * A NULL destination queries bounds only. Otherwise supply width * height
 * RGB565 pixels; the output is packed column-major, top to bottom. */
bool native_wps_art_frame(struct ui_rect *bounds, fb_data *column_major);
void native_wps_invalidate(void);
void native_wps_close(void);

#endif
