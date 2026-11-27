/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Stock iPod Classic 2.0.5 power screens and Sleep preview under
 * style.menu_tree=stock (Fruit Company and Fruit Company Ultra):
 *  - dedicated full-screen Charging / Charged standby screen
 *  - stock low-battery shutdown screen
 *  - Main Menu > Sleep immediate action and moon/stars right-pane preview
 ****************************************************************************/
#ifndef PODBOX_SCREENS_POWER_STOCK_H
#define PODBOX_SCREENS_POWER_STOCK_H

#include <stdbool.h>

struct stock_canvas;

/* Run the stock Charging / Charged standby screen until the user wakes it,
 * the charger disconnects, or a system event (USB, reboot, critical battery)
 * arrives. Returns 0 on user wake/unplug, or the pending system event. */
long power_stock_charging_screen(void);

/* Show the stock Low Battery warning screen before hardware shutdown.
 * `critical` selects the "Please Connect to Power" empty-battery variant. */
void power_stock_low_battery_screen(bool critical);

/* Paint the crescent moon and stars right-pane preview for Home > Sleep. */
void power_stock_sleep_pane_paint(struct stock_canvas *band);

/* Immediate Sleep action for Fruit Company's Home > Sleep row: enters
 * charging standby when on external power, or requests power off otherwise. */
int power_stock_sleep_now(void);

#endif /* PODBOX_SCREENS_POWER_STOCK_H */
