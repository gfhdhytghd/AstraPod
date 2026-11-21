#ifndef SM1_IRQ_6G_H
#define SM1_IRQ_6G_H
#include <stdbool.h>
struct sm1_irq_stats { unsigned interrupts, semaphore_wakes, tick_wakes; };
void sm1_irq_begin(void);
void sm1_irq_disarm(void);
void sm1_irq_prepare(void);
bool sm1_irq_wait(unsigned timeout_us);
struct sm1_irq_stats sm1_irq_stats(void);
#endif
