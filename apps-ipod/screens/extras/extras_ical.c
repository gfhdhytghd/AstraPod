/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * RFC 5545 iCalendar (VEVENT and VTODO) scanner, recurrence matcher,
 * loader, and lossless atomic editor for /Calendars .ics files.
 ****************************************************************************/
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "extras_ical.h"

#ifdef EXTRAS_HOST_TEST
#include <dirent.h>
#include <fcntl.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static size_t ical_strlcpy(char *dst, const char *src, size_t size)
{
    size_t len = strlen(src);
    if (size)
    {
        size_t copy = len < size - 1 ? len : size - 1;
        memcpy(dst, src, copy);
        dst[copy] = '\0';
    }
    return len;
}
#define strlcpy ical_strlcpy
#else
#include "config.h"
#include "system.h"
#include "string-extra.h"
#include "dir.h"
#include "file.h"
#endif

#define ICAL_LINE_BUF 1024
#define POOL_NONE     0xffffu

/* ---- Property parsing helpers ---- */

static uint16_t pool_add(struct ical_index *idx, const char *s)
{
    if (!s || !*s)
        return POOL_NONE;
    size_t len = strlen(s) + 1;
    if ((size_t)idx->pool_used + len > ICAL_POOL_BYTES)
        return POOL_NONE;
    uint16_t off = idx->pool_used;
    memcpy(idx->pool + off, s, len);
    idx->pool_used = (uint16_t)(off + len);
    return off;
}

const char *ical_pool_str(const struct ical_index *idx, uint16_t off)
{
    return off == POOL_NONE ? "" : idx->pool + off;
}

void ical_file_path(const struct ical_index *idx, int file_idx,
                    char *buf, size_t size)
{
    if (file_idx < 0 || file_idx >= idx->file_count)
    {
        if (size)
            buf[0] = '\0';
        return;
    }
    snprintf(buf, size, "%s/%s", idx->dir,
             ical_pool_str(idx, idx->file_off[file_idx]));
}

/* Split "NAME;PARAM1=V1;PARAM2=V2:VALUE" in place. */
static void split_ical_prop(char *line, char **name, char **params,
                            char **value)
{
    char *colon = extras_param_separator(line, ':');
    if (colon)
    {
        *colon = '\0';
        *value = colon + 1;
    }
    else
        *value = line + strlen(line);

    char *semi = strchr(line, ';');
    if (semi)
    {
        *semi = '\0';
        *params = semi + 1;
    }
    else
        *params = NULL;
    *name = line;
}

/* Extract a parameter value (e.g. TZID=America/New_York) from params string. */
static bool get_param(const char *params, const char *key,
                      char *out, size_t out_size)
{
    if (!params || !*params)
        return false;
    size_t klen = strlen(key);
    const char *p = params;
    while (*p)
    {
        const char *semi = extras_param_separator((char *)p, ';');
        size_t seg = semi ? (size_t)(semi - p) : strlen(p);
        if (seg > klen + 1 && !strncasecmp(p, key, klen) && p[klen] == '=')
        {
            const char *v = p + klen + 1;
            size_t vlen = seg - (klen + 1);
            if (vlen >= 2 && v[0] == '"' && v[vlen - 1] == '"')
            {
                v++;
                vlen -= 2;
            }
            if (vlen >= out_size)
                vlen = out_size - 1;
            memcpy(out, v, vlen);
            out[vlen] = '\0';
            return true;
        }
        if (!semi)
            break;
        p = semi + 1;
    }
    return false;
}

static int parse_digits(const char *s, int count)
{
    int v = 0;
    for (int i = 0; i < count; i++)
    {
        if (s[i] < '0' || s[i] > '9')
            return -1;
        v = v * 10 + (s[i] - '0');
    }
    return v;
}

/* Parse YYYYMMDD or YYYYMMDDTHHMMSS[Z]. */
static bool parse_ical_dt(const char *params, const char *val,
                          struct extras_date *date, int *hour, int *min,
                          bool *is_date_only, enum ical_time_mode *mode,
                          char *tzid, size_t tzid_size)
{
    while (*val == ' ' || *val == '\t')
        val++;
    size_t len = strlen(val);
    if (len < 8)
        return false;
    int y = parse_digits(val, 4);
    int m = parse_digits(val + 4, 2);
    int d = parse_digits(val + 6, 2);
    if (y < 1 || m < 1 || m > 12 || d < 1 ||
        d > extras_month_days(y, m))
        return false;
    date->year = y;
    date->month = m;
    date->day = d;

    char pval[64];
    bool val_date = get_param(params, "VALUE", pval, sizeof(pval)) &&
                    !strcasecmp(pval, "DATE");

    if (val_date || len == 8)
    {
        if (len != 8)
            return false;
        if (hour) *hour = 0;
        if (min) *min = 0;
        if (is_date_only) *is_date_only = true;
        if (mode) *mode = ICAL_TIME_DATE;
        if (tzid && tzid_size) tzid[0] = '\0';
        return true;
    }

    if ((len != 15 && len != 16) ||
        (val[8] != 'T' && val[8] != 't') ||
        (len == 16 && val[15] != 'Z' && val[15] != 'z'))
        return false;
    int H = parse_digits(val + 9, 2);
    int M = parse_digits(val + 11, 2);
    int S = parse_digits(val + 13, 2);
    if (H < 0 || H > 23 || M < 0 || M > 59 || S < 0 || S > 60)
        return false;
    if (hour) *hour = H;
    if (min) *min = M;
    if (is_date_only) *is_date_only = false;

    if (len == 16)
    {
        if (mode) *mode = ICAL_TIME_UTC;
        if (tzid && tzid_size) tzid[0] = '\0';
    }
    else if (tzid && tzid_size && get_param(params, "TZID", tzid, tzid_size))
    {
        if (mode) *mode = ICAL_TIME_TZID;
    }
    else if (get_param(params, "TZID", pval, sizeof(pval)))
    {
        if (mode) *mode = ICAL_TIME_TZID;
    }
    else
    {
        if (mode) *mode = ICAL_TIME_FLOATING;
        if (tzid && tzid_size) tzid[0] = '\0';
    }
    return true;
}

/* Parse ISO-8601 duration in minutes (signed, for TRIGGER and DURATION). */
static bool parse_duration_minutes(const char *s, int *out_minutes)
{
    int sign = 1;
    while (*s == ' ')
        s++;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') { s++; }
    if (*s != 'P' && *s != 'p')
        return false;
    s++;
    int total = 0;
    bool in_time = false;
    bool any = false;
    while (*s)
    {
        if (*s == 'T' || *s == 't')
        {
            in_time = true;
            s++;
            continue;
        }
        if (*s < '0' || *s > '9')
            break;
        int n = 0;
        while (*s >= '0' && *s <= '9')
            n = n * 10 + (*s++ - '0');
        any = true;
        if (*s == 'W' || *s == 'w') { total += n * 7 * 24 * 60; s++; }
        else if (*s == 'D' || *s == 'd') { total += n * 24 * 60; s++; }
        else if (*s == 'H' || *s == 'h') { total += n * 60; s++; }
        else if (*s == 'M' || *s == 'm') { total += in_time ? n : n * 30 * 24 * 60; s++; }
        else if (*s == 'S' || *s == 's') { total += n / 60; s++; }
        else break;
    }
    if (!any)
        return false;
    *out_minutes = sign * total;
    return true;
}

static enum ical_alarm trigger_to_alarm(const char *val)
{
    int mins = 0;
    if (!parse_duration_minutes(val, &mins))
        return ICAL_ALARM_NONE;
    if (mins > 0)
        return ICAL_ALARM_AT_TIME;
    int before = -mins;
    if (before == 0) return ICAL_ALARM_AT_TIME;
    if (before <= 5) return ICAL_ALARM_5M;
    if (before <= 15) return ICAL_ALARM_15M;
    if (before <= 30) return ICAL_ALARM_30M;
    if (before <= 60) return ICAL_ALARM_1H;
    return ICAL_ALARM_1D;
}

static const char *alarm_to_trigger(enum ical_alarm a)
{
    switch (a)
    {
        case ICAL_ALARM_AT_TIME: return "PT0S";
        case ICAL_ALARM_5M:      return "-PT5M";
        case ICAL_ALARM_15M:     return "-PT15M";
        case ICAL_ALARM_30M:     return "-PT30M";
        case ICAL_ALARM_1H:      return "-PT1H";
        case ICAL_ALARM_1D:      return "-P1D";
        default:                 return NULL;
    }
}

