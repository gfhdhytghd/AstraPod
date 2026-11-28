/* PodBox Fruit Company Ultra: the USB screen's entry. GPL version 2+. */
#ifndef PODBOX_USB_ULTRA_H
#define PODBOX_USB_ULTRA_H

#include <stdbool.h>
#include "ui/retained.h"

/* Under Ultra, take the memory for the entry and say where its picture
 * goes; false leaves the screen to draw as Fruit Company's. */
bool usb_ultra_open(struct ui_rect sprite);
/* The screen without the picture is in the frame buffer. */
void usb_ultra_keep_backdrop(void);
/* Bring the picture in. `draw` paints the screen's rows top to bottom,
 * picture included, into the frame buffer without sending them. */
void usb_ultra_enter(void (*draw)(int top, int bottom));
void usb_ultra_close(void);

#endif
