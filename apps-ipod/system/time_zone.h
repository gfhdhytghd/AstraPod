/* AstraPod time zone catalog and conversion. GPL version 2 or later. */
#ifndef ASTRA_TIME_ZONE_H
#define ASTRA_TIME_ZONE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define TIME_ZONE_DEFAULT "Etc/UTC"
#define TIME_ZONE_NAME_SIZE 64
#define TIME_ZONE_RULE_SIZE 96
#define TIME_ZONE_SETTING_SIZE (TIME_ZONE_NAME_SIZE + TIME_ZONE_RULE_SIZE)

struct zone_day_rule {
    int day, month, week, weekday, seconds;
    char type;
};

struct zone_rule {
    int standard, summer;
    bool seasonal;
    struct zone_day_rule start, end;
};

struct time_zone {
    int catalog;
    struct zone_rule rule;
};

bool zone_rule_parse(const char *text, struct zone_rule *rule);
int zone_rule_offset(const struct zone_rule *rule, time_t utc, int *dst);
bool time_zone_lookup(const char *name, struct time_zone *zone);
int time_zone_offset(const struct time_zone *zone, time_t utc, int *dst);
time_t time_zone_to_utc(const struct time_zone *zone, const struct tm *local);
unsigned time_zone_generation(void);
time_t time_zone_current_to_utc(const struct tm *local);
void time_zone_init(void);
bool time_zone_follow(const char *name, const char *rule, time_t utc,
                      int offset);
void time_zone_cfg_load(void *var, char *value);
char *time_zone_cfg_write(void *var, char *buf, int size);
bool time_zone_cfg_changed(void *var, void *def);
void time_zone_cfg_default(void *var, void *def);
int time_zone_picker(void);
#endif
