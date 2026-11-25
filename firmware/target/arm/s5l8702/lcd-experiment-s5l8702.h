/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LCD_EXPERIMENT_S5L8702_H
#define LCD_EXPERIMENT_S5L8702_H

#include <stdbool.h>
#include <stdint.h>

#define LCD_STOCK_ALL 0x0f

enum lcd_te_irq_mode { LCD_TE_IRQ_OFF, LCD_TE_IRQ_RISE, LCD_TE_IRQ_FALL };

struct lcd_stock_readback {
    uint32_t before[4];
    uint32_t after[4];
};

bool lcd_stock_set(unsigned int mask, struct lcd_stock_readback *readback);
bool lcd_te_set_irq_mode(enum lcd_te_irq_mode mode);

/* Test transfers use the normal staging queue. The caller owns the LCD bus. */
void lcd_experiment_present(int y, int height,
                            void (*draw)(uint16_t *, int, unsigned int),
                            unsigned int frame);
uint32_t lcd_experiment_dma_total(void);

/* Controller and scheduler hooks. */
struct eic_handler;
void lcd_stock_reconfigure(unsigned int mask, struct lcd_stock_readback *regs,
                           uint32_t *command, uint32_t *frame);
void lcd_stock_restore_active(void);
enum lcd_te_irq_mode lcd_experiment_irq_mode(void);
void lcd_experiment_irq_select(enum lcd_te_irq_mode mode);
void lcd_experiment_irq_bind(struct eic_handler *handler);
void lcd_experiment_irq_started(uint32_t wait, uint32_t phase);
void lcd_experiment_irq_retry(void);
void lcd_experiment_irq_fallback(void);
void lcd_experiment_report(int (*report)(const char *, ...));

#endif
