/* Foreground execution leases. GNU GPL version 2 or later. */
#ifndef ASTRAPOD_ROCK_EXECUTION_H
#define ASTRAPOD_ROCK_EXECUTION_H

#include <stdbool.h>
#include <stddef.h>

void rock_execution_begin(void);
void rock_execution_end(void);
void rock_execution_stop_ticks(void);

unsigned int rock_worker_start(void (*run)(void *), void *arg,
                               void *stack, size_t bytes);
bool rock_worker_stopping(void);
bool rock_worker_join(unsigned int id);

bool rock_timer_start(long cycles, void (*tick)(void *), void *arg);
unsigned int rock_timer_dispatch(unsigned int limit);
void rock_timer_stop(void);

#endif
