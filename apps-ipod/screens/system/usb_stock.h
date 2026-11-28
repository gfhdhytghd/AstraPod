/* PodBox: the stock look's USB screen. GPL version 2+. */
#ifndef PODBOX_USB_STOCK_H
#define PODBOX_USB_STOCK_H

#include <stdbool.h>

/* Load the screen's fonts and pre-cache glyphs while the player still owns
 * its storage. Nothing outside the stock look (style.art). */
void usb_stock_open(void);
/* Draw the screen and send it to the LCD, or return false and draw nothing
 * when the theme is not the stock look or its art is missing. */
bool usb_stock_draw(void);
/* Bound the USB event wait while the stock screen is open. */
int usb_stock_timeout(int timeout);
/* Refresh the connection state, animation, battery and hold. Safe after
 * font_disable_all(): reads only cached glyphs and in-RAM stock art. */
void usb_stock_tick(void);

/* Fruit Company Ultra's entry, once the host is answered. */
void usb_stock_enter(void);
/* Unload what usb_stock_open() loaded. */
void usb_stock_close(void);

#endif
