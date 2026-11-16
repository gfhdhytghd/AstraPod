/* Rockbox system.h for the threaded liveness fixture: an "IRQ mask" is one
 * recursive process-wide lock shared with the emulated EP0 interrupt. */
#ifndef TEST_SYSTEM_H
#define TEST_SYSTEM_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <assert.h>
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define ARRAYLEN(a) (sizeof(a) / sizeof((a)[0]))
long live_tick(void);
#define current_tick (live_tick())
#define is_thread_context() true
int disable_irq_save(void);
void restore_irq(int level);
#endif