static const char *repeat_to_rrule(enum ical_repeat r)
{
    switch (r)
    {
        case ICAL_REPEAT_DAILY:   return "FREQ=DAILY";
        case ICAL_REPEAT_WEEKLY:  return "FREQ=WEEKLY";
        case ICAL_REPEAT_MONTHLY: return "FREQ=MONTHLY";
        case ICAL_REPEAT_YEARLY:  return "FREQ=YEARLY";
        default:                  return NULL;
    }
}

static int rule_weekday(const char *value)
{
    static const char days[][3] = { "SU", "MO", "TU", "WE", "TH", "FR", "SA" };
    for (int i = 0; i < 7; i++)
        if (!strcasecmp(value, days[i]))
            return i;
    return -1;
}

static void parse_rule_part(struct ical_event_idx *e, const char *key, char *v)
{
    if (!strcasecmp(key, "FREQ"))
    {
        static const char *const names[] = {
            "", "DAILY", "WEEKLY", "MONTHLY", "YEARLY"
        };
        for (unsigned i = 1; i < ARRAYLEN(names); i++)
            if (!strcasecmp(v, names[i]))
                e->repeat = i;
    }
    else if (!strcasecmp(key, "UNTIL"))
    {
        e->has_until = parse_ical_dt(NULL, v, &e->until_date,
                                    NULL, NULL, NULL, NULL, NULL, 0);
    }
    else if (!strcasecmp(key, "WKST"))
    {
        int weekday = rule_weekday(v);
        if (weekday >= 0)
            e->week_start = weekday;
    }
    else if (!strcasecmp(key, "INTERVAL") || !strcasecmp(key, "COUNT"))
    {
        char *end;
        unsigned long n = strtoul(v, &end, 10);
        if (*v >= '0' && *v <= '9' && !*end && n > 0 && n <= INT32_MAX)
        {
            if (!strcasecmp(key, "INTERVAL"))
                e->interval = n;
            else
                e->rrule_count = n;
        }
    }
    else
    {
        while (v && *v)
        {
            char *comma = strchr(v, ',');
            if (comma) *comma = '\0';
            char *end;
            long n = strtol(v, &end, 10);

            if (!strcasecmp(key, "BYDAY"))
            {
                int wd = rule_weekday(end);
                if (wd >= 0 && n >= -53 && n <= 53)
                {
                    if (!n)
                        e->byday_mask |= 1u << wd;
                    else if (e->ordinal_count < ARRAYLEN(e->ordinal_days))
                        e->ordinal_days[e->ordinal_count++] = n * 8 + wd;
                }
            }
            else if (!*end && !strcasecmp(key, "BYMONTH") && n > 0 && n <= 12)
                e->bymonth_mask |= 1u << (n - 1);
            else if (!*end && !strcasecmp(key, "BYMONTHDAY") && n && n >= -31 && n <= 31)
            {
                if (n > 0) e->monthdays |= 1u << (n - 1);
                else e->reverse_monthdays |= 1u << (-n - 1);
            }
            else if (!*end && !strcasecmp(key, "BYSETPOS") && n && n >= -366 &&
                     n <= 366 && e->position_count < ARRAYLEN(e->positions))
                e->positions[e->position_count++] = n;
            v = comma ? comma + 1 : NULL;
        }
    }
}

static void parse_rrule(char *value, struct ical_event_idx *e)
{
    e->interval = 1;
    e->week_start = 1; /* Monday */
    for (char *part = value; part && *part; )
    {
        char *semi = strchr(part, ';');
        if (semi) *semi = '\0';
        char *eq = strchr(part, '=');
        if (eq)
        {
            *eq = '\0';
            parse_rule_part(e, part, eq + 1);
        }
        part = semi ? semi + 1 : NULL;
    }
}

static void scan_recurrence_dates(struct ical_index *idx, const char *params,
                                  char *value, bool excluded)
{
    for (char *item = value; item && *item && idx->xdate_count < ICAL_MAX_XDATES; )
    {
        char *comma = strchr(item, ',');
        if (comma) *comma = '\0';
        char *period = strchr(item, '/');
        if (period) *period++ = '\0';
        struct ical_xdate date = { .excluded = excluded };
        int hour, min;
        if (parse_ical_dt(params, item, &date.date, &hour, &min,
                          NULL, NULL, NULL, 0))
        {
            if (period && !excluded)
            {
                int duration, end_hour, end_min;
                struct extras_date end;
                if (parse_duration_minutes(period, &duration) && duration > 0)
                    date.span_days = (hour * 60 + min + duration + 1439) / 1440;
                else if (parse_ical_dt(params, period, &end, &end_hour,
                                        &end_min, NULL, NULL, NULL, 0))
                    date.span_days = extras_date_days_between(&date.date, &end)
                                     + (end_hour != 0 || end_min != 0);
                if (date.span_days < 1) date.span_days = 1;
            }
            idx->xdates[idx->xdate_count++] = date;
        }
        item = comma ? comma + 1 : NULL;
    }
}

/* ---- Scanning ---- */

static bool path_is_dir(const char *dir_path,
#ifndef EXTRAS_HOST_TEST
                        DIR *dir, struct dirent *entry
#else
                        const char *name
#endif
                        )
{
#ifdef EXTRAS_HOST_TEST
    char full[MAX_PATH];
    struct stat st;
    snprintf(full, sizeof(full), "%s/%s", dir_path, name);
    return stat(full, &st) == 0 && S_ISDIR(st.st_mode);
#else
    (void)dir_path;
    return (dir_get_info(dir, entry).attribute & ATTR_DIRECTORY) != 0;
#endif
}

