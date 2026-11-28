/* AstraPod time zone preference and clock integration. GPL version 2 or later. */
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "string-extra.h"
#include "system.h"
#include "timefuncs.h"
#include "settings/settings.h"
#include "system/time_zone.h"
#include "system/time_zone_catalog.h"

_Static_assert(sizeof(global_settings.time_zone) == TIME_ZONE_SETTING_SIZE,
               "time zone cfg storage must match wire field limits");

static struct time_zone active;
static bool ready;
static unsigned generation;

static struct time_zone snapshot(void)
{
    int irq = disable_irq_save();
    struct time_zone zone = active;
    restore_irq(irq);
    return zone;
}

static int offset_at(time_t utc, int *dst)
{
    struct time_zone zone = snapshot();
    return time_zone_offset(&zone, utc, dst);
}

static time_t from_local(const struct tm *local)
{
    struct time_zone zone = snapshot();
    return time_zone_to_utc(&zone, local);
}

unsigned time_zone_generation(void)
{
    return generation;
}

time_t time_zone_current_to_utc(const struct tm *local)
{
    return from_local(local);
}

static bool valid_name(const char *name)
{
    size_t len = strlen(name);
    if (!len || len >= TIME_ZONE_NAME_SIZE)
        return false;
    for (; *name; name++)
        if (!((*name >= 'a' && *name <= 'z') ||
              (*name >= 'A' && *name <= 'Z') ||
              (*name >= '0' && *name <= '9') ||
              *name == '_' || *name == '-' || *name == '+' || *name == '/'))
            return false;
    return true;
}

static bool decode(const char *value, struct time_zone *zone)
{
    const char *sep = strchr(value, '|');
    if (!sep)
        return time_zone_lookup(value, zone);
    if (sep == value || sep - value >= TIME_ZONE_NAME_SIZE)
        return false;
    char name[TIME_ZONE_NAME_SIZE];
    memcpy(name, value, sep - value);
    name[sep - value] = 0;

    *zone = (struct time_zone){ .catalog = -1 };
    return valid_name(name) && strlen(sep + 1) < TIME_ZONE_RULE_SIZE &&
           zone_rule_parse(sep + 1, &zone->rule);
}

void time_zone_cfg_load(void *var, char *value)
{
    struct time_zone zone;
    if (strlen(value) >= TIME_ZONE_SETTING_SIZE || !decode(value, &zone))
        return;
    if (var != value)
        strlcpy(var, value, TIME_ZONE_SETTING_SIZE);
    int irq = disable_irq_save();
    active = zone;
    generation++;
    restore_irq(irq);
}

char *time_zone_cfg_write(void *var, char *buf, int size)
{
    strlcpy(buf, var, size);
    return buf;
}

bool time_zone_cfg_changed(void *var, void *def)
{
    return strcmp(var, def) != 0;
}

void time_zone_cfg_default(void *var, void *def)
{
    time_zone_cfg_load(var, def);
}

void time_zone_init(void)
{
    if (!ready)
    {
        /* Legacy civil RTC fields enter UTC at zero offset. An explicit clock
         * set or a computer timestamp supplies the original instant. */
        time_zone_cfg_load(global_settings.time_zone, global_settings.time_zone);
        clock_use_utc(offset_at, from_local);
        ready = true;
    }
}

bool time_zone_follow(const char *name, const char *rule, time_t utc,
                      int offset)
{
    if (!valid_name(name))
        return false;
    struct time_zone zone, host = { .catalog = -1 };
    bool known = time_zone_lookup(name, &zone);
    bool has_rule = zone_rule_parse(rule, &host.rule);
    int dst;

    char value[TIME_ZONE_SETTING_SIZE];
    /* Keep historical transitions when the catalog agrees with the host. */
    bool rule_matches = has_rule && zone_rule_offset(&host.rule, utc, &dst) == offset;
    if (known && time_zone_offset(&zone, utc, &dst) == offset &&
        (!rule_matches || !strcmp(rule, time_zone_footer(&zone))))
        strlcpy(value, name, sizeof(value));
    else if (rule_matches)
        snprintf(value, sizeof(value), "%s|%s", name, rule);
    else
    {
        unsigned seconds = offset < 0 ? -offset : offset;
        snprintf(value, sizeof(value), "%s|FIX%s%u:%02u:%02u", name,
                 offset > 0 ? "-" : "", seconds / 3600, seconds / 60 % 60,
                 seconds % 60);
    }
    if (strcmp(value, global_settings.time_zone) != 0)
    {
        time_zone_cfg_load(global_settings.time_zone, value);
        settings_save();
    }
    return true;
}
