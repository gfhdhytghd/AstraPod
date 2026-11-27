/* Private RTC boundary shared by clock_utc.c and timefuncs.c.
 * GPL version 2 or later.
 */
#ifndef CLOCK_UTC_INTERNAL_H
#define CLOCK_UTC_INTERNAL_H

struct tm *clock_read_raw(bool fresh);
int clock_write_raw(const struct tm *raw);
bool clock_uses_utc(void);
time_t clock_utc_from_local(const struct tm *local);

#endif
