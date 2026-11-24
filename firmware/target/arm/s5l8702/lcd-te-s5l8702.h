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

/* Driver hooks. The wait runs with the LCD mutex held, before DMA. */
void lcd_te_probe(void);
void lcd_te_wait(int y, int width, int height);
void lcd_te_dma_done(unsigned int width, unsigned int height, uint32_t usec);
bool lcd_te_acquire_bus(void);
void lcd_te_release_bus(void);

#endif
