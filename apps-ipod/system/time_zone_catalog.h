/* AstraPod time zone picker catalog. GPL version 2 or later. */
#ifndef ASTRA_TIME_ZONE_CATALOG_H
#define ASTRA_TIME_ZONE_CATALOG_H
#include <stdbool.h>
struct time_zone;
int time_zone_count(void);
const char *time_zone_name(int i, bool only_picker);
const char *time_zone_footer(const struct time_zone *zone);
#endif