static void scan_ics_file(struct ical_index *idx, int file_idx)
{
    char path[MAX_PATH];
    char line[ICAL_LINE_BUF];
    struct extras_reader r;
    uint32_t raw_start = 0, raw_end = 0;

    ical_file_path(idx, file_idx, path, sizeof(path));
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return;

    extras_reader_init(&r, fd, 0);

    enum { COMP_NONE, COMP_EVENT, COMP_TODO } comp = COMP_NONE;
    int valarm_depth = 0;
    uint32_t comp_start = 0;
    bool have_start = false, have_end = false;
    struct ical_event_idx ev;
    struct ical_todo_idx td;
    struct extras_date end_dt = {0};
    int end_h = 0, end_m = 0;
    bool end_is_date = false;
    int dur_mins = 0;
    bool have_dur = false;
    char summary[160];

    while (extras_read_prop(&r, line, sizeof(line), &raw_start, &raw_end,
                            false))
    {
        char *name, *params, *val;
        split_ical_prop(line, &name, &params, &val);

        if (!strcasecmp(name, "BEGIN"))
        {
            char *v = extras_trim(val);
            if (comp == COMP_NONE && !strcasecmp(v, "VEVENT"))
            {
                comp = COMP_EVENT;
                comp_start = raw_start;
                valarm_depth = 0;
                memset(&ev, 0, sizeof(ev));
                ev.file_idx = (uint8_t)file_idx;
                ev.interval = 1;
                ev.xdate_start = idx->xdate_count;
                ev.span_days = 1;
                have_start = have_end = have_dur = false;
                summary[0] = '\0';
            }
            else if (comp == COMP_NONE && !strcasecmp(v, "VTODO"))
            {
                comp = COMP_TODO;
                comp_start = raw_start;
                valarm_depth = 0;
                memset(&td, 0, sizeof(td));
                td.file_idx = (uint8_t)file_idx;
                summary[0] = '\0';
            }
            else if (comp != COMP_NONE && !strcasecmp(v, "VALARM"))
            {
                valarm_depth++;
            }
            continue;
        }
        if (!strcasecmp(name, "END"))
        {
            char *v = extras_trim(val);
            if (valarm_depth > 0 && !strcasecmp(v, "VALARM"))
            {
                valarm_depth--;
            }
            else if (comp == COMP_EVENT && !strcasecmp(v, "VEVENT"))
            {
                if (have_start && idx->event_count < ICAL_MAX_EVENTS)
                {
                    ev.comp_start = comp_start;
                    ev.comp_end = raw_end;
                    if (have_end)
                    {
                        int diff = extras_date_days_between(&ev.start_date,
                                                            &end_dt);
                        if (ev.all_day || end_is_date)
                            ev.span_days = diff >= 1 ? diff : 1;
                        else
                        {
                            ev.end_hour = (uint8_t)end_h;
                            ev.end_min = (uint8_t)end_m;
                            ev.span_days = diff >= 0 ? diff + 1 : 1;
                            if (diff > 0 && end_h == 0 && end_m == 0)
                                ev.span_days = diff;
                        }
                    }
                    else if (have_dur && dur_mins > 0)
                    {
                        if (ev.all_day)
                            ev.span_days = MAX(1, dur_mins / (24 * 60));
                        else
                        {
                            int tot = ev.start_hour * 60 + ev.start_min +
                                      dur_mins;
                            ev.end_hour = (uint8_t)((tot / 60) % 24);
                            ev.end_min = (uint8_t)(tot % 60);
                            ev.span_days = MAX(1, tot / (24 * 60) + 1);
                        }
                    }
                    else if (!ev.all_day)
                    {
                        int tot = ev.start_hour * 60 + ev.start_min + 60;
                        ev.end_hour = (uint8_t)((tot / 60) % 24);
                        ev.end_min = (uint8_t)(tot % 60);
                    }
                    ev.xdate_end = idx->xdate_count;
                    ev.summary_off = pool_add(idx, summary);
                    idx->events[idx->event_count++] = ev;
                }
                else
                    idx->xdate_count = ev.xdate_start;
                comp = COMP_NONE;
            }
            else if (comp == COMP_TODO && !strcasecmp(v, "VTODO"))
            {
                if (idx->todo_count < ICAL_MAX_TODOS)
                {
                    td.comp_start = comp_start;
                    td.comp_end = raw_end;
                    td.summary_off = pool_add(idx, summary);
                    idx->todos[idx->todo_count++] = td;
                }
                comp = COMP_NONE;
            }
            continue;
        }

        if (comp == COMP_EVENT)
        {
            if (valarm_depth > 0)
            {
                if (!strcasecmp(name, "TRIGGER") &&
                    ev.alarm == ICAL_ALARM_NONE)
                    ev.alarm = (uint8_t)trigger_to_alarm(val);
                continue;
            }
            if (!strcasecmp(name, "SUMMARY"))
            {
                extras_unescape_text(val);
                strlcpy(summary, extras_trim(val), sizeof(summary));
            }
            else if (!strcasecmp(name, "DTSTART"))
            {
                int H = 0, M = 0;
                bool is_date = false;
                if (parse_ical_dt(params, val, &ev.start_date, &H, &M,
                                  &is_date, NULL, NULL, 0))
                {
                    ev.all_day = is_date ? 1 : 0;
                    ev.start_hour = (uint8_t)H;
                    ev.start_min = (uint8_t)M;
                    have_start = true;
                }
            }
            else if (!strcasecmp(name, "DTEND"))
            {
                if (parse_ical_dt(params, val, &end_dt, &end_h, &end_m,
                                  &end_is_date, NULL, NULL, 0))
                    have_end = true;
            }
            else if (!strcasecmp(name, "DURATION"))
            {
                if (parse_duration_minutes(val, &dur_mins))
                    have_dur = true;
            }
            else if (!strcasecmp(name, "RRULE"))
                parse_rrule(val, &ev);
            else if (!strcasecmp(name, "EXDATE"))
                scan_recurrence_dates(idx, params, val, true);
            else if (!strcasecmp(name, "RDATE"))
                scan_recurrence_dates(idx, params, val, false);
        }
        else if (comp == COMP_TODO && valarm_depth == 0)
        {
            if (!strcasecmp(name, "SUMMARY"))
            {
                extras_unescape_text(val);
                strlcpy(summary, extras_trim(val), sizeof(summary));
            }
            else if (!strcasecmp(name, "DUE"))
            {
                if (parse_ical_dt(params, val, &td.due_date, NULL, NULL,
                                  NULL, NULL, NULL, 0))
                    td.has_due = 1;
            }
            else if (!strcasecmp(name, "PRIORITY"))
            {
                int p = atoi(extras_trim(val));
                if (p >= 0 && p <= 9)
                    td.priority = (uint8_t)p;
            }
            else if (!strcasecmp(name, "STATUS"))
            {
                if (!strcasecmp(extras_trim(val), "COMPLETED"))
                    td.completed = 1;
            }
        }
    }
    close(fd);
}

static const struct ical_index *sort_idx;

static int cmp_file_off(const void *a, const void *b)
{
    uint16_t oa = *(const uint16_t *)a;
    uint16_t ob = *(const uint16_t *)b;
    return strcasecmp(ical_pool_str(sort_idx, oa),
                      ical_pool_str(sort_idx, ob));
}

void ical_scan(struct ical_index *idx, const char *dir_path)
{
    memset(idx, 0, sizeof(*idx));
    strlcpy(idx->dir, dir_path ? dir_path : ICAL_DIR, sizeof(idx->dir));

    DIR *dir = opendir(idx->dir);
    if (!dir)
        return;
    while (idx->file_count < ICAL_MAX_FILES)
    {
        struct dirent *entry = readdir(dir);
        if (!entry)
            break;
        const char *name = (const char *)entry->d_name;
#ifdef EXTRAS_HOST_TEST
        if (path_is_dir(idx->dir, name) || !extras_has_ext(name, ".ics"))
            continue;
#else
        if (path_is_dir(idx->dir, dir, entry) || !extras_has_ext(name, ".ics"))
            continue;
#endif
        uint16_t off = pool_add(idx, name);
        if (off != POOL_NONE)
            idx->file_off[idx->file_count++] = off;
    }
    closedir(dir);

    sort_idx = idx;
    if (idx->file_count > 1)
        qsort(idx->file_off, (size_t)idx->file_count,
              sizeof(idx->file_off[0]), cmp_file_off);

    for (int f = 0; f < idx->file_count; f++)
        scan_ics_file(idx, f);
}

/* ---- Recurrence & day query ---- */

static bool rule_day_matches(const struct ical_event_idx *e,
                             const struct extras_date *d)
{
    int month_length = extras_month_days(d->year, d->month);
    if (e->bymonth_mask && !(e->bymonth_mask & (1u << (d->month - 1))))
        return false;
    if ((e->monthdays || e->reverse_monthdays) &&
        !(e->monthdays & (1u << (d->day - 1))) &&
        !(e->reverse_monthdays & (1u << (month_length - d->day))))
        return false;

    if (e->byday_mask || e->ordinal_count)
    {
        int wd = extras_week_day(d);
        if (e->byday_mask & (1u << wd))
            return true;

        int forward = d->day - 1;
        int backward = month_length - d->day;
        if (e->repeat == ICAL_REPEAT_YEARLY && !e->bymonth_mask)
        {
            struct extras_date jan = {d->year, 1, 1};
            forward = extras_date_days_between(&jan, d);
            backward = 364 + extras_leap_year(d->year) - forward;
        }
        int first = (forward / 7 + 1) * 8 + wd;
        int last = -(backward / 7 + 1) * 8 + wd;
        for (int i = 0; i < e->ordinal_count; i++)
            if (e->ordinal_days[i] == first || e->ordinal_days[i] == last)
                return true;
        return false;
    }
    if (e->repeat == ICAL_REPEAT_WEEKLY)
        return extras_week_day(d) == extras_week_day(&e->start_date);
    if (e->repeat == ICAL_REPEAT_YEARLY && !e->bymonth_mask &&
        !e->monthdays && !e->reverse_monthdays && d->month != e->start_date.month)
        return false;
    if (e->repeat >= ICAL_REPEAT_MONTHLY && !e->monthdays &&
        !e->reverse_monthdays)
        return d->day == e->start_date.day;
    return true;
}

static int days_since_week_start(const struct ical_event_idx *e,
                                 const struct extras_date *d)
{
    return (extras_week_day(d) + 7 - e->week_start) % 7;
}

static bool rule_position_matches(const struct ical_event_idx *e,
                                  const struct extras_date *d)
{
    if (!e->position_count)
        return true;

    struct extras_date day = *d;
    int length = 1;
    if (e->repeat == ICAL_REPEAT_WEEKLY)
    {
        extras_step_days(&day, -days_since_week_start(e, d));
        length = 7;
    }
    else if (e->repeat == ICAL_REPEAT_MONTHLY)
    {
        day.day = 1;
        length = extras_month_days(day.year, day.month);
    }
    else if (e->repeat == ICAL_REPEAT_YEARLY)
    {
        day.month = day.day = 1;
        length = 365 + extras_leap_year(day.year);
    }
    int before = 0, after = 0;
    for (int i = 0; i < length; i++, extras_step_days(&day, 1))
    {
        if (!rule_day_matches(e, &day))
            continue;
        int order = extras_date_cmp(&day, d);
        before += order < 0;
        after += order > 0;
    }
    for (int i = 0; i < e->position_count; i++)
        if (e->positions[i] == before + 1 || e->positions[i] == -after - 1)
            return true;
    return false;
}

static bool rule_starts_on(const struct ical_event_idx *e,
                          const struct extras_date *d, int days)
{
    if (days == 0)
        return true; /* DTSTART is the first occurrence. */
    if (days < 0 || !e->repeat ||
        (e->has_until && extras_date_cmp(d, &e->until_date) > 0))
        return false;
    int period = days;
    switch (e->repeat)
    {
        case ICAL_REPEAT_WEEKLY:
            period = (days + days_since_week_start(e, &e->start_date)) / 7;
            break;
        case ICAL_REPEAT_MONTHLY:
            period = (d->year - e->start_date.year) * 12 +
                     d->month - e->start_date.month;
            break;
        case ICAL_REPEAT_YEARLY:
            period = d->year - e->start_date.year;
            break;
    }
    return period % MAX(1u, e->interval) == 0 && rule_day_matches(e, d) &&
           rule_position_matches(e, d);
}

