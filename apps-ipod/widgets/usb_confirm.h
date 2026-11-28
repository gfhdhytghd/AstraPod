/* PodBox USB consent popup. GNU General Public License, version 2+. */
#ifndef GUI_USB_CONFIRM_H
#define GUI_USB_CONFIRM_H
#include <stdbool.h>
#include <stdint.h>
bool gui_usb_confirm_boot_waiting(void);
bool gui_usb_confirm_boot_pending(void);
void gui_usb_confirm_boot_ready(void);
bool gui_usb_confirm_run_deferred(void);
bool gui_usb_confirm_run(uint32_t epoch);
#endif
