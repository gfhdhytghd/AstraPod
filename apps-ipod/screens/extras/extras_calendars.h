/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The stock calendar page, for the panes and editors that show it outside
 * Extras > Calendars.
 ****************************************************************************/
#ifndef PODBOX_SCREENS_EXTRAS_CALENDARS_H
#define PODBOX_SCREENS_EXTRAS_CALENDARS_H

#include <stdbool.h>
#include <stddef.h>
#include "ui/menu_pane.h"
#include "ui/stock_art.h"

/* Settings_DateTime_Calendar_Image with its top-left at screen (x, y), the
 * short name of `month` (1-12) on its red band and `day` (1-31) below it in
 * two digits, as stock draws them. */
void extras_calendars_icon_put(struct stock_canvas *canvas, int x, int y,
                               int month, int day);
/* Fill `c` with stock's calendar pane: the time, today's page and weekday.
 * A list's pane callback passes it to menu_pane_set_picture() and returns
 * LIST_PANE_PICTURE. Its stamp changes with the minute, so a list that
 * redraws when the minute turns keeps the time current. */
void extras_calendars_pane(struct menu_pane_content *c);

/* Calendar event reminders for extras_alarm_ring.c.
 * `minute` is minutes since 2000-01-01 00:00 (matching extras_alarm_ring.c).
 * `sound_mode` receives 0 (Beep) or 1 (None/silent); Off never triggers. */
bool extras_calendars_alarms_armed(void);
bool extras_calendars_due_event(long minute, char *summary, size_t size,
                                int *hour, int *min, int *sound_mode);

#endif