static bool is_excluded(const struct ical_index *idx,
                        const struct ical_event_idx *e,
                        const struct extras_date *date)
{
    for (int i = e->xdate_start; i < e->xdate_end; i++)
        if (idx->xdates[i].excluded &&
            extras_date_cmp(date, &idx->xdates[i].date) == 0)
            return true;
    return false;
}

bool ical_event_occurs_on(const struct ical_index *idx,
                          const struct ical_event_idx *e,
                          const struct extras_date *d)
{
    int span = MAX(1, e->span_days);
    for (int i = e->xdate_start; i < e->xdate_end; i++)
    {
        const struct ical_xdate *x = &idx->xdates[i];
        int offset = extras_date_days_between(&x->date, d);
        if (!x->excluded && offset >= 0 &&
            offset < (x->span_days ? x->span_days : span) &&
            !is_excluded(idx, e, &x->date))
            return true;
    }

    int days = extras_date_days_between(&e->start_date, d);
    if (days < 0)
        return false;
    int first = MAX(0, days - span + 1);
    struct extras_date start = *d;
    extras_step_days(&start, first - days);
    for (int day = first; day <= days; day++, extras_step_days(&start, 1))
    {
        if (!rule_starts_on(e, &start, day) || is_excluded(idx, e, &start))
            continue;
        if (!e->rrule_count || day == 0)
            return true;
        /* Exclusions do not extend COUNT. */
        uint32_t count = 1;
        struct extras_date date = e->start_date;
        for (int n = 1; n < day && count < e->rrule_count; n++)
        {
            extras_step_days(&date, 1);
            count += rule_starts_on(e, &date, n);
        }
        if (count < e->rrule_count)
            return true;
        break;
    }
    return false;
}

bool ical_has_event_on(const struct ical_index *idx,
                       const struct extras_date *d)
{
    for (int i = 0; i < idx->event_count; i++)
        if (ical_event_occurs_on(idx, &idx->events[i], d))
            return true;
    return false;
}

static int cmp_day_event(const void *a, const void *b)
{
    const struct ical_event_idx *ea = &sort_idx->events[*(const int *)a];
    const struct ical_event_idx *eb = &sort_idx->events[*(const int *)b];
    if (ea->all_day != eb->all_day)
        return eb->all_day - ea->all_day; /* all-day first */
    int ta = ea->start_hour * 60 + ea->start_min;
    int tb = eb->start_hour * 60 + eb->start_min;
    if (ta != tb)
        return ta - tb;
    return strcasecmp(ical_pool_str(sort_idx, ea->summary_off),
                      ical_pool_str(sort_idx, eb->summary_off));
}

int ical_events_on_day(const struct ical_index *idx,
                       const struct extras_date *d,
                       int *out_indices, int max_out)
{
    int n = 0;
    for (int i = 0; i < idx->event_count && n < max_out; i++)
        if (ical_event_occurs_on(idx, &idx->events[i], d))
            out_indices[n++] = i;
    sort_idx = idx;
    if (n > 1)
        qsort(out_indices, (size_t)n, sizeof(out_indices[0]), cmp_day_event);
    return n;
}

int ical_priority_to_bucket(int rfc_priority)
{
    if (rfc_priority >= 1 && rfc_priority <= 4) return 3; /* High */
    if (rfc_priority == 5) return 2;                      /* Medium */
    if (rfc_priority >= 6 && rfc_priority <= 9) return 1; /* Low */
    return 0;                                             /* None */
}

int ical_bucket_to_priority(int bucket)
{
    static const int map[4] = { 0, 9, 5, 1 };
    if (bucket < 0 || bucket > 3)
        return 0;
    return map[bucket];
}

/* ---- Loaders ---- */

void ical_event_init_new(struct ical_event *ev, const struct extras_date *d)
{
    memset(ev, 0, sizeof(*ev));
    ev->sequence = -1;
    ev->date = d ? *d : extras_today();
    ev->all_day = false;
    ev->start_hour = 9;
    ev->start_min = 0;
    ev->end_hour = 10;
    ev->end_min = 0;
    ev->span_days = 1;
    const char *tz = extras_device_tzid();
    if (tz && *tz)
    {
        ev->time_mode = ICAL_TIME_TZID;
        strlcpy(ev->tzid, tz, sizeof(ev->tzid));
    }
    else
        ev->time_mode = ICAL_TIME_FLOATING;
    ev->repeat = ICAL_REPEAT_NONE;
    ev->alarm = ICAL_ALARM_NONE;
}

bool ical_load_event(const char *path, uint32_t comp_start, uint32_t comp_end,
                     struct ical_event *ev)
{
    char saved_path[MAX_PATH];
    strlcpy(saved_path, path, sizeof(saved_path));
    memset(ev, 0, sizeof(*ev));
    strlcpy(ev->path, saved_path, sizeof(ev->path));
    ev->comp_start = comp_start;
    ev->comp_end = comp_end;
    ev->sequence = -1;
    ev->span_days = 1;
    ev->is_existing = true;

    int fd = open(ev->path, O_RDONLY);
    if (fd < 0)
        return false;

    struct extras_reader r;
    extras_reader_init(&r, fd, comp_start);
    char line[ICAL_LINE_BUF];
    uint32_t rs = 0, re = 0;
    int valarm_idx = -1;
    int valarm_depth = 0;
    bool have_end = false, have_dur = false;
    struct extras_date end_dt = {0};
    int end_h = 0, end_m = 0, dur_mins = 0;
    bool end_is_date = false;

    while (r.offset < comp_end &&
           extras_read_prop(&r, line, sizeof(line), &rs, &re, false))
    {
        char *name, *params, *val;
        split_ical_prop(line, &name, &params, &val);
        if (!strcasecmp(name, "BEGIN"))
        {
            if (!strcasecmp(extras_trim(val), "VALARM"))
            {
                valarm_depth++;
                valarm_idx++;
            }
            continue;
        }
        if (!strcasecmp(name, "END"))
        {
            char *v = extras_trim(val);
            if (valarm_depth > 0 && !strcasecmp(v, "VALARM"))
                valarm_depth--;
            else if (!strcasecmp(v, "VEVENT"))
                break;
            continue;
        }
        if (valarm_depth > 0)
        {
            if (valarm_idx == 0 && !strcasecmp(name, "TRIGGER"))
                ev->alarm = trigger_to_alarm(val);
            continue;
        }
        if (!strcasecmp(name, "UID"))
            strlcpy(ev->uid, extras_trim(val), sizeof(ev->uid));
        else if (!strcasecmp(name, "SEQUENCE"))
            ev->sequence = atoi(extras_trim(val));
        else if (!strcasecmp(name, "SUMMARY"))
        {
            extras_unescape_text(val);
            strlcpy(ev->summary, extras_trim(val), sizeof(ev->summary));
        }
        else if (!strcasecmp(name, "DESCRIPTION"))
        {
            extras_unescape_text(val);
            strlcpy(ev->notes, val, sizeof(ev->notes));
        }
        else if (!strcasecmp(name, "DTSTART"))
        {
            parse_ical_dt(params, val, &ev->date, &ev->start_hour,
                          &ev->start_min, &ev->all_day, &ev->time_mode,
                          ev->tzid, sizeof(ev->tzid));
        }
        else if (!strcasecmp(name, "DTEND"))
        {
            if (parse_ical_dt(params, val, &end_dt, &end_h, &end_m,
                              &end_is_date, NULL, NULL, 0))
                have_end = true;
        }
        else if (!strcasecmp(name, "DURATION"))
        {
            if (parse_duration_minutes(val, &dur_mins))
                have_dur = true;
        }
        else if (!strcasecmp(name, "RRULE"))
        {
            struct ical_event_idx rule = {0};
            parse_rrule(val, &rule);
            ev->repeat = rule.repeat;
        }
    }
    close(fd);

    if (have_end)
    {
        int diff = extras_date_days_between(&ev->date, &end_dt);
        if (ev->all_day || end_is_date)
        {
            ev->span_days = diff >= 1 ? diff : 1;
            ev->start_hour = 9; ev->start_min = 0;
            ev->end_hour = 10; ev->end_min = 0;
        }
        else
        {
            ev->end_hour = end_h;
            ev->end_min = end_m;
            ev->end_day_offset = MAX(0, diff);
        }
    }
    else if (have_dur && dur_mins > 0)
    {
        if (ev->all_day)
            ev->span_days = MAX(1, dur_mins / (24 * 60));
        else
        {
            int tot = ev->start_hour * 60 + ev->start_min + dur_mins;
            ev->end_hour = (tot / 60) % 24;
            ev->end_min = tot % 60;
            ev->end_day_offset = tot / 1440;
        }
    }
    else if (!ev->all_day)
    {
        ev->end_hour = ev->start_hour;
        ev->end_min = ev->start_min;
    }

