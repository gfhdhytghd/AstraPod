/* AstraPod iCalendar events: an instant or an unzoned calendar date.
 * GPL version 2 or later.
 */
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "system.h"
#include "string-extra.h"
#include "file.h"
#include "timefuncs.h"
#include "system/strutil.h"
#include "system/time_zone.h"
#include "screens/extras/calendar_ics.h"

#define ICS_FILE ROCKBOX_DIR "/calendar.ics"
#define ICS_EVENTS 128
#define ICS_DAY 86400

struct ics_event {
    struct calendar_stamp start, end;
    int first, last;
    char title[64];
};

static struct ics_event events[ICS_EVENTS];
static int event_count;
static unsigned zone_generation;

static int digits(const char *text, int n)
{
    int value = 0;
    while (n--)
    {
        if (*text < '0' || *text > '9')
            return -1;
        value = value * 10 + *text++ - '0';
    }
    return value;
}

bool calendar_ics_zone(const char *params, struct time_zone *zone, bool *found)
{
    *found = false;
    while (*params == ';')
    {
        const char *start = ++params;
        while (*params && *params != ';')
            params++;
        if (params - start < 5 || strncasecmp(start, "TZID=", 5))
            continue;
        start += 5;
        const char *end = params;
        if (*start == '"' && end - start > 2 && end[-1] == '"')
        {
            start++;
            end--;
        }
        if (end - start <= 0 || end - start >= TIME_ZONE_NAME_SIZE)
            return false;
        char name[TIME_ZONE_NAME_SIZE];
        memcpy(name, start, end - start);
        name[end - start] = 0;
        *found = true;
        return time_zone_lookup(name, zone);
    }
    return true;
}

bool calendar_ics_stamp(const char *params, const char *value,
                        struct calendar_stamp *stamp)
{
    size_t len = strlen(value);
    bool date = len == 8, utc = len == 16 && value[15] == 'Z';
    if (!date && !(len == 15 || utc))
        return false;
    int year = digits(value, 4), month = digits(value + 4, 2);
    int day = digits(value + 6, 2);
    int hour = date ? 0 : digits(value + 9, 2);
    int min = date ? 0 : digits(value + 11, 2);
    int sec = date ? 0 : digits(value + 13, 2);
    if (year < 2000 || year > 2037 || month < 1 || month > 12 ||
        day < 1 || day > 31 || (!date && value[8] != 'T') ||
        hour < 0 || hour > 23 || min < 0 || min > 59 || sec < 0 || sec > 59)
        return false;

    struct tm tm = { .tm_year = year - 1900, .tm_mon = month - 1,
                     .tm_mday = day, .tm_hour = hour, .tm_min = min,
                     .tm_sec = sec };
    time_t civil = clock_civil_seconds(&tm);
    struct tm checked;
    gmtime_r(&civil, &checked);
    if (checked.tm_mday != day)
        return false;

    struct time_zone zone;
    bool zoned;
    if (!calendar_ics_zone(params, &zone, &zoned) || (zoned && (date || utc)))
        return false;
    time_t instant = zoned ? time_zone_to_utc(&zone, &tm) : civil;
    if (instant < 0)
        return false;
    *stamp = (struct calendar_stamp){instant, utc || zoned, date};
    return true;
}

static void summary(char *title, const char *value, size_t size)
{
    char *start = title, *end = title + size - 1;
    while (*value && title < end)
    {
        if (*value == '\\' && value[1])
        {
            value++;
            *title++ = *value == 'n' || *value == 'N' ? ' ' : *value;
        }
        else
            *title++ = *value;
        value++;
    }
    /* Drop a partial UTF-8 sequence at the end of a clipped title. */
    while (title > start && *value &&
           ((unsigned char)*value & 0xc0) == 0x80)
    {
        value--;
        title--;
    }
    *title = 0;
}

struct parser {
    int depth;
    bool start, end, invalid, recurring;
    struct ics_event event;
};

