/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Reusable Date and Time pickers for Extras screens (Calendars, To Do's,
 * Contacts): stock 2.0.5 light-blue 3-well picker under Classic / Fruit
 * Company / Fruit Company Ultra, and a native dark 3-well picker under
 * Scrim.
 ****************************************************************************/
#ifndef PODBOX_SCREENS_EXTRAS_PICKER_H
#define PODBOX_SCREENS_EXTRAS_PICKER_H

#include "extras_store.h"

/* Run the 3-well Date Picker (Month / Day / Year).
 * Returns 1 if accepted (`date` updated), 0 if cancelled, -1 on USB. */
int extras_pick_date(const char *title, const char *caption,
                     struct extras_date *date, int min_year, int max_year);

/* Run the 2- or 3-well Time Picker (Hour / Minute [/ AM-PM]).
 * Returns 1 if accepted (`hour` and `minute` updated), 0 if cancelled,
 * -1 on USB. */
int extras_pick_time(const char *title, const char *caption,
                     int *hour, int *minute);

/* Utility: format time according to global_settings.timeformat. */
void extras_format_time(char *buf, size_t size, int hour, int minute);

#endif
