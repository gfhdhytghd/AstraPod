#ifndef LIVE_THREAD_H
#define LIVE_THREAD_H
#include <stddef.h>
#define DEFAULT_STACK_SIZE 0x400
#define PRIORITY_BACKGROUND 20
#define IF_PRIO(...) __VA_ARGS__
#define IF_COP(...)
unsigned int create_thread(void (*function)(void), void *stack, size_t size,
                           unsigned flags, const char *name, int priority);
void yield(void);
#endif
