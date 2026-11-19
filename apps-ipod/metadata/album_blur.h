/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ALBUM_BLUR_H
#define ALBUM_BLUR_H

#include "lcd.h"

#define ALBUM_BLUR_PANEL_W 220
#define ALBUM_BLUR_PANEL_H 170
#define ALBUM_BLUR_TEXT_LEFT 12
#define ALBUM_BLUR_HEADER_RIGHT 208
#define ALBUM_BLUR_DIVIDER 64
#define ALBUM_BLUR_ROWS_TOP (ALBUM_BLUR_DIVIDER + 2)
#define ALBUM_BLUR_ROW_PITCH 20
#define ALBUM_BLUR_ROWS ((ALBUM_BLUR_PANEL_H - ALBUM_BLUR_ROWS_TOP - 4) / ALBUM_BLUR_ROW_PITCH)
#define ALBUM_BLUR_ROWS_RIGHT 210
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
void album_blur_request_window(const struct album_blur_source *sources, unsigned count);
unsigned album_blur_generation(void);
bool album_blur_draw_panel(const struct album_blur_source *source);
bool album_blur_render_wps(fb_data *out, int art, unsigned generation, unsigned fg);

#endif
