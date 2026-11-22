/* Foreground USB HID profile. GNU GPL version 2 or later. */
#ifndef USB_PLUGIN_HID_H
#define USB_PLUGIN_HID_H

#include <stdbool.h>
#include "config.h"

#ifdef HAVE_USB_PLUGIN_HID
#include "usb_hid_usage_tables.h"

bool usb_plugin_hid_open(void);
bool usb_plugin_hid_arm(void);
void usb_plugin_hid_close(void);
void usb_plugin_hid_insert(void);
void usb_plugin_hid_detach(void);
bool usb_plugin_hid_reserved(void);
bool usb_plugin_hid_latched(void);
bool usb_plugin_hid_send(usage_page_t page, int id);
void usb_plugin_hid_deliver(void);
void usb_plugin_hid_flush(void);
#else
static inline bool usb_plugin_hid_reserved(void) { return false; }
static inline bool usb_plugin_hid_latched(void) { return false; }
#endif

#endif