    ev->orig_all_day = ev->all_day;
    ev->orig_date = ev->date;
    ev->orig_sh = ev->start_hour;
    ev->orig_sm = ev->start_min;
    ev->orig_eh = ev->end_hour;
    ev->orig_em = ev->end_min;
    ev->orig_repeat = ev->repeat;
    ev->orig_alarm = ev->alarm;
    return true;
}

void ical_todo_init_new(struct ical_todo *td)
{
    memset(td, 0, sizeof(*td));
    td->sequence = -1;
    td->due_date = extras_today();
}

bool ical_load_todo(const char *path, uint32_t comp_start, uint32_t comp_end,
                    struct ical_todo *td)
{
    char saved_path[MAX_PATH];
    strlcpy(saved_path, path, sizeof(saved_path));
    memset(td, 0, sizeof(*td));
    strlcpy(td->path, saved_path, sizeof(td->path));
    td->comp_start = comp_start;
    td->comp_end = comp_end;
    td->sequence = -1;
    td->is_existing = true;

    int fd = open(td->path, O_RDONLY);
    if (fd < 0)
        return false;

    struct extras_reader r;
    extras_reader_init(&r, fd, comp_start);
    char line[ICAL_LINE_BUF];
    uint32_t rs = 0, re = 0;
    int valarm_depth = 0;

    while (r.offset < comp_end &&
           extras_read_prop(&r, line, sizeof(line), &rs, &re, false))
    {
        char *name, *params, *val;
        split_ical_prop(line, &name, &params, &val);
        if (!strcasecmp(name, "BEGIN"))
        {
            if (!strcasecmp(extras_trim(val), "VALARM"))
                valarm_depth++;
            continue;
        }
        if (!strcasecmp(name, "END"))
        {
            char *v = extras_trim(val);
            if (valarm_depth > 0 && !strcasecmp(v, "VALARM"))
                valarm_depth--;
            else if (!strcasecmp(v, "VTODO"))
                break;
            continue;
        }
        if (valarm_depth > 0)
            continue;
        if (!strcasecmp(name, "UID"))
            strlcpy(td->uid, extras_trim(val), sizeof(td->uid));
        else if (!strcasecmp(name, "SEQUENCE"))
            td->sequence = atoi(extras_trim(val));
        else if (!strcasecmp(name, "SUMMARY"))
        {
            extras_unescape_text(val);
            strlcpy(td->summary, extras_trim(val), sizeof(td->summary));
        }
        else if (!strcasecmp(name, "DESCRIPTION"))
        {
            extras_unescape_text(val);
            strlcpy(td->notes, val, sizeof(td->notes));
        }
        else if (!strcasecmp(name, "DUE"))
        {
            if (parse_ical_dt(params, val, &td->due_date, NULL, NULL,
                              NULL, NULL, NULL, 0))
                td->has_due = true;
        }
        else if (!strcasecmp(name, "PRIORITY"))
        {
            int p = atoi(extras_trim(val));
            if (p >= 0 && p <= 9)
                td->priority = p;
        }
        else if (!strcasecmp(name, "STATUS"))
        {
            if (!strcasecmp(extras_trim(val), "COMPLETED"))
                td->completed = true;
        }
    }
    close(fd);

    td->orig_completed = td->completed;
    td->orig_has_due = td->has_due;
    td->orig_due_date = td->due_date;
    td->orig_priority = td->priority;
    return true;
}

/* ---- Writers ---- */

static bool same_text_field(char *value, const char *saved, size_t capacity,
                            bool trim)
{
    extras_unescape_text(value);
    if (trim)
        value = extras_trim(value);
    if (strlen(value) >= capacity)
        value[capacity - 1] = '\0';
    return !strcmp(value, saved);
}

static bool write_event_dt(int fd, const char *prop,
                           const struct extras_date *d, bool all_day,
                           int hour, int min, enum ical_time_mode mode,
                           const char *tzid)
{
    char line[160];
    if (all_day)
    {
        snprintf(line, sizeof(line), "%s;VALUE=DATE:%04d%02d%02d",
                 prop, d->year, d->month, d->day);
        return extras_write_folded(fd, line);
    }
    if (mode == ICAL_TIME_TZID && tzid && *tzid)
    {
        const char *quote = strpbrk(tzid, ":;,") ? "\"" : "";
        snprintf(line, sizeof(line),
                 "%s;TZID=%s%s%s:%04d%02d%02dT%02d%02d00",
                 prop, quote, tzid, quote, d->year, d->month, d->day,
                 hour, min);
    }
    else if (mode == ICAL_TIME_UTC)
    {
        snprintf(line, sizeof(line), "%s:%04d%02d%02dT%02d%02d00Z",
                 prop, d->year, d->month, d->day, hour, min);
    }
    else
    {
        snprintf(line, sizeof(line), "%s:%04d%02d%02dT%02d%02d00",
                 prop, d->year, d->month, d->day, hour, min);
    }
    return extras_write_folded(fd, line);
}

static bool write_valarm(int fd, enum ical_alarm alarm, const char *summary)
{
    const char *trig = alarm_to_trigger(alarm);
    if (!trig)
        return true;
    char line[128];
    if (!extras_write_folded(fd, "BEGIN:VALARM"))
        return false;
    if (!extras_write_folded(fd, "ACTION:DISPLAY"))
        return false;
    if (!extras_write_text_prop(fd, "DESCRIPTION:",
                                summary[0] ? summary : "Reminder"))
        return false;
    snprintf(line, sizeof(line), "TRIGGER:%s", trig);
    if (!extras_write_folded(fd, line))
        return false;
    return extras_write_folded(fd, "END:VALARM");
}

static struct extras_date event_end_date(const struct ical_event *ev)
{
    struct extras_date date = ev->date;
    int offset = ev->end_day_offset;
    if (ev->all_day)
        offset = MAX(1, ev->span_days);
    else if (!offset && ev->end_hour * 60 + ev->end_min <
                       ev->start_hour * 60 + ev->start_min)
        offset = 1;
    extras_step_days(&date, offset);
    return date;
}

static bool write_new_event_body(int fd, const struct ical_event *ev,
                                 const char *now_stamp)
{
    char line[192];
    const char *title = ev->summary[0] ? ev->summary : "Untitled";

    if (!extras_write_folded(fd, "BEGIN:VEVENT"))
        return false;
    snprintf(line, sizeof(line), "UID:%s", ev->uid);
    if (!extras_write_folded(fd, line))
        return false;
    snprintf(line, sizeof(line), "DTSTAMP:%s", now_stamp);
    if (!extras_write_folded(fd, line))
        return false;
    snprintf(line, sizeof(line), "LAST-MODIFIED:%s", now_stamp);
    if (!extras_write_folded(fd, line))
        return false;
    if (!extras_write_folded(fd, "SEQUENCE:0"))
        return false;

    enum ical_time_mode mode = ev->time_mode;
    const char *tzid = ev->tzid;
    if (!ev->all_day && mode == ICAL_TIME_DATE)
    {
        const char *dtz = extras_device_tzid();
        if (dtz && *dtz) { mode = ICAL_TIME_TZID; tzid = dtz; }
        else { mode = ICAL_TIME_FLOATING; tzid = ""; }
    }

    if (!write_event_dt(fd, "DTSTART", &ev->date, ev->all_day,
                        ev->start_hour, ev->start_min, mode, tzid))
        return false;

    struct extras_date end_d = event_end_date(ev);

    if (!write_event_dt(fd, "DTEND", &end_d, ev->all_day,
                        ev->end_hour, ev->end_min, mode, tzid))
        return false;

    if (!extras_write_text_prop(fd, "SUMMARY:", title))
        return false;

    const char *rr = repeat_to_rrule(ev->repeat);
    if (rr)
    {
        snprintf(line, sizeof(line), "RRULE:%s", rr);
        if (!extras_write_folded(fd, line))
            return false;
    }
    if (ev->notes[0] && !extras_write_text_prop(fd, "DESCRIPTION:", ev->notes))
        return false;
    if (!write_valarm(fd, ev->alarm, title))
        return false;
    return extras_write_folded(fd, "END:VEVENT");
}

