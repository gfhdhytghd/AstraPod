/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The stock black status bar of the full-screen Extras pages (Alarms'
 * editors, Stopwatch, Screen Lock): StatusBarBlack_Background_Image over
 * the page, the title centred in white, and the battery at the right.
 ****************************************************************************/
#ifndef PODBOX_SCREENS_EXTRAS_BAR_H
#define PODBOX_SCREENS_EXTRAS_BAR_H

#include "ui/stock_art.h"

#define EXTRAS_BAR_HEIGHT 24

/* Compose the bar over the part of `canvas` above row EXTRAS_BAR_HEIGHT,
 * for a page composed at 8 bits a channel. */
void extras_bar_put(struct stock_canvas *canvas, const char *title);
/* The same, choosing whether the battery and playback indicators appear. */
void extras_bar_put_ex(struct stock_canvas *canvas, const char *title,
                       bool show_battery, bool show_play);
/* Draw the bar over Background_LightBlue_Image into the framebuffer, rows
 * 0 to EXTRAS_BAR_HEIGHT - 1; the caller updates the LCD. */
void extras_bar_draw(const char *title);
/* Unload the title font; a page calls it when it leaves. */
void extras_bar_release(void);

/* Band `i` (0 or 1) of the rows the full-screen Extras pages compose in, a
 * band at a time: one page runs at once, and Stopwatch lays one band over
 * the other. Taken from the buffer when first asked for and given back by
 * extras_bar_release(); NULL when the buffer cannot spare them. */
#define EXTRAS_BAND_ROWS 8
uint32_t *extras_band(int i);

#endif
