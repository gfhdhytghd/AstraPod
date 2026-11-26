/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LCD_HWSCROLL_H
#define LCD_HWSCROLL_H

#include <stdbool.h>
#include <stdint.h>

/* A zeroed request exits scroll mode and redraws the framebuffer. */
struct lcd_hwscroll_request {
    uint16_t lines, top, height, start;
};

bool lcd_hwscroll_valid(const struct lcd_hwscroll_request *request);
int lcd_hwscroll_test(const struct lcd_hwscroll_request *request);

/* Caller must own the drained LCD bus. */
bool lcd_hwscroll_send(uint8_t cmd, const uint8_t *data, unsigned int size);

#endif
