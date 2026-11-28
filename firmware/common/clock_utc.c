/* AstraPod's UTC RTC and local clock boundary. GPL version 2 or later. */
#include <stddef.h>
#include "config.h"
#include "kernel.h"
#include "timefuncs.h"
#include "clock_utc.h"

#if CONFIG_RTC
static int (*utc_offset)(time_t, int *);
static time_t (*local_to_utc)(const struct tm *);

void clock_use_utc(int (*offset)(time_t, int *),
                   time_t (*from_local)(const struct tm *))
{
    local_to_utc = from_local;
    utc_offset = offset;
}

bool clock_uses_utc(void)
{
    return utc_offset != NULL;
}

time_t clock_utc_from_local(const struct tm *local)
{
    return local_to_utc(local);
}

int64_t clock_civil_seconds(const struct tm *civil)
{
    static const int days[] = {0, 31, 59, 90, 120, 151,
                                181, 212, 243, 273, 304, 334};
    int year = civil->tm_year + 1900 + civil->tm_mon / 12;
    int month = civil->tm_mon % 12;
    if (month < 0)
    {
        month += 12;
        year--;
    }
    int last = year - (month < 2);
    int leap = (last - 1968) / 4 - (last - 1900) / 100
               + (last - 1600) / 400;
    return (((year - 1970) * 365LL + days[month] + leap + civil->tm_mday - 1)
            * 24 + civil->tm_hour) * 3600 + civil->tm_min * 60 + civil->tm_sec;
}

#ifdef SIMULATOR
/* The simulator must never set the host or its hardware RTC. */
static time_t sim_base;
static unsigned long sim_tick;
static bool sim_set;
#endif

static time_t read_utc(bool fresh)
{
#ifdef SIMULATOR
    if (utc_offset)
    {
        if (sim_set)
            return sim_base + ((unsigned long)current_tick - sim_tick) / HZ;
        extern bool sim_screenshot;
        if (!sim_screenshot)
            return time(NULL);
    }
#endif
    struct tm raw = *clock_read_raw(fresh);
    if (!valid_time(&raw) || raw.tm_year > 137)
        return -1;
    time_t utc = clock_civil_seconds(&raw);
    struct tm checked;
    gmtime_r(&utc, &checked);
    return checked.tm_mday == raw.tm_mday ? utc : -1;
}

time_t get_time_utc(void)
{
    return read_utc(false);
}

time_t get_time_utc_fresh(void)
{
    return read_utc(true);
}

void clock_localtime(time_t utc, struct tm *local)
{
    int dst = -1;
    if (utc_offset)
        utc += utc_offset(utc, &dst);
    gmtime_r(&utc, local);
    local->tm_isdst = dst;
}

int set_time_utc(time_t utc)
{
    struct tm tm;
    if (utc < 946684800L || utc >= 2145916800L)
        return -2;
#ifdef SIMULATOR
    if (utc_offset)
    {
        sim_tick = current_tick;
        sim_base = utc;
        sim_set = true;
        return 0;
    }
#endif
    gmtime_r(&utc, &tm);
    return clock_write_raw(&tm);
}
#endif /* CONFIG_RTC */

