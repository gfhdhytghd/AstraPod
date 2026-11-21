#ifndef SM1_IRQ_HOST_TEST
#include "config.h"
#include "kernel.h"
#include "system.h"
#include "s5l87xx.h"
#define SM1_IRQ (1u << 18)
#define SM1_REG(offset) (*(volatile uint32_t *)(SM1_BASE + (offset)))
#endif
#include "sm1-irq-6g.h"

/* IRQ18 is serialized by the SM1 session mutex. The ISR does not claim
 * the SRAM or clear the DSP latch: the thread must claim it first. */
static struct semaphore sm1_done;
static volatile bool sm1_irq_armed;
static volatile unsigned sm1_irq_count;
static unsigned sm1_sem_wakes, sm1_tick_wakes;

void INT_IRQ18(void)
{
    VIC0INTENCLEAR = SM1_IRQ;
    if (sm1_irq_armed)
    {
        sm1_irq_armed = false;
        sm1_irq_count++;
        semaphore_release(&sm1_done);
    }
}

void sm1_irq_disarm(void)
{
    int old = disable_irq_save();
    VIC0INTENCLEAR = SM1_IRQ;
    sm1_irq_armed = false;
    restore_irq(old);
}

void sm1_irq_prepare(void)
{
    sm1_irq_disarm();
    semaphore_wait(&sm1_done, 0);
    int old = disable_irq_save();
    sm1_irq_armed = true;
    VIC0INTENABLE = SM1_IRQ;
    restore_irq(old);
}

bool sm1_irq_wait(unsigned timeout)
{
    uint32_t start = USEC_TIMER;
    for (;;)
    {
        /* Cold initialization can finish without asserting IRQ18. After
         * a real IRQ, ordinary frames need only a bounded recovery timer. */
        if (SM1_REG(0xa98) & 4)
        {
            sm1_irq_disarm();
            return true;
        }
        uint32_t elapsed = USEC_TIMER - start;
        if (elapsed >= timeout)
        {
            sm1_irq_disarm();
            return (SM1_REG(0xa98) & 4) != 0;
        }
        unsigned ticks = 1;
        if (sm1_irq_count && sm1_irq_armed)
        {
            unsigned remaining = timeout - elapsed;
            ticks = (remaining + (1000000/HZ) - 1) / (1000000/HZ);
            if (ticks > HZ/20) ticks = HZ/20;
            if (!ticks) ticks = 1;
        }
        if (semaphore_wait(&sm1_done, ticks) == OBJ_WAIT_SUCCEEDED)
            sm1_sem_wakes++;
        else
            sm1_tick_wakes++;
    }
}


void sm1_irq_begin(void)
{
    sm1_irq_disarm();
    semaphore_init(&sm1_done, 1, 0);
    sm1_irq_count = sm1_sem_wakes = sm1_tick_wakes = 0;
}

struct sm1_irq_stats sm1_irq_stats(void)
{
    return (struct sm1_irq_stats){sm1_irq_count, sm1_sem_wakes, sm1_tick_wakes};
}
