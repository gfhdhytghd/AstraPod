#ifndef LIVE_PANIC_H
#define LIVE_PANIC_H
void panicf(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
#endif
