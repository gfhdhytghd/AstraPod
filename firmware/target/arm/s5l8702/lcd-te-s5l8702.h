/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LCD_TE_S5L8702_H
#define LCD_TE_S5L8702_H

#include <stdbool.h>
#include <stdint.h>

bool lcd_te_supported(void);
bool lcd_te_enabled(void);
void lcd_te_set_enabled(bool enable);

/* Reports one line per call after releasing the LCD bus. */
bool lcd_te_run_diagnostic(int (*report)(const char *fmt, ...));

/* Driver hooks. The wait runs in command mode with the LCD mutex held. */
void lcd_te_wait(int y);
bool lcd_te_acquire_bus(uint32_t *command_mode);
void lcd_te_release_bus(void);

#endif
