/* AstraPod POSIX TZ rules. GPL version 2 or later. */
#include <stdint.h>
#include <string.h>
#include "timefuncs.h"
#include "system/time_zone.h"

static bool digit(char c)
{
    return c >= '0' && c <= '9';
}

static bool alpha(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static bool name(const char **p)
{
    const char *start;
    if (**p == '<')
    {
        start = ++*p;
        while (alpha(**p) || digit(**p) || **p == '+' || **p == '-')
            ++*p;
        if (**p != '>' || *p - start < 3)
            return false;
        ++*p;
        return true;
    }
    start = *p;
    while (alpha(**p))
        ++*p;
    return *p - start >= 3;
}

static bool number(const char **p, int lo, int hi, int *out)
{
    if (!digit(**p))
        return false;
    int n = 0;
    while (digit(**p) && n <= hi)
        n = n * 10 + *(*p)++ - '0';
    if (n < lo || n > hi)
        return false;
    *out = n;
    return true;
}

static bool take(const char **p, char c)
{
    if (**p != c)
        return false;
    ++*p;
    return true;
}

static bool hms(const char **p, int max_hours, int *out)
{
    int sign = take(p, '-') ? -1 : 1;
    if (sign > 0)
        take(p, '+');
    int h, m = 0, s = 0;
    if (!number(p, 0, max_hours, &h))
        return false;
    if (take(p, ':') &&
        (!number(p, 0, 59, &m) ||
         (take(p, ':') && !number(p, 0, 59, &s))))
        return false;
    *out = sign * (h * 3600 + m * 60 + s);
    return true;
}

static bool day_rule(const char **p, struct zone_day_rule *r)
{
    r->type = 'n';
    r->seconds = 7200;
    if (take(p, 'M'))
    {
        r->type = 'M';
        if (!number(p, 1, 12, &r->month) || !take(p, '.') ||
            !number(p, 1, 5, &r->week) || !take(p, '.') ||
            !number(p, 0, 6, &r->weekday))
            return false;
    }
    else
    {
        if (take(p, 'J'))
            r->type = 'J';
        if (!number(p, r->type == 'J', 365,
                    &r->day))
            return false;
    }
    return !take(p, '/') || hms(p, 167, &r->seconds);
}

bool zone_rule_parse(const char *text, struct zone_rule *rule)
{
    struct zone_rule r = {0};
    if (!name(&text) || !hms(&text, 24, &r.standard))
        return false;
    r.standard = -r.standard;
    if (*text)
    {
        if (!name(&text))
            return false;
        r.seasonal = true;
        r.summer = r.standard + 3600;
        if (*text != ',')
        {
            if (!hms(&text, 24, &r.summer))
                return false;
            r.summer = -r.summer;
        }
        if (!take(&text, ',') || !day_rule(&text, &r.start) ||
            !take(&text, ',') || !day_rule(&text, &r.end) || *text)
            return false;
    }
    *rule = r;
    return true;
}

static int64_t transition(int year, const struct zone_day_rule *r, int offset)
{
    struct tm civil = { .tm_year = year, .tm_mday = 1 };
    if (r->type == 'M')
    {
        civil.tm_mon = r->month - 1;
        int weekday = (clock_civil_seconds(&civil) / 86400 + 4) % 7;
        int day = 1 + (r->weekday - weekday + 7) % 7 + 7 * (r->week - 1);
        int64_t start = clock_civil_seconds(&civil);
        civil.tm_mon++;
        if (day > (clock_civil_seconds(&civil) - start) / 86400)
            day -= 7;
        return start + (day - 1) * 86400 + r->seconds - offset;
    }
    int day = r->day;
    if (r->type == 'J')
    {
        day--;
        int y = year + 1900;
        if (day >= 59 && y % 4 == 0 && (y % 100 || y % 400 == 0))
            day++;
    }
    return clock_civil_seconds(&civil) + day * 86400 + r->seconds - offset;
}

int zone_rule_offset(const struct zone_rule *r, time_t utc, int *dst)
{
    *dst = 0;
    if (!r->seasonal)
        return r->standard;

    struct tm tm;
    gmtime_r(&utc, &tm);
    int64_t last = INT64_MIN;
    for (int y = tm.tm_year - 1; y <= tm.tm_year + 1; y++)
    {
        int64_t start = transition(y, &r->start, r->standard);
        int64_t end = transition(y, &r->end, r->summer);
        if (start <= utc && start > last)
        {
            last = start;
            *dst = 1;
        }
        if (end <= utc && end > last)
        {
            last = end;
            *dst = 0;
        }
    }
    return *dst ? r->summer : r->standard;
}
