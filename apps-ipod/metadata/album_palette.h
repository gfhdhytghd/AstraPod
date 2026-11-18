/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PODBOX_ALBUM_PALETTE_H
#define PODBOX_ALBUM_PALETTE_H

#include <stdbool.h>
#include "lcd.h"
#include "ui/native_theme.h"

/* Reserve scratch before the audio buffer claims core memory. Initialize
 * events after settings load. Both functions are called once at startup. */
void album_palette_alloc(void);
void album_palette_init(void);
void album_palette_seed_resume(void);

/* Resolve the current track on the UI thread; may read a local thumbnail.
 * Each view can read the resulting RGB888 palette and watch its generation. */
void album_palette_refresh(void);
bool album_palette_snapshot(struct ui_album_palette *out);
unsigned album_palette_generation(void);
bool album_palette_needs_repaint(void);
bool album_palette_pending(void);

/* Playback calls opened before publishing a new artwork buffer. Consumers
 * pin handles for the duration of a bitmap read. */
int album_art_current(void);
unsigned album_art_generation(void);
void album_art_opened(int handle);

/* Render centered cover artwork into a linear RGB565 output, stride width.
 * Apply the adaptive text scrim and optionally soften the artwork. The
 * source bitmap stays unmodified. text_color is the packed LCD foreground. */
bool album_art_render_background(fb_data *out, int width, int height, int handle,
                                 bool blur, unsigned text_color);

#endif