static void line(struct parser *p, char *text)
{
    if (!strncasecmp(text, "BEGIN:", 6))
    {
        if (p->depth)
            p->depth++;
        else if (!strcasecmp(text + 6, "VEVENT"))
            *p = (struct parser){ .depth = 1 };
        return;
    }
    if (!strncasecmp(text, "END:", 4))
    {
        if (p->depth == 1 && !strcasecmp(text + 4, "VEVENT") &&
            p->start && !p->invalid && !p->recurring &&
            event_count < ICS_EVENTS)
        {
            bool same_kind = p->event.start.absolute == p->event.end.absolute &&
                             p->event.start.all_day == p->event.end.all_day;
            if (!p->end || (same_kind &&
                            p->event.end.seconds > p->event.start.seconds))
            {
                if (!p->event.title[0])
                    strlcpy(p->event.title, "Event", sizeof(p->event.title));
                events[event_count++] = p->event;
            }
        }
        if (p->depth)
            p->depth--;
        return;
    }
    if (p->depth != 1)
        return;
    char *value = strchr(text, ':');
    if (!value)
        return;
    *value++ = 0;
    size_t key = strcspn(text, ";");
    if (key == 7 && !strncasecmp(text, "DTSTART", key))
    {
        p->start = calendar_ics_stamp(text + key, value, &p->event.start);
        p->invalid |= !p->start;
    }
    else if (key == 5 && !strncasecmp(text, "DTEND", key))
    {
        p->end = calendar_ics_stamp(text + key, value, &p->event.end);
        p->invalid |= !p->end;
    }
    else if (key == 7 && !strncasecmp(text, "SUMMARY", key))
        summary(p->event.title, value, sizeof(p->event.title));
    else if ((key == 5 && !strncasecmp(text, "RRULE", key)) ||
             (key == 5 && !strncasecmp(text, "RDATE", key)))
        p->recurring = true;
}

static int display_day(struct calendar_stamp stamp, bool exclusive)
{
    if (exclusive)
        stamp.seconds--;
    if (stamp.absolute)
    {
        struct tm local;
        clock_localtime(stamp.seconds, &local);
        stamp.seconds = clock_civil_seconds(&local);
    }
    return stamp.seconds / ICS_DAY;
}

static void refresh_days(void)
{
    for (int i = 0; i < event_count; i++)
    {
        struct ics_event *e = &events[i];
        e->first = display_day(e->start, false);
        e->last = e->end.seconds ? display_day(e->end, true) : e->first;
    }
    zone_generation = time_zone_generation();
}

void calendar_ics_load(void)
{
    event_count = 0;
    int fd = open(ICS_FILE, O_RDONLY);
    if (fd < 0)
        return;

    struct parser p = {0};
    char logical[400] = "", physical[400];
    bool clipped = false, continue_read = false;
    while (event_count < ICS_EVENTS &&
           read_line(fd, physical, sizeof(physical)) > 0)
    {
        bool more = strlen(physical) == sizeof(physical) - 1;
        if (continue_read)
        {
            continue_read = more;
            continue;
        }
        bool folded = physical[0] == ' ' || physical[0] == '\t';
        if (!folded)
        {
            if (!clipped)
                line(&p, logical);
            strlcpy(logical, physical, sizeof(logical));
            clipped = false;
        }
        else if (strlcat(logical, physical + 1, sizeof(logical)) >=
                 sizeof(logical))
            clipped = true;
        if (more)
        {
            continue_read = clipped = true;
            if (p.depth == 1)
                p.invalid = true;
        }
    }
    if (!clipped)
        line(&p, logical);
    close(fd);
    refresh_days();
}

bool calendar_ics_on(int year, int month, int day,
                     void (*found)(const char *, void *), void *data)
{
    if (zone_generation != time_zone_generation())
        refresh_days();
    struct tm civil = { .tm_year = year - 1900, .tm_mon = month - 1,
                        .tm_mday = day };
    int date = clock_civil_seconds(&civil) / ICS_DAY;
    bool any = false;
    for (int i = 0; i < event_count; i++)
    {
        const struct ics_event *e = &events[i];
        if (date < e->first || date > e->last)
            continue;
        if (!found)
            return true;
        any = true;
        char label[64];
        if (e->start.all_day || date > e->first)
            found(e->title, data);
        else
        {
            struct tm local;
            if (e->start.absolute)
                clock_localtime(e->start.seconds, &local);
            else
                gmtime_r(&e->start.seconds, &local);
            snprintf(label, sizeof(label), "%02u:%02u %.55s",
                     (unsigned)local.tm_hour, (unsigned)local.tm_min, e->title);
            found(label, data);
        }
    }
    return any;
}
