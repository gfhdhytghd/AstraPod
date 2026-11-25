/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "system.h"
#include "gpio-s5l8702.h"
#include "lcd-experiment-s5l8702.h"

#define LCD_REG(offset) (*(REG32_PTR_T)(LCD_BASE + (offset)))

static const unsigned int offsets[] = {0, 0x88, 0x7c, 0x78};
static const uint32_t stock[] = {1, 0x01000000, 0x00000804, 0x0005000a};
static uint32_t original[ARRAYLEN(offsets)];
static unsigned int stock_mask;

#define IRQ_TRACE_SIZE 16

static enum lcd_te_irq_mode irq_mode;
static struct {
    unsigned int started, retries, fallbacks;
    uint32_t max_wait, max_phase;
    struct {
        uint32_t wait, phase;
    } trace[IRQ_TRACE_SIZE];
} irq_stats;

static void read_regs(uint32_t *values)
{
    for (unsigned int i = 0; i < ARRAYLEN(offsets); ++i)
        values[i] = LCD_REG(offsets[i]);
}

void lcd_stock_reconfigure(unsigned int mask, struct lcd_stock_readback *regs,
                           uint32_t *command, uint32_t *frame)
{
    read_regs(regs->before);
    unsigned int changed = mask ^ stock_mask;
    for (unsigned int i = 0; i < ARRAYLEN(offsets); ++i)
    {
        unsigned int bit = 1u << i;
        if (!(changed & bit))
            continue;
        if (mask & bit)
            original[i] = regs->before[i];

        uint32_t value = mask & bit ? stock[i] : original[i];
        if (i == 0)
        {
            /* Preserve the current bus format and carry bit 0 through both
             * subsequent command and frame mode switches. */
            uint32_t con_bit = value & 1u;
            *command = (*command & ~1u) | con_bit;
            *frame = (*frame & ~1u) | con_bit;
            value = (regs->before[i] & ~1u) | con_bit;
            udelay(1);
        }
        LCD_REG(offsets[i]) = value;
    }
    stock_mask = mask;
    read_regs(regs->after);
}

void lcd_stock_restore_active(void)
{
    /* LCD_CON is set by the driver's command mode on wake. */
    for (unsigned int i = 1; i < ARRAYLEN(offsets); ++i)
        if (stock_mask & (1u << i))
            LCD_REG(offsets[i]) = stock[i];
}

enum lcd_te_irq_mode lcd_experiment_irq_mode(void)
{
    return irq_mode;
}

void lcd_experiment_irq_select(enum lcd_te_irq_mode mode)
{
    irq_mode = mode;
    memset(&irq_stats, 0, sizeof(irq_stats));
}

void lcd_experiment_irq_bind(struct eic_handler *handler)
{
    handler->level = EIC_INTLEVEL_HIGH;
    if (irq_mode == LCD_TE_IRQ_FALL)
    {
        unsigned int gpio = handler->gpio_n;
        handler->level = EIC_INTLEVEL_LOW;
        /* eint_register ORs in the level; unregister preserves it. */
        EIC_INTLEVEL(EIC_GROUP(gpio)) &= ~(1u << EIC_INDEX(gpio));
    }
    eint_register(handler);
}

void lcd_experiment_irq_started(uint32_t wait, uint32_t phase)
{
    unsigned int slot = irq_stats.started++ % IRQ_TRACE_SIZE;
    irq_stats.trace[slot].wait = wait;
    irq_stats.trace[slot].phase = phase;
    irq_stats.max_wait = MAX(irq_stats.max_wait, wait);
    irq_stats.max_phase = MAX(irq_stats.max_phase, phase);
}

void lcd_experiment_irq_retry(void)
{
    irq_stats.retries++;
}

void lcd_experiment_irq_fallback(void)
{
    irq_stats.fallbacks++;
}

void lcd_experiment_report(int (*report)(const char *, ...))
{
    int flags = disable_irq_save();
    enum lcd_te_irq_mode mode = irq_mode;
    typeof(irq_stats) stats = irq_stats;
    restore_irq(flags);

    report("TE IRQ=%s starts=%u retries=%u no-edge fallbacks=%u",
           mode == LCD_TE_IRQ_RISE ? "rise" :
           mode == LCD_TE_IRQ_FALL ? "fall" : "off",
           stats.started, stats.retries, stats.fallbacks);
    if (stats.started)
    {
        report("TE IRQ max ready-to-start=%lu us max ISR-to-start=%lu us",
               (unsigned long)stats.max_wait, (unsigned long)stats.max_phase);
        report("TE IRQ recent frames: ready-to-start / selected ISR-to-start");
        unsigned int first = stats.started - MIN(stats.started, IRQ_TRACE_SIZE);
        for (unsigned int i = first; i < stats.started; i++)
        {
            unsigned int slot = i % IRQ_TRACE_SIZE;
            report("  frame %u: %lu / %lu us", i + 1,
                   (unsigned long)stats.trace[slot].wait,
                   (unsigned long)stats.trace[slot].phase);
        }
    }
}
