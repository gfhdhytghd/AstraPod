/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ALBUM_BLUR_H
#define ALBUM_BLUR_H

#include "lcd.h"

/* The Cover Flow album panel: a card 254 px wide from row 28 to the bottom
 * of the screen, a 44 px header over seven 24 px track rows. The text starts
 * 16 px in, past the selection marker and the playing sign. */
#define ALBUM_BLUR_PANEL_W 254
#define ALBUM_BLUR_PANEL_H 212
#define ALBUM_BLUR_TEXT_LEFT 16
#define ALBUM_BLUR_HEADER_RIGHT (ALBUM_BLUR_PANEL_W - 12)
#define ALBUM_BLUR_DIVIDER 44
#define ALBUM_BLUR_ROWS_TOP ALBUM_BLUR_DIVIDER
#define ALBUM_BLUR_ROW_PITCH 24
#define ALBUM_BLUR_ROWS \
    ((ALBUM_BLUR_PANEL_H - ALBUM_BLUR_ROWS_TOP) / ALBUM_BLUR_ROW_PITCH)
#define ALBUM_BLUR_ROWS_RIGHT (ALBUM_BLUR_PANEL_W - 12)
#define ALBUM_BLUR_NEIGHBORS 3
#define ALBUM_BLUR_DIM_TEXT LCD_RGBPACK(0x8a, 0x8a, 0x8a)

struct album_blur_source {
    unsigned local;
    char remote[65];
    bool portrait;
};

void album_blur_alloc(void);
void album_blur_init(void);
void album_blur_request_wps(void);
void album_blur_playing_changed(void);
void album_blur_request_window(const struct album_blur_source *sources, unsigned count);
unsigned album_blur_generation(void);
/* Rows `top` to `top + height` of the album panel's backdrop, drawn at the
 * same rows of the current viewport. */
bool album_blur_draw_panel(const struct album_blur_source *source, int top, int height);
bool album_blur_render_wps(fb_data *out, int art, unsigned generation, unsigned fg);

enum album_blur_tint_state { ALBUM_BLUR_WAITING, ALBUM_BLUR_NO_ART, ALBUM_BLUR_TINT };
enum album_blur_tint_state album_blur_status_tint(unsigned height, unsigned *color);

#endif
