/* Remote Control SDK binding. GNU GPL version 2 or later. */
#ifndef ASTRAPOD_ROCK_HID_H
#define ASTRAPOD_ROCK_HID_H

#include "usbstack/usb_hid_usage_tables.h"

enum rock_hid_status {
    ROCK_HID_ARMED,
    ROCK_HID_CABLE_BUSY,
    ROCK_HID_UNAVAILABLE,
};

int rock_hid_open(void);
void rock_hid_close(void);
void rock_hid_send(usage_page_t page, int id);

#endif
