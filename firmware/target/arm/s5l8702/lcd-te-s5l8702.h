/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LCD_TE_S5L8702_H
#define LCD_TE_S5L8702_H

#include <stdbool.h>
#include <stdint.h>

/* Include bus and FIFO time around the measured 23.47 ms at 0x33. */
#define FULL_DMA_FAST_US  22000
#define FULL_DMA_SLOW_US  25500

bool lcd_te_supported(void);
bool lcd_te_enabled(void);
void lcd_te_set_enabled(bool enable);
bool lcd_te_async_enabled(void);
void lcd_te_set_async_enabled(bool enable);
bool lcd_te_async_timing(uint32_t *busy, uint32_t *phase,
                         uint32_t *next_slot, uint32_t *copy);
unsigned int lcd_te_bus_timing(void);
bool lcd_te_set_bus_timing(unsigned int timing);
/* The probed refresh period and the latest TE rise seen, in USEC_TIMER
 * microseconds, with the kernel tick it was seen at. False until a probe
 * qualifies the pulse. */
bool lcd_te_timing(uint32_t *period, uint32_t *rise, long *rise_tick);
/* The last synchronized transfer: how long it waited for its TE window and
 * how long its DMA took, in microseconds. *count advances per transfer. */
void lcd_te_last_transfer(uint32_t *wait, uint32_t *dma, unsigned int *count);

/* Reports one line per call after releasing the LCD bus. The survey of
 * every GPIO and LCD signal holds the calling thread for one second. */
bool lcd_te_run_diagnostic(int (*report)(const char *fmt, ...), bool survey);

/* Driver hooks. Submission and cancellation run with IRQs masked. */
void lcd_te_probe(void);
void lcd_te_stop(void);
void lcd_te_submit(int y, int width, int height, uint32_t copy);
bool lcd_te_pending(void);
void lcd_te_cancel(void);
void lcd_te_untimed_start(void);
bool lcd_te_dma_busy(void);
/* Rows started from `offset` into the pending rect, up to the next wrap
 * of the hardware-scroll ring; 0 if none. */
int lcd_te_start_dma(int offset, int rows, bool timed,
                     uint32_t deadline, uint32_t *started);
void lcd_te_dma_done(unsigned int width, unsigned int height, uint32_t usec);
bool lcd_te_acquire_bus(void);
/* The latest TE rise from the edge interrupts, while they are tracked. */
bool lcd_te_irq_timing(uint32_t *rise, uint32_t *period, uint32_t *high);
void lcd_te_timer_arm(uint32_t deadline);
void lcd_te_release_bus(void);

#endif
