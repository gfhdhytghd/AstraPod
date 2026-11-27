/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The stock black status bar of the full-screen Extras pages (Alarms'
 * editors, Stopwatch, Screen Lock): StatusBarBlack_Background_Image over
 * the page, the title centred in white, and the battery at the right.
 ****************************************************************************/
#ifndef PODBOX_SCREENS_EXTRAS_BAR_H
#define PODBOX_SCREENS_EXTRAS_BAR_H

#include <stdbool.h>
#include "ui/stock_art.h"

struct gui_synclist;

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

/* Wheel-only navigation under Fruit themes: wheel returns STD_PREV/STD_NEXT,
 * SELECT returns STD_OK, MENU returns STD_CANCEL; LEFT/RIGHT control playback
 * and PLAY controls playback when `play_is_playback` is true (otherwise PLAY
 * returns ACTION_WPS_PLAY, for Stopwatch). */
int extras_get_action(int timeout, bool play_is_playback);
/* Locked Screen Lock wait: swallows MENU/PLAY/LEFT/RIGHT; wheel or SELECT
 * returns ACTION_STD_OK to open the combination tumbler. */
int extras_get_action_locked(int timeout);
/* list_do_action() backed by extras_get_action(timeout, true). */
bool extras_list_do_action(int timeout, struct gui_synclist *lists,
                           int *action);

#endif
