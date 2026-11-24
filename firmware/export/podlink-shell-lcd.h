/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PODLINK_SHELL_LCD_H
#define PODLINK_SHELL_LCD_H

#include <stdbool.h>
#include <stdint.h>

#define SHELL_LCD_INDEX 0x4955
#define SHELL_LCD_EVENT 0x20002u

struct usb_ctrlrequest;
void shell_lcd_request(const struct usb_ctrlrequest *req, uint8_t *buffer,
                       bool allowed);
void shell_lcd_dispatch(void);

#endif
