/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The Alarms' stored table and the check that rings them. Extras > Alarms
 * edits alarm_table and saves it; get_action() calls the check, which shows
 * the alert over whatever screen is up when an alarm comes due.
 ****************************************************************************/
#ifndef PODBOX_SCREENS_EXTRAS_ALARM_RING_H
#define PODBOX_SCREENS_EXTRAS_ALARM_RING_H

#include <stdbool.h>
#include <stdint.h>

#define MAX_ALARMS 8
#define ALARM_LABELS 23
#define ALARM_PLAYLIST_LEN 64

enum alarm_repeat {
    ALARM_ONCE, ALARM_EVERY_DAY, ALARM_WEEKDAYS, ALARM_WEEKENDS,
    ALARM_EVERY_WEEK, ALARM_EVERY_MONTH, ALARM_EVERY_YEAR,
};
enum alarm_sound { ALARM_SOUND_NONE, ALARM_SOUND_BEEP, ALARM_SOUND_PLAYLIST };

/* One alarm as the file holds it; `month` counts from 1. */
struct alarm {
    uint8_t on, repeat, sound, label;
    uint8_t hour, minute, month, day;
    uint16_t year;
    char playlist[ALARM_PLAYLIST_LEN];
};

struct alarm_table {
    uint32_t magic;
    uint32_t count;
    struct alarm list[MAX_ALARMS];
};

extern struct alarm_table alarm_table;
/* The lang ids of the labels, in the order the Label list shows them. */
extern const int alarm_label_names[ALARM_LABELS];

/* Read the table from its file, empty when there is none. */
void alarm_table_load(void);
/* Write the table; alarms ring by the times now in it. */
void alarm_table_save(void);

/* Ring any alarm that has come due, returning true once its alert is
 * dismissed (the screen under it needs repainting). Otherwise shorten
 * `*timeout` (ticks, or TIMEOUT_BLOCK) so the caller looks again when the
 * next minute begins. */
bool extras_alarm_ring_poll(int *timeout);

#endif
