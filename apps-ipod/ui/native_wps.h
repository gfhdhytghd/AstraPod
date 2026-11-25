/* Retained Now Playing. GPL version 2+. */
#ifndef PODBOX_UI_NATIVE_WPS_H
#define PODBOX_UI_NATIVE_WPS_H

#include <stdbool.h>
#include "native_theme.h"
#include "lcd.h"

struct viewport;
struct ui_rect;
struct wps_state;
struct album_blur_source;

void native_wps_alloc(void);
int native_wps_art_slot(void);
/* UI thread only. The cache belongs to Now Playing, even while it is closed. */
int native_wps_background(int art, unsigned generation, enum ui_bg_kind mode, unsigned fg);
void native_wps_paint_background(const struct viewport *vp, int background, unsigned bg);
void native_wps_open(const struct wps_state *state);
bool native_wps_active(void);
bool native_wps_animating(void);
void native_wps_render(const struct wps_state *state, bool submit);
/* Paint the first frame using the selected album's backdrop. */
void native_wps_paint_handoff(const struct wps_state *state,
                              const struct album_blur_source *source);
/* The theme slot is available before the page opens. An empty rectangle
 * means no slot. True means the live page has confirmed the layout. */
bool native_wps_art_layout(struct ui_rect *bounds);
/* Actual last painted artwork, before the panel, text and meter overlays.
 * A NULL destination queries bounds only. Otherwise supply width * height
 * RGB565 pixels; the output is packed column-major, top to bottom. */
bool native_wps_art_frame(struct ui_rect *bounds, fb_data *column_major);
void native_wps_pick_up_art(void);
void native_wps_paint_chrome(void);
int native_wps_chrome_stop(void);
void native_wps_paint_slide(int hidden, fb_data base);
void native_wps_invalidate(void);
void native_wps_close(void);

#endif
