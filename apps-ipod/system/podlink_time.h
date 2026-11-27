/* UI-owned RTC sync from the resident PodLink mailbox. GPL version 2+. */
#ifndef PODLINK_TIME_H
#define PODLINK_TIME_H
#include <stdbool.h>
#include <time.h>
bool podlink_time_decode(const char *record, struct tm *local, int *offset);
struct podlink_clock {
    time_t utc;
    int offset;
    const char *zone, *rule;
};
bool podlink_time_parse(char *record, struct podlink_clock *clock);
void podlink_time_sync(void);
#endif
