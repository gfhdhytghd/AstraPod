/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The stock iPod Classic Extras items, drawn as the stock firmware draws
 * them. The Fruit Company menu tree calls these from its Extras menu.
 ****************************************************************************/
#ifndef PODBOX_SCREENS_EXTRAS_H
#define PODBOX_SCREENS_EXTRAS_H

#include <stdbool.h>

/* Each runs one stock Extras item and returns 0 to go back to the Extras
 * menu, or a GO_TO_* code (root_menu.h) to leave for that screen. */
int extras_alarms(void);
int extras_calendars(void);
int extras_clock(void);
int extras_contacts(void);
int extras_notes(void);
int extras_screen_lock(void);
int extras_stopwatch(void);

/* Whether Screen Lock has a combination saved: the Extras pane reads
 * "Combination Set" when it has, "No Combination" when not. */
bool extras_screen_lock_set(void);

#endif
