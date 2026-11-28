/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * RFC 5545 iCalendar (VEVENT and VTODO) scanner, recurrence matcher,
 * loader, and lossless atomic editor for /Calendars .ics files.
 ****************************************************************************/
#ifndef PODBOX_SCREENS_EXTRAS_ICAL_H
#define PODBOX_SCREENS_EXTRAS_ICAL_H

#include <stdbool.h>
#include <stdint.h>
#include "extras_store.h"

#define ICAL_DIR            "/Calendars"
#define ICAL_DEFAULT_FILE   "AstraPod.ics"
#define ICAL_MAX_FILES      32
#define ICAL_MAX_EVENTS     256
#define ICAL_MAX_TODOS      256
#define ICAL_MAX_XDATES     512
#define ICAL_POOL_BYTES     16384

enum ical_repeat {
    ICAL_REPEAT_NONE = 0,
    ICAL_REPEAT_DAILY,
    ICAL_REPEAT_WEEKLY,
    ICAL_REPEAT_MONTHLY,
    ICAL_REPEAT_YEARLY,
    ICAL_REPEAT_COUNT
};

enum ical_alarm {
    ICAL_ALARM_NONE = 0,
    ICAL_ALARM_AT_TIME,
    ICAL_ALARM_5M,
    ICAL_ALARM_15M,
    ICAL_ALARM_30M,
    ICAL_ALARM_1H,
    ICAL_ALARM_1D,
    ICAL_ALARM_COUNT
};

enum ical_time_mode {
    ICAL_TIME_FLOATING = 0,
    ICAL_TIME_UTC,
    ICAL_TIME_TZID,
    ICAL_TIME_DATE
};

struct ical_event_idx {
    uint8_t file_idx;
    uint32_t comp_start;
    uint32_t comp_end;
    struct extras_date start_date;
    int span_days;                   /* >= 1 */
    uint8_t all_day;
    uint8_t start_hour, start_min;
    uint8_t end_hour, end_min;
    uint8_t repeat;                  /* enum ical_repeat */
    uint32_t interval;               /* RRULE INTERVAL */
    uint8_t byday_mask;              /* bit 0=Sun .. 6=Sat; 0 if unset */
    uint16_t bymonth_mask;
    uint32_t monthdays, reverse_monthdays;
    int16_t ordinal_days[14];         /* Encoded as (ordinal * 8) + weekday */
    uint8_t ordinal_count;
    int16_t positions[16];           /* BYSETPOS */
    uint8_t position_count;
    uint8_t week_start;
    uint32_t rrule_count;            /* 0 = unlimited */
    uint8_t has_until;
    struct extras_date until_date;
    uint16_t xdate_start, xdate_end;  /* Half-open range in index */
    uint8_t alarm;                   /* enum ical_alarm */
    uint16_t summary_off;            /* offset into pool */
};

struct ical_todo_idx {
    uint8_t file_idx;
    uint32_t comp_start;
    uint32_t comp_end;
    uint8_t has_due;
    struct extras_date due_date;
    uint8_t priority;                /* RFC 5545 0..9 */
    uint8_t completed;
    uint16_t summary_off;            /* offset into pool */
};

struct ical_xdate {
    struct extras_date date;
    bool excluded;
    int span_days;                   /* 0 = use the event duration */
};

struct ical_index {
    char dir[MAX_PATH];
    int file_count;
    uint16_t file_off[ICAL_MAX_FILES];
    int event_count;
    struct ical_event_idx events[ICAL_MAX_EVENTS];
    int todo_count;
    struct ical_todo_idx todos[ICAL_MAX_TODOS];
    uint16_t xdate_count;
    struct ical_xdate xdates[ICAL_MAX_XDATES];
    uint16_t pool_used;
    char pool[ICAL_POOL_BYTES];
};

struct ical_event {
    char path[MAX_PATH];
    uint32_t comp_start;
    uint32_t comp_end;               /* 0 = new event */
    char uid[128];
    int sequence;                    /* -1 if absent in source */
    char summary[160];
    char notes[512];
    struct extras_date date;
    bool all_day;
    int start_hour, start_min;
    int end_hour, end_min;
    int span_days;                   /* All-day duration */
    int end_day_offset;              /* Timed events spanning midnight */
    enum ical_time_mode time_mode;
    char tzid[64];
    enum ical_repeat repeat;
    enum ical_alarm alarm;

    /* Snapshot at load time for lossless preservation of unchanged fields. */
    bool is_existing;
    bool orig_all_day;
    struct extras_date orig_date;
    int orig_sh, orig_sm, orig_eh, orig_em;
    enum ical_repeat orig_repeat;
    enum ical_alarm orig_alarm;
};

struct ical_todo {
    char path[MAX_PATH];
    uint32_t comp_start;
    uint32_t comp_end;               /* 0 = new todo */
    char uid[128];
    int sequence;                    /* -1 if absent in source */
    char summary[160];
    char notes[512];
    bool completed;
    bool has_due;
    struct extras_date due_date;
    int priority;                    /* 0..9 */

    bool is_existing;
    bool orig_completed;
    bool orig_has_due;
    struct extras_date orig_due_date;
    int orig_priority;
};

void ical_scan(struct ical_index *idx, const char *dir);
const char *ical_pool_str(const struct ical_index *idx, uint16_t off);
void ical_file_path(const struct ical_index *idx, int file_idx,
                    char *buf, size_t size);

bool ical_event_occurs_on(const struct ical_index *idx,
                          const struct ical_event_idx *e,
                          const struct extras_date *d);
bool ical_has_event_on(const struct ical_index *idx,
                       const struct extras_date *d);
/* Collect indices of events occurring on `d`, sorted all-day first then
 * by start time. Returns number stored (up to `max_out`). */
int ical_events_on_day(const struct ical_index *idx,
                       const struct extras_date *d,
                       int *out_indices, int max_out);

/* Priority UI buckets: 0=None, 1=Low (6..9), 2=Medium (5), 3=High (1..4). */
int ical_priority_to_bucket(int rfc_priority);
int ical_bucket_to_priority(int bucket);

void ical_event_init_new(struct ical_event *ev, const struct extras_date *d);
bool ical_load_event(const char *path, uint32_t comp_start, uint32_t comp_end,
                     struct ical_event *ev);
bool ical_save_event(const char *cal_dir, struct ical_event *ev);

void ical_todo_init_new(struct ical_todo *td);
bool ical_load_todo(const char *path, uint32_t comp_start, uint32_t comp_end,
                    struct ical_todo *td);
bool ical_save_todo(const char *cal_dir, struct ical_todo *td);

bool ical_delete_component(const char *path, uint32_t comp_start,
                           uint32_t comp_end);

#endif