static bool write_new_todo_body(int fd, const struct ical_todo *td,
                                const char *now_stamp)
{
    char line[192];
    const char *title = td->summary[0] ? td->summary : "Untitled";

    if (!extras_write_folded(fd, "BEGIN:VTODO"))
        return false;
    snprintf(line, sizeof(line), "UID:%s", td->uid);
    if (!extras_write_folded(fd, line))
        return false;
    snprintf(line, sizeof(line), "DTSTAMP:%s", now_stamp);
    if (!extras_write_folded(fd, line))
        return false;
    snprintf(line, sizeof(line), "LAST-MODIFIED:%s", now_stamp);
    if (!extras_write_folded(fd, line))
        return false;
    if (!extras_write_folded(fd, "SEQUENCE:0"))
        return false;
    if (!extras_write_text_prop(fd, "SUMMARY:", title))
        return false;
    if (td->has_due)
    {
        snprintf(line, sizeof(line), "DUE;VALUE=DATE:%04d%02d%02d",
                 td->due_date.year, td->due_date.month, td->due_date.day);
        if (!extras_write_folded(fd, line))
            return false;
    }
    if (td->priority > 0)
    {
        snprintf(line, sizeof(line), "PRIORITY:%d", td->priority);
        if (!extras_write_folded(fd, line))
            return false;
    }
    if (td->completed)
    {
        if (!extras_write_folded(fd, "STATUS:COMPLETED"))
            return false;
        if (!extras_write_folded(fd, "PERCENT-COMPLETE:100"))
            return false;
        snprintf(line, sizeof(line), "COMPLETED:%s", now_stamp);
        if (!extras_write_folded(fd, line))
            return false;
    }
    else
    {
        if (!extras_write_folded(fd, "STATUS:NEEDS-ACTION"))
            return false;
    }
    if (td->notes[0] && !extras_write_text_prop(fd, "DESCRIPTION:", td->notes))
        return false;
    return extras_write_folded(fd, "END:VTODO");
}

/* Find byte offset of the last END:VCALENDAR line in `fd`, or false. */
static bool find_end_vcalendar(int fd, uint32_t *out_pos)
{
    struct extras_reader r;
    extras_reader_init(&r, fd, 0);
    char line[256];
    uint32_t rs = 0, re = 0;
    bool found = false;
    while (extras_read_prop(&r, line, sizeof(line), &rs, &re, false))
    {
        char *name, *params, *val;
        split_ical_prop(line, &name, &params, &val);
        if (!strcasecmp(name, "END") &&
            !strcasecmp(extras_trim(val), "VCALENDAR"))
        {
            *out_pos = rs;
            found = true;
        }
    }
    return found;
}

static bool append_to_default_ics(const char *cal_dir,
                                  bool (*write_comp)(int fd, const void *item,
                                                     const char *stamp),
                                  const void *item, const char *now_stamp)
{
    char target[MAX_PATH];
    snprintf(target, sizeof(target), "%s/%s",
             cal_dir ? cal_dir : ICAL_DIR, ICAL_DEFAULT_FILE);

    int src = open(target, O_RDONLY);
    uint32_t end_vcal = 0;
    bool have_wrapper = (src >= 0 && find_end_vcalendar(src, &end_vcal));

    struct extras_atomic a;
    if (!extras_atomic_open(&a, target))
    {
        if (src >= 0) close(src);
        return false;
    }

    bool ok = true;
    if (have_wrapper)
    {
        ok = extras_copy_range(src, a.fd, 0, end_vcal) &&
             write_comp(a.fd, item, now_stamp) &&
             extras_copy_to_eof(src, a.fd, end_vcal);
    }
    else
    {
        ok = extras_write_folded(a.fd, "BEGIN:VCALENDAR") &&
             extras_write_folded(a.fd, "VERSION:2.0") &&
             extras_write_folded(a.fd, "PRODID:-//AstraPod//Extras Calendar//EN") &&
             extras_write_folded(a.fd, "CALSCALE:GREGORIAN") &&
             write_comp(a.fd, item, now_stamp) &&
             extras_write_folded(a.fd, "END:VCALENDAR");
    }

    if (src >= 0)
        close(src);
    if (!ok)
    {
        extras_atomic_abort(&a);
        return false;
    }
    return extras_atomic_commit(&a);
}

static bool write_event_cb(int fd, const void *item, const char *stamp)
{
    return write_new_event_body(fd, (const struct ical_event *)item, stamp);
}

static bool write_todo_cb(int fd, const void *item, const char *stamp)
{
    return write_new_todo_body(fd, (const struct ical_todo *)item, stamp);
}

