/* Firmware-owned plugin services. GNU GPL version 2 or later. */
#ifndef ASTRAPOD_ROCK_RESIDENT_H
#define ASTRAPOD_ROCK_RESIDENT_H

#include <stdbool.h>

#define ROCK_ANNOUNCE_FORMAT_MAX 63

struct rock_announce_status {
    bool track_change;
    int interval;
    int grouping;
    char format[ROCK_ANNOUNCE_FORMAT_MAX + 1];
};

bool rock_battery_bench_running(void);
bool rock_battery_bench_start(void);
bool rock_battery_bench_stop(void);
void rock_announce_status_get(struct rock_announce_status *options);
bool rock_announce_status_set(const struct rock_announce_status *options);
bool rock_announce_status_speak(void);

#ifdef ASTRAPOD_ROCK_COMPAT
void rock_resident_shutdown(bool disk_safe);
bool rock_announce_status_on_track_change(void);
void rock_announce_status_track_change(void);
#else
static inline void rock_resident_shutdown(bool disk_safe) { (void)disk_safe; }
#endif

#endif
