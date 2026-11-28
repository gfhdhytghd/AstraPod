/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Shared storage primitives for Extras (Calendars, To Do's, Contacts):
 * atomic temp-file writes, RFC 5545 / vCard line unfolding with exact raw
 * byte offsets, 75-octet UTF-8-safe line folding, UID generation, date
 * arithmetic, and the device time zone hook (t3/time-zone placeholder).
 ****************************************************************************/
#ifndef PODBOX_SCREENS_EXTRAS_STORE_H
#define PODBOX_SCREENS_EXTRAS_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef EXTRAS_HOST_TEST
#include <limits.h>
#ifndef MAX_PATH
#define MAX_PATH 512
#endif
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#ifndef ARRAYLEN
#define ARRAYLEN(a) (sizeof(a) / sizeof((a)[0]))
#endif
#else
#include "file.h"
#endif

struct extras_date {
    int year, month, day;            /* month 1-12, day 1-31 */
};

/* ---- Date arithmetic ---- */
bool extras_leap_year(int year);
int extras_month_days(int year, int month);
int extras_week_day(const struct extras_date *d); /* 0 = Sunday .. 6 = Saturday */
void extras_step_days(struct extras_date *d, int days);
void extras_step_month(struct extras_date *d, int months);
int extras_date_cmp(const struct extras_date *a, const struct extras_date *b);
int extras_date_days_between(const struct extras_date *a,
                             const struct extras_date *b);
struct extras_date extras_today(void);

/* ---- Time zone hook & timestamps ---- */
/* Returns the device's configured IANA TZID (e.g. "America/New_York"), or
 * NULL when no Time Zone setting is configured yet (placeholder hook for the
 * parallel t3/time-zone branch). */
const char *extras_device_tzid(void);
void extras_now_utc_stamp(char *buf, size_t size);
void extras_gen_uid(char *buf, size_t size);

#ifdef EXTRAS_HOST_TEST
void extras_test_set_now(int year, int month, int day,
                         int hour, int min, int sec);
void extras_test_set_tzid(const char *tzid);
void extras_test_reset_uid_seq(void);
#endif

/* ---- Atomic writes & file helpers ---- */
struct extras_atomic {
    int fd;
    char target[MAX_PATH];
    char tmp[MAX_PATH];
};

bool extras_ensure_dir(const char *path);
bool extras_atomic_open(struct extras_atomic *a, const char *target);
bool extras_atomic_commit(struct extras_atomic *a);
void extras_atomic_abort(struct extras_atomic *a);
bool extras_write_all(int fd, const void *buf, size_t len);
bool extras_copy_range(int src_fd, int dst_fd, uint32_t start, uint32_t end);
bool extras_copy_to_eof(int src_fd, int dst_fd, uint32_t start);

/* ---- Buffered property reader with exact raw byte spans ---- */
struct extras_reader {
    int fd;
    int len, pos;
    uint32_t offset;
    unsigned char buf[512];
};

void extras_reader_init(struct extras_reader *r, int fd,
                        uint32_t start_offset);
/* Reads the next unfolded property into `line` (truncating if longer than
 * `line_size - 1`, while still advancing through the entire folded property).
 * `raw_start` and `raw_end` receive the exact byte span in the file so
 * unknown properties of any length can be copied verbatim. */
bool extras_read_prop(struct extras_reader *r, char *line, size_t line_size,
                      uint32_t *raw_start, uint32_t *raw_end,
                      bool qp_continue);

/* ---- RFC 5545 / vCard folding & text escaping ---- */
bool extras_write_folded(int fd, const char *prop_line);
void extras_escape_text(const char *in, char *out, size_t out_size);
void extras_unescape_text(char *s);
bool extras_write_text_prop(int fd, const char *prefix, const char *value);
/* Locate a delimiter outside quoted property parameters. */
char *extras_param_separator(char *s, char delimiter);
char *extras_trim(char *s);
bool extras_has_ext(const char *name, const char *ext);

#endif
