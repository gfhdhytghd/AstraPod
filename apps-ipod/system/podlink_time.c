/* UTC RTC synchronization; the resident transport only queues records.
 * GPL version 2 or later. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "config.h"
#include "kernel.h"
#include "podlink.h"
#include "timefuncs.h"
#include "remote_log.h"
#include "settings/settings.h"
#include "podlink_time.h"
#include "system/time_zone.h"

#define CLOCK_DRIFT_SECONDS 2
#define CLOCK_RECORD_MAX_AGE (5 * HZ)

static bool clock_date_valid(const struct tm *t)
{
    static const int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    /* Signed 32-bit log/scrobble timestamps must remain representable. */
    if (t->tm_year < 100 || t->tm_year > 137 || t->tm_mon < 0 ||
        t->tm_mon > 11 || t->tm_hour < 0 || t->tm_hour > 23 ||
        t->tm_min < 0 || t->tm_min > 59 || t->tm_sec < 0 || t->tm_sec > 59)
        return false;
    int max = days[t->tm_mon] + (t->tm_mon == 1 && t->tm_year % 4 == 0);
    return t->tm_mday >= 1 && t->tm_mday <= max;
}

bool podlink_time_decode(const char *record, struct tm *local, int *offset)
{
    size_t n = strlen(record);
    if (n < 27 || n > 31 || memcmp(record, "TS\t1\t", 5) ||
        record[9] != '-' || record[12] != '-' || record[15] != 'T' ||
        record[18] != ':' || record[21] != ':' || record[24] != '\t' ||
        (record[25] != '+' && record[25] != '-')) return false;
    for (size_t i = 5; i < n; i++)
    {
        if (i == 9 || i == 12 || i == 15 || i == 18 || i == 21 ||
            i == 24 || i == 25) continue;
        if (record[i] < '0' || record[i] > '9') return false;
    }
    int end = 0;
    memset(local, 0, sizeof(*local));
    if (sscanf(record + 5, "%d-%d-%dT%d:%d:%d\t%d%n", &local->tm_year,
               &local->tm_mon, &local->tm_mday, &local->tm_hour,
               &local->tm_min, &local->tm_sec, offset, &end) != 7 ||
        end != (int)n - 5 || *offset < -50400 || *offset > 50400)
        return false;
    local->tm_year -= 1900;
    local->tm_mon--;
    if (!clock_date_valid(local)) return false;
    time_t civil = clock_civil_seconds(local);
    gmtime_r(&civil, local);
    local->tm_isdst = -1;
    return true;
}

bool podlink_time_parse(char *record, struct podlink_clock *clock)
{
    /* Split only after the four legacy fields. The accepted legacy prefix
     * remains exactly the record understood by older players. */
    char *ext = strlen(record) > 25 ? strchr(record + 25, '\t') : NULL;
    if (ext)
        *ext++ = 0;
    struct tm local;
    if (!podlink_time_decode(record, &local, &clock->offset))
        return false;
    int64_t utc = clock_civil_seconds(&local) - clock->offset;
    if (utc < 946684800L || utc >= 2145916800L)
        return false;
    clock->utc = utc;
    clock->zone = clock->rule = NULL;
    if (!ext)
        return true;

    char *zone = strchr(ext, '\t');
    if (!zone || zone - ext != 20 || ext[19] != 'Z')
        return false;
    *zone++ = 0;
    char *rule = strchr(zone, '\t');
    if (!rule || rule == zone || rule - zone >= TIME_ZONE_NAME_SIZE)
        return false;
    *rule++ = 0;
    for (const char *p = zone; *p; p++)
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
              (*p >= '0' && *p <= '9') || strchr("_+-/", *p)))
            return false;
    struct zone_rule parsed;
    if (strlen(rule) >= TIME_ZONE_RULE_SIZE ||
        (strcmp(rule, "-") != 0 && !zone_rule_parse(rule, &parsed)))
        return false;

    struct tm stamp;
    gmtime_r(&clock->utc, &stamp);
    char expected[21];
    snprintf(expected, sizeof(expected), "%04u-%02u-%02uT%02u:%02u:%02uZ",
             (unsigned)(stamp.tm_year + 1900), (unsigned)(stamp.tm_mon + 1),
             (unsigned)stamp.tm_mday, (unsigned)stamp.tm_hour,
             (unsigned)stamp.tm_min, (unsigned)stamp.tm_sec);
    if (strcmp(ext, expected))
        return false;
    clock->zone = zone;
    clock->rule = rule;
    return true;
}

void podlink_time_sync(void)
{
    char record[PODLINK_TIME_RECORD], note[REMOTE_LOG_DATA];
    unsigned long received;
    if (!podlink_take_time(record, &received))
        return;
    unsigned long age = (unsigned long)current_tick - received;
    struct podlink_clock clock = {0};
    long drift = 0;
    const char *decision;
    if (!global_settings.sync_time_from_computer) decision = "ignored-off";
    else if (!podlink_time_parse(record, &clock)) decision = "ignored-invalid";
    else if (age > CLOCK_RECORD_MAX_AGE) decision = "ignored-stale";
    else
    {
        time_t target = clock.utc + age / HZ;
        time_t before = get_time_utc_fresh();
        bool old_valid = before >= 0;
        drift = old_valid ? target - before : 0;
        if (target >= 2145916800L)
            decision = "ignored-range";
        else
        {
            bool stable = old_valid && drift >= -CLOCK_DRIFT_SECONDS &&
                          drift <= CLOCK_DRIFT_SECONDS;
            if (!stable && set_time_utc(target) != 0)
                decision = "ignored-rtc-error";
            else
            {
                if (global_settings.follow_computer_zone)
                {
                    struct tm local;
                    clock_localtime(clock.utc, &local);
                    int offset = clock_civil_seconds(&local) - clock.utc;
                    if (clock.zone || offset != clock.offset)
                        time_zone_follow(clock.zone ? clock.zone : "Legacy",
                                         clock.rule ? clock.rule : "-",
                                         clock.utc, clock.offset);
                }
                if (!stable && global_status.debug_log_since)
                {
                    int64_t since = old_valid ?
                        (int64_t)global_status.debug_log_since + drift : target;
                    global_status.debug_log_since = since < 0 ? 0 :
                        since > target ? target : (int)since;
                }
                decision = stable ? "ignored-drift" : "applied";
            }
        }
    }
    snprintf(note, sizeof(note), "%s drift=%ld offset=%d age_ticks=%lu record=%.63s",
             decision, drift, clock.offset, age, record);
    remote_log_event(REMOTE_LOG_TIME, current_tick, 0, 0, note, strlen(note) + 1);
}
