/* Retained Now Playing. GPL version 2+. */
#ifndef PODBOX_UI_NATIVE_WPS_H
#define PODBOX_UI_NATIVE_WPS_H

#include <stdbool.h>
#include "native_theme.h"
#include "lcd.h"

struct viewport;
struct ui_rect;
struct wps_state;
struct stock_wps_controls;
struct album_blur_source;

void native_wps_alloc(void);
int native_wps_art_slot(void);
/* UI thread only. The cache belongs to Now Playing, even while it is closed. */
int native_wps_background(int art, unsigned generation, enum ui_bg_kind mode, unsigned fg);
void native_wps_paint_background(const struct viewport *vp, int background, unsigned bg);
void native_wps_open(const struct wps_state *state);
/* Hand the LCD to an overlay while keeping the page and its damage state. */
void native_wps_suspend(bool suspend);
bool native_wps_active(void);
bool native_wps_animating(void);
int native_wps_timeout(void);
void native_wps_stock_controls(const struct stock_wps_controls *controls);
void native_wps_render(const struct wps_state *state, bool submit);
/* Paint the first frame using the selected album's backdrop. */
void native_wps_paint_handoff(const struct wps_state *state,
                              const struct album_blur_source *source);
void native_wps_paint_prepared(void);
/* The theme slot is available before the page opens. An empty rectangle
 * means no slot. True means the live page has confirmed the layout. */
bool native_wps_art_layout(struct ui_rect *bounds);
/* The slot is a cover turned away at the stock Now Playing pose; its bounds
 * are the face's. */
bool native_wps_art_turned(void);
/* Scrim can wait for the blurred backdrop before the first sheet frame. */
bool native_wps_sheet_ready(void);
/* The current cover's square centre, column-major, and its side or 0. */
int native_wps_art_side(void);
void native_wps_copy_art(fb_data *dst, int side);
bool native_wps_has_art(void);
/* Borrow the carousel's selected cover until native art or a different track arrives. */
void native_wps_set_landing(void (*paint)(const struct ui_rect *clip));
void native_wps_pick_up_art(void);
/* Borrow a column-major texture from the turn while this track is current. */
void native_wps_prepare_turn_art(int thumbnail, int side);
void native_wps_paint_slide(int hidden, fb_data base);
/* The page's status bar, text and controls over what the frame buffer holds,
 * without its background or cover. */
void native_wps_paint_chrome(void);
void native_wps_invalidate(void);
void native_wps_close(void);

#endif