bool ical_save_event(const char *cal_dir, struct ical_event *ev)
{
    char now_stamp[24];
    extras_now_utc_stamp(now_stamp, sizeof(now_stamp));

    if (!ev->uid[0])
        extras_gen_uid(ev->uid, sizeof(ev->uid));

    if (!ev->is_existing || ev->comp_end <= ev->comp_start)
    {
        snprintf(ev->path, sizeof(ev->path), "%s/%s",
                 cal_dir ? cal_dir : ICAL_DIR, ICAL_DEFAULT_FILE);
        return append_to_default_ics(cal_dir, write_event_cb, ev, now_stamp);
    }

    int src = open(ev->path, O_RDONLY);
    if (src < 0)
        return false;
    /* Second descriptor for copying raw spans without disturbing reader. */
    int copy_fd = open(ev->path, O_RDONLY);
    if (copy_fd < 0)
    {
        close(src);
        return false;
    }

    struct extras_atomic a;
    if (!extras_atomic_open(&a, ev->path))
    {
        close(src);
        close(copy_fd);
        return false;
    }

    bool ok = extras_copy_range(copy_fd, a.fd, 0, ev->comp_start);

    bool time_changed =
        ev->all_day != ev->orig_all_day ||
        extras_date_cmp(&ev->date, &ev->orig_date) != 0 ||
        (!ev->all_day && (ev->start_hour != ev->orig_sh ||
                          ev->start_min != ev->orig_sm ||
                          ev->end_hour != ev->orig_eh ||
                          ev->end_min != ev->orig_em));
    bool repeat_changed = ev->repeat != ev->orig_repeat;
    bool alarm_changed = ev->alarm != ev->orig_alarm;

    enum ical_time_mode mode = ev->time_mode;
    const char *tzid = ev->tzid;
    if (!ev->all_day && mode == ICAL_TIME_DATE)
    {
        const char *dtz = extras_device_tzid();
        if (dtz && *dtz) { mode = ICAL_TIME_TZID; tzid = dtz; }
        else { mode = ICAL_TIME_FLOATING; tzid = ""; }
    }

    int next_seq = ev->sequence < 0 ? 1 :
                   ev->sequence < INT32_MAX ? ev->sequence + 1 : INT32_MAX;
    const char *title = ev->summary[0] ? ev->summary : "Untitled";

    bool wrote_uid = false, wrote_dtstamp = false, wrote_lastmod = false;
    bool wrote_seq = false, wrote_summary = false, wrote_desc = false;
    bool wrote_dtstart = false, wrote_dtend = false, wrote_rrule = false;
    bool saw_valarm0 = false, wrote_valarm0_trig = false;
    int valarm_depth = 0, valarm_idx = -1;

    struct extras_reader r;
    extras_reader_init(&r, src, ev->comp_start);
    char line[ICAL_LINE_BUF];
    char out_line[192];
    uint32_t rs = 0, re = 0;
    bool end_seen = false;

    while (ok && r.offset < ev->comp_end &&
           extras_read_prop(&r, line, sizeof(line), &rs, &re, false))
    {
        char *name, *params, *val;
        split_ical_prop(line, &name, &params, &val);

        if (!strcasecmp(name, "BEGIN"))
        {
            char *v = extras_trim(val);
            if (!strcasecmp(v, "VEVENT"))
            {
                ok = extras_copy_range(copy_fd, a.fd, rs, re);
                continue;
            }
            if (!strcasecmp(v, "VALARM"))
            {
                valarm_depth++;
                valarm_idx++;
                if (valarm_idx == 0)
                {
                    saw_valarm0 = true;
                    if (!alarm_changed || ev->alarm != ICAL_ALARM_NONE)
                        ok = extras_copy_range(copy_fd, a.fd, rs, re);
                }
                else
                    ok = extras_copy_range(copy_fd, a.fd, rs, re);
                continue;
            }
        }
        if (!strcasecmp(name, "END"))
        {
            char *v = extras_trim(val);
            if (valarm_depth > 0 && !strcasecmp(v, "VALARM"))
            {
                if (valarm_idx == 0)
                {
                    if (!alarm_changed)
                        ok = extras_copy_range(copy_fd, a.fd, rs, re);
                    else if (ev->alarm != ICAL_ALARM_NONE)
                    {
                        if (!wrote_valarm0_trig)
                        {
                            snprintf(out_line, sizeof(out_line), "TRIGGER:%s",
                                     alarm_to_trigger(ev->alarm));
                            ok = extras_write_folded(a.fd, out_line);
                        }
                        if (ok)
                            ok = extras_copy_range(copy_fd, a.fd, rs, re);
                    }
                }
                else
                    ok = extras_copy_range(copy_fd, a.fd, rs, re);
                valarm_depth--;
                continue;
            }
            if (!strcasecmp(v, "VEVENT"))
            {
                /* Emit any newly added owned properties before END:VEVENT. */
                if (ok && !wrote_uid)
                {
                    snprintf(out_line, sizeof(out_line), "UID:%s", ev->uid);
                    ok = extras_write_folded(a.fd, out_line);
                }
                if (ok && !wrote_dtstamp)
                {
                    snprintf(out_line, sizeof(out_line), "DTSTAMP:%s",
                             now_stamp);
                    ok = extras_write_folded(a.fd, out_line);
                }
                if (ok && !wrote_lastmod)
                {
                    snprintf(out_line, sizeof(out_line), "LAST-MODIFIED:%s",
                             now_stamp);
                    ok = extras_write_folded(a.fd, out_line);
                }
                if (ok && !wrote_seq)
                {
                    snprintf(out_line, sizeof(out_line), "SEQUENCE:%d",
                             next_seq);
                    ok = extras_write_folded(a.fd, out_line);
                }
                if (ok && !wrote_dtstart)
                {
                    ok = write_event_dt(a.fd, "DTSTART", &ev->date,
                                        ev->all_day, ev->start_hour,
                                        ev->start_min, mode, tzid);
                }
                if (ok && !wrote_dtend && time_changed)
                {
                    struct extras_date end_d = event_end_date(ev);
                    ok = write_event_dt(a.fd, "DTEND", &end_d, ev->all_day,
                                        ev->end_hour, ev->end_min, mode, tzid);
                }
                if (ok && !wrote_summary)
                    ok = extras_write_text_prop(a.fd, "SUMMARY:", title);
                if (ok && !wrote_rrule && ev->repeat != ICAL_REPEAT_NONE)
                {
                    snprintf(out_line, sizeof(out_line), "RRULE:%s",
                             repeat_to_rrule(ev->repeat));
                    ok = extras_write_folded(a.fd, out_line);
                }
                if (ok && !wrote_desc && ev->notes[0])
                    ok = extras_write_text_prop(a.fd, "DESCRIPTION:",
                                                ev->notes);
                if (ok && !saw_valarm0 && ev->alarm != ICAL_ALARM_NONE)
                    ok = write_valarm(a.fd, ev->alarm, title);
                if (ok)
                    ok = extras_copy_range(copy_fd, a.fd, rs, re);
                end_seen = re == ev->comp_end;
                break;
            }
        }

        if (valarm_depth > 0)
        {
            if (valarm_idx == 0)
            {
                if (!alarm_changed)
                    ok = extras_copy_range(copy_fd, a.fd, rs, re);
                else if (ev->alarm == ICAL_ALARM_NONE)
                    continue;
                else if (!strcasecmp(name, "TRIGGER"))
                {
                    snprintf(out_line, sizeof(out_line), "TRIGGER:%s",
                             alarm_to_trigger(ev->alarm));
                    ok = extras_write_folded(a.fd, out_line);
                    wrote_valarm0_trig = true;
                }
                else
                    ok = extras_copy_range(copy_fd, a.fd, rs, re);
            }
            else
                ok = extras_copy_range(copy_fd, a.fd, rs, re);
            continue;
        }

        /* Top-level VEVENT properties. */
        if (!strcasecmp(name, "UID"))
        {
            ok = extras_copy_range(copy_fd, a.fd, rs, re);
            wrote_uid = true;
        }
        else if (!strcasecmp(name, "DTSTAMP"))
        {
            snprintf(out_line, sizeof(out_line), "DTSTAMP:%s", now_stamp);
            ok = extras_write_folded(a.fd, out_line);
            wrote_dtstamp = true;
        }
        else if (!strcasecmp(name, "LAST-MODIFIED"))
        {
            snprintf(out_line, sizeof(out_line), "LAST-MODIFIED:%s",
                     now_stamp);
            ok = extras_write_folded(a.fd, out_line);
            wrote_lastmod = true;
        }
        else if (!strcasecmp(name, "SEQUENCE"))
        {
            snprintf(out_line, sizeof(out_line), "SEQUENCE:%d", next_seq);
            ok = extras_write_folded(a.fd, out_line);
            wrote_seq = true;
        }
        else if (!strcasecmp(name, "SUMMARY"))
        {
            ok = same_text_field(val, ev->summary, sizeof(ev->summary),
                                 true) ?
                 extras_copy_range(copy_fd, a.fd, rs, re) :
                 extras_write_text_prop(a.fd, "SUMMARY:", title);
            wrote_summary = true;
        }
        else if (!strcasecmp(name, "DESCRIPTION"))
        {
            if (same_text_field(val, ev->notes, sizeof(ev->notes),
                                 false))
                ok = extras_copy_range(copy_fd, a.fd, rs, re);
            else if (ev->notes[0])
                ok = extras_write_text_prop(a.fd, "DESCRIPTION:", ev->notes);
            wrote_desc = true;
        }
        else if (!strcasecmp(name, "DTSTART"))
        {
            if (!time_changed)
                ok = extras_copy_range(copy_fd, a.fd, rs, re);
            else
                ok = write_event_dt(a.fd, "DTSTART", &ev->date, ev->all_day,
                                    ev->start_hour, ev->start_min, mode, tzid);
            wrote_dtstart = true;
        }
        else if (!strcasecmp(name, "DTEND"))
        {
            if (!time_changed)
                ok = extras_copy_range(copy_fd, a.fd, rs, re);
            else
            {
                struct extras_date end_d = event_end_date(ev);
                ok = write_event_dt(a.fd, "DTEND", &end_d, ev->all_day,
                                    ev->end_hour, ev->end_min, mode, tzid);
            }
            wrote_dtend = true;
        }
        else if (!strcasecmp(name, "DURATION"))
        {
            if (!time_changed)
            {
                ok = extras_copy_range(copy_fd, a.fd, rs, re);
                wrote_dtend = true;
            }
            /* If time was edited, omit DURATION and emit DTEND instead. */
        }
        else if (!strcasecmp(name, "RRULE"))
        {
            if (!repeat_changed)
                ok = extras_copy_range(copy_fd, a.fd, rs, re);
            else if (ev->repeat != ICAL_REPEAT_NONE)
            {
                snprintf(out_line, sizeof(out_line), "RRULE:%s",
                         repeat_to_rrule(ev->repeat));
                ok = extras_write_folded(a.fd, out_line);
            }
            wrote_rrule = true;
        }
        else
        {
            /* Unknown property: preserve verbatim byte-for-byte. */
            ok = extras_copy_range(copy_fd, a.fd, rs, re);
        }
    }

    ok = ok && end_seen && extras_copy_to_eof(copy_fd, a.fd, ev->comp_end);

    close(src);
    close(copy_fd);
    if (!ok)
    {
        extras_atomic_abort(&a);
        return false;
    }
    return extras_atomic_commit(&a);
}

