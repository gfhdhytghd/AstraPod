/* AstraPod iCalendar event dates. GPL version 2 or later. */
#ifndef ASTRA_CALENDAR_ICS_H
#define ASTRA_CALENDAR_ICS_H
#include <stdbool.h>
#include <time.h>

struct time_zone;

bool calendar_ics_zone(const char *params, struct time_zone *zone, bool *found);

struct calendar_stamp {
    time_t seconds;
    bool absolute, all_day;
};

bool calendar_ics_stamp(const char *params, const char *value,
                        struct calendar_stamp *stamp);
void calendar_ics_load(void);
bool calendar_ics_on(int year, int month, int day,
                     void (*found)(const char *, void *), void *data);
#endif
