/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The stock world clock's large clock on its map, for a split list's pane.
 ****************************************************************************/
#ifndef PODBOX_SCREENS_EXTRAS_CLOCK_H
#define PODBOX_SCREENS_EXTRAS_CLOCK_H

#include <stdbool.h>

struct stock_canvas;

/* Draw the world map and the large clock where stock draws them in the
 * right-hand pane, showing `hour` (0-23) and `minute`. */
void extras_clock_put_large(struct stock_canvas *band, int hour, int minute);
/* A menu_pane_content.paint that shows the player's own time. */
void extras_clock_pane_paint(struct stock_canvas *band);

/* Shared timezone/city table interface for Settings > Date & Time > Time Zone. */
int extras_clock_city_count(void);
const char *extras_clock_city_name(int city);
int extras_clock_home_city(void);
void extras_clock_set_home_city(int city, bool adjust_rtc);

#endif