bool ical_save_todo(const char *cal_dir, struct ical_todo *td)
{
    char now_stamp[24];
    extras_now_utc_stamp(now_stamp, sizeof(now_stamp));

    if (!td->uid[0])
        extras_gen_uid(td->uid, sizeof(td->uid));

    if (!td->is_existing || td->comp_end <= td->comp_start)
    {
        snprintf(td->path, sizeof(td->path), "%s/%s",
                 cal_dir ? cal_dir : ICAL_DIR, ICAL_DEFAULT_FILE);
        return append_to_default_ics(cal_dir, write_todo_cb, td, now_stamp);
    }

    int src = open(td->path, O_RDONLY);
    if (src < 0)
        return false;
    int copy_fd = open(td->path, O_RDONLY);
    if (copy_fd < 0)
    {
        close(src);
        return false;
    }

    struct extras_atomic a;
    if (!extras_atomic_open(&a, td->path))
    {
        close(src);
        close(copy_fd);
        return false;
    }

    bool ok = extras_copy_range(copy_fd, a.fd, 0, td->comp_start);

    bool due_changed = td->has_due != td->orig_has_due ||
                       (td->has_due &&
                        extras_date_cmp(&td->due_date, &td->orig_due_date) != 0);
    bool prio_changed = td->priority != td->orig_priority;
    bool comp_changed = td->completed != td->orig_completed;

    int next_seq = td->sequence < 0 ? 1 :
                   td->sequence < INT32_MAX ? td->sequence + 1 : INT32_MAX;
    const char *title = td->summary[0] ? td->summary : "Untitled";

    bool wrote_uid = false, wrote_dtstamp = false, wrote_lastmod = false;
    bool wrote_seq = false, wrote_summary = false, wrote_desc = false;
    bool wrote_due = false, wrote_prio = false, wrote_status = false;
    bool wrote_completed = false, wrote_pct = false;
    int valarm_depth = 0;

    struct extras_reader r;
    extras_reader_init(&r, src, td->comp_start);
    char line[ICAL_LINE_BUF];
    char out_line[192];
    uint32_t rs = 0, re = 0;
    bool end_seen = false;

    while (ok && r.offset < td->comp_end &&
           extras_read_prop(&r, line, sizeof(line), &rs, &re, false))
    {
        char *name, *params, *val;
        split_ical_prop(line, &name, &params, &val);

        if (!strcasecmp(name, "BEGIN"))
        {
            char *v = extras_trim(val);
            if (!strcasecmp(v, "VALARM"))
                valarm_depth++;
            ok = extras_copy_range(copy_fd, a.fd, rs, re);
            continue;
        }
        if (!strcasecmp(name, "END"))
        {
            char *v = extras_trim(val);
            if (valarm_depth > 0 && !strcasecmp(v, "VALARM"))
            {
                valarm_depth--;
                ok = extras_copy_range(copy_fd, a.fd, rs, re);
                continue;
            }
            if (!strcasecmp(v, "VTODO"))
            {
                if (ok && !wrote_uid)
                {
                    snprintf(out_line, sizeof(out_line), "UID:%s", td->uid);
                    ok = extras_write_folded(a.fd, out_line);
                }
                if (ok && !wrote_dtstamp)
                {
                    snprintf(out_line, sizeof(out_line), "DTSTAMP:%s",
                             now_stamp);
                    ok = extras_write_folded(a.fd, out_line);
                }
                if (ok && !wrote_lastmod)
                {
                    snprintf(out_line, sizeof(out_line), "LAST-MODIFIED:%s",
                             now_stamp);
                    ok = extras_write_folded(a.fd, out_line);
                }
                if (ok && !wrote_seq)
                {
                    snprintf(out_line, sizeof(out_line), "SEQUENCE:%d",
                             next_seq);
                    ok = extras_write_folded(a.fd, out_line);
                }
                if (ok && !wrote_summary)
                    ok = extras_write_text_prop(a.fd, "SUMMARY:", title);
                if (ok && !wrote_due && td->has_due)
                {
                    snprintf(out_line, sizeof(out_line),
                             "DUE;VALUE=DATE:%04d%02d%02d",
                             td->due_date.year, td->due_date.month,
                             td->due_date.day);
                    ok = extras_write_folded(a.fd, out_line);
                }
                if (ok && !wrote_prio && td->priority > 0)
                {
                    snprintf(out_line, sizeof(out_line), "PRIORITY:%d",
                             td->priority);
                    ok = extras_write_folded(a.fd, out_line);
                }
                if (ok && !wrote_status)
                    ok = extras_write_folded(a.fd,
                            td->completed ? "STATUS:COMPLETED" :
                                            "STATUS:NEEDS-ACTION");
                if (ok && td->completed && !wrote_pct)
                    ok = extras_write_folded(a.fd, "PERCENT-COMPLETE:100");
                if (ok && td->completed && !wrote_completed)
                {
                    snprintf(out_line, sizeof(out_line), "COMPLETED:%s",
                             now_stamp);
                    ok = extras_write_folded(a.fd, out_line);
                }
                if (ok && !wrote_desc && td->notes[0])
                    ok = extras_write_text_prop(a.fd, "DESCRIPTION:",
                                                td->notes);
                if (ok)
                    ok = extras_copy_range(copy_fd, a.fd, rs, re);
                end_seen = re == td->comp_end;
                break;
            }
        }

        if (valarm_depth > 0)
        {
            ok = extras_copy_range(copy_fd, a.fd, rs, re);
            continue;
        }

        if (!strcasecmp(name, "UID"))
        {
            ok = extras_copy_range(copy_fd, a.fd, rs, re);
            wrote_uid = true;
        }
        else if (!strcasecmp(name, "DTSTAMP"))
        {
            snprintf(out_line, sizeof(out_line), "DTSTAMP:%s", now_stamp);
            ok = extras_write_folded(a.fd, out_line);
            wrote_dtstamp = true;
        }
        else if (!strcasecmp(name, "LAST-MODIFIED"))
        {
            snprintf(out_line, sizeof(out_line), "LAST-MODIFIED:%s",
                     now_stamp);
            ok = extras_write_folded(a.fd, out_line);
            wrote_lastmod = true;
        }
        else if (!strcasecmp(name, "SEQUENCE"))
        {
            snprintf(out_line, sizeof(out_line), "SEQUENCE:%d", next_seq);
            ok = extras_write_folded(a.fd, out_line);
            wrote_seq = true;
        }
        else if (!strcasecmp(name, "SUMMARY"))
        {
            ok = same_text_field(val, td->summary, sizeof(td->summary),
                                 true) ?
                 extras_copy_range(copy_fd, a.fd, rs, re) :
                 extras_write_text_prop(a.fd, "SUMMARY:", title);
            wrote_summary = true;
        }
        else if (!strcasecmp(name, "DESCRIPTION"))
        {
            if (same_text_field(val, td->notes, sizeof(td->notes),
                                 false))
                ok = extras_copy_range(copy_fd, a.fd, rs, re);
            else if (td->notes[0])
                ok = extras_write_text_prop(a.fd, "DESCRIPTION:", td->notes);
            wrote_desc = true;
        }
        else if (!strcasecmp(name, "DUE"))
        {
            if (!due_changed)
                ok = extras_copy_range(copy_fd, a.fd, rs, re);
            else if (td->has_due)
            {
                snprintf(out_line, sizeof(out_line),
                         "DUE;VALUE=DATE:%04d%02d%02d",
                         td->due_date.year, td->due_date.month,
                         td->due_date.day);
                ok = extras_write_folded(a.fd, out_line);
            }
            wrote_due = true;
        }
        else if (!strcasecmp(name, "PRIORITY"))
        {
            if (!prio_changed)
                ok = extras_copy_range(copy_fd, a.fd, rs, re);
            else if (td->priority > 0)
            {
                snprintf(out_line, sizeof(out_line), "PRIORITY:%d",
                         td->priority);
                ok = extras_write_folded(a.fd, out_line);
            }
            wrote_prio = true;
        }
        else if (!strcasecmp(name, "STATUS"))
        {
            if (!comp_changed)
                ok = extras_copy_range(copy_fd, a.fd, rs, re);
            else
                ok = extras_write_folded(a.fd,
                        td->completed ? "STATUS:COMPLETED" :
                                        "STATUS:NEEDS-ACTION");
            wrote_status = true;
        }
        else if (!strcasecmp(name, "COMPLETED"))
        {
            if (td->completed)
            {
                if (!comp_changed)
                    ok = extras_copy_range(copy_fd, a.fd, rs, re);
                else
                {
                    snprintf(out_line, sizeof(out_line), "COMPLETED:%s",
                             now_stamp);
                    ok = extras_write_folded(a.fd, out_line);
                }
                wrote_completed = true;
            }
            /* If marked incomplete, omit COMPLETED. */
        }
        else if (!strcasecmp(name, "PERCENT-COMPLETE"))
        {
            if (td->completed)
            {
                if (!comp_changed)
                    ok = extras_copy_range(copy_fd, a.fd, rs, re);
                else
                    ok = extras_write_folded(a.fd, "PERCENT-COMPLETE:100");
                wrote_pct = true;
            }
            /* If marked incomplete, omit PERCENT-COMPLETE. */
        }
        else
        {
            /* Unknown property: preserve verbatim byte-for-byte. */
            ok = extras_copy_range(copy_fd, a.fd, rs, re);
        }
    }

    ok = ok && end_seen && extras_copy_to_eof(copy_fd, a.fd, td->comp_end);

    close(src);
    close(copy_fd);
    if (!ok)
    {
        extras_atomic_abort(&a);
        return false;
    }
    return extras_atomic_commit(&a);
}

bool ical_delete_component(const char *path, uint32_t comp_start,
                           uint32_t comp_end)
{
    if (comp_end <= comp_start)
        return false;
    int src = open(path, O_RDONLY);
    if (src < 0)
        return false;

    struct extras_atomic a;
    if (!extras_atomic_open(&a, path))
    {
        close(src);
        return false;
    }

    bool ok = extras_copy_range(src, a.fd, 0, comp_start) &&
              extras_copy_to_eof(src, a.fd, comp_end);
    close(src);
    if (!ok)
    {
        extras_atomic_abort(&a);
        return false;
    }
    return extras_atomic_commit(&a);
}
