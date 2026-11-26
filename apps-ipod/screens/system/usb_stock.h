/* PodBox: the stock look's USB screen. GPL version 2+. */
#ifndef PODBOX_USB_STOCK_H
#define PODBOX_USB_STOCK_H

#include <stdbool.h>

/* Load the screen's fonts while the player still owns its storage. Nothing
 * outside the stock look (style.art). */
void usb_stock_open(void);
/* Draw the screen and send it to the LCD, or return false and draw nothing
 * when the theme is not the stock look or its art is missing. */
bool usb_stock_draw(void);
/* Unload what usb_stock_open() loaded. */
void usb_stock_close(void);

#endif
