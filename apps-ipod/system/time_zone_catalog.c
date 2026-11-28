/* AstraPod IANA time zone catalog. GPL version 2 or later.
 * The generated table contains transitions and UTC offsets from IANA tzdata.
 */
#include <stddef.h>
#include <string.h>
#include "timefuncs.h"
#include "system/time_zone.h"
#include "system/time_zone_catalog.h"

struct zone_change {
    int32_t utc, offset;
    int8_t dst;
};

struct zone_entry {
    const char *name;
    const struct zone_change *changes;
    const char *footer;
    unsigned short count;
    bool picker;
};

#define ZONE(name, i, footer, picker) \
    {name, zone_##i, footer, sizeof(zone_##i)/sizeof(*zone_##i), picker}
#include "zone_data.inc"
#undef ZONE

int time_zone_count(void)
{
    return sizeof(zone_entries) / sizeof(*zone_entries);
}

const char *time_zone_name(int i, bool only_picker)
{
    if (i < 0 || i >= time_zone_count() ||
        (only_picker && !zone_entries[i].picker))
        return NULL;
    return zone_entries[i].name;
}

bool time_zone_lookup(const char *name, struct time_zone *zone)
{
    int lo = 0, hi = time_zone_count();
    while (lo < hi)
    {
        int mid = (lo + hi) / 2;
        int cmp = strcmp(name, zone_entries[mid].name);
        if (cmp == 0)
        {
            *zone = (struct time_zone){ .catalog = mid };
            return true;
        }
        if (cmp > 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return false;
}

const char *time_zone_footer(const struct time_zone *zone)
{
    return zone_entries[zone->catalog].footer;
}

int time_zone_offset(const struct time_zone *zone, time_t utc, int *dst)
{
    if (zone->catalog < 0)
        return zone_rule_offset(&zone->rule, utc, dst);

    const struct zone_entry *entry = &zone_entries[zone->catalog];
    unsigned lo = 0, hi = entry->count;
    while (lo + 1 < hi)
    {
        unsigned mid = (lo + hi) / 2;
        if (entry->changes[mid].utc <= utc)
            lo = mid;
        else
            hi = mid;
    }
    *dst = entry->changes[lo].dst;
    return entry->changes[lo].offset;
}

time_t time_zone_to_utc(const struct time_zone *zone, const struct tm *local)
{
    int64_t civil = clock_civil_seconds(local);
    if (civil < 0 || civil > INT32_MAX - 2 * 86400)
        return -1;

    /* The earlier occurrence wins a fold. A gap uses the previous offset. */
    int dst;
    int before = time_zone_offset(zone, civil - 2 * 86400, &dst);
    int after = time_zone_offset(zone, civil + 2 * 86400, &dst);
    time_t a = civil - before, b = civil - after;
    bool valid_a = before == time_zone_offset(zone, a, &dst);
    bool valid_b = after == time_zone_offset(zone, b, &dst);
    return valid_b && (!valid_a || b < a) ? b : a;
}
