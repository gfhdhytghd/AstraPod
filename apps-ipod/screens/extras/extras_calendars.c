/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Extras > Calendars as stock iPod Classic 2.0.5 draws it:
 *   - Top menu: All Calendars, each /Calendars .ics calendar (X-WR-CALNAME
 *     or filename stem), To Do's, and Alarms (Beep / None / Off, persisted).
 *   - Month grid: 6x7 stock cells with bottom-right event flags and bell
 *     indicator for events with VALARM; wheel moves day by day across month
 *     boundaries, SELECT opens the day, MENU exits.
 *   - Day view: selectable list of the day's events (SELECT opens Event
 *     Detail), or "No events for this day" when empty.
 *   - To Do's: selectable list of VTODO items ("List of To Do's", SELECT
 *     opens To Do Detail), or stock's Instructions page when none exist.
 *   - Calendar VALARM reminders feed extras_alarm_ring_poll().
 *   - Rockbox calendar plugin .memo entries are still included in All
 *     Calendars as a fallback.
 ****************************************************************************/
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "string-extra.h"
#include "config.h"
#include "kernel.h"
#include "core_alloc.h"
#include "dir.h"
#include "file.h"
#include "font.h"
#include "lang.h"
#include "lcd.h"
#include "rbpaths.h"
#include "timefuncs.h"
#include "input/action.h"
#include "draw/text_wrap.h"
#include "draw/viewport.h"
#include "settings/settings.h"
#include "system/shutdown.h"
#include "system/strutil.h"
#include "ui/menu_pane.h"
#include "ui/native_theme_runtime.h"
#include "ui/retained.h"
#include "ui/scene.h"
#include "ui/statusbar.h"
#include "ui/stock_art.h"
#include "widgets/list.h"
#include "root_menu.h"
#include "extras_bar.h"
#include "extras_calendars.h"
#include "extras.h"
#include "extras_font.h"

#define MEMO_FILE PLUGIN_APPS_DATA_DIR "/.memo"
#define CAL_STATE_FILE ROCKBOX_DIR "/calendars.dat"
#define CAL_STATE_MAGIC 0x31444c43u  /* "CLD1" */
#define MEMO_MAX 128
#define CAL_FONT FONT_DIR "/14-helvetica-bold-calendar.pbf"

#define CAL_MAX_FILES 16
#define CAL_MAX_EVENTS 128
#define CAL_MAX_TODOS 64
#define CAL_MAX_REMINDERS 32
#define CAL_DAY_MAX 32

#define WHITE 0xffffff
#define BLACK 0x000000
#define GREY 0x666666                /* weekday heads, labels, other months */
#define BODY_GREY 0x474747           /* To Do's instructions paragraphs */
#define SCROLL_TRACK 0xe2e2e2
#define SCROLL_THUMB 0x999999
#define BELL_GOLD 0xf0b820
#define BELL_EDGE 0x9a6a00

/* The calendar page: its size, the month's baseline and the day's origin
 * and baseline below its top-left. */
#define ICON_W 130
#define MONTH_BASE 36
#define DAY_X 40
#define DAY_BASE 90
/* AstraPod_CalendarDigits_Image: ten cells, the strip's top this far above
 * the baseline. */
#define DIGIT_W 27
#define DIGIT_TOP 35
/* The pane: the page's top-left and the time's and weekday's baselines. */
#define PANE_ICON_X 176
#define PANE_ICON_Y 56
#define PANE_MID (MENU_PANE_X + (LCD_WIDTH - MENU_PANE_X) / 2)
#define TIME_BASE 34
#define WEEKDAY_BASE 221
/* The month: six rows of seven 41x30 cells under the weekday heads, each
 * number's origin and baseline inside its cell, and its event flag. */
#define CELL_X 4
#define CELL_Y 38
#define CELL_DX 45
#define CELL_DY 34
#define CELL_W 41
#define HEAD_BASE 34
#define NUM_X 4
#define NUM_BASE 15
#define FLAG_X 26
#define FLAG_Y 16
#define BELL_X 27
#define BELL_Y 3
/* Day / Detail / To Do pages. */
#define NO_EVENTS_BASE 114
#define TODO_X 24
#define TODO_W 280
#define TODO_HEAD_BASE 71
#define TODO_BODY_BASE 93
#define TODO_PARA_GAP 31
#define DETAIL_LABEL_RIGHT 86
#define DETAIL_VALUE_X 92
#define DETAIL_VALUE_R 308
#define DETAIL_TOP_BASE 36
#define DETAIL_ROW_STEP 18
#define DETAIL_NOTES_GAP 4
#define DETAIL_SCROLL_STEP 24
#define BAND_ROWS 16

enum {
    RRULE_NONE = 0,
    RRULE_DAILY,
    RRULE_WEEKLY,
    RRULE_MONTHLY,
    RRULE_YEARLY,
};

enum {
    TODO_STATUS_NONE = 0,
    TODO_STATUS_NEEDS_ACTION,
    TODO_STATUS_COMPLETED,
    TODO_STATUS_IN_PROCESS,
    TODO_STATUS_CANCELLED,
};

/* ---- dates ---- */

struct cal_date {
    int year, month, day;            /* month 1-12, day 1-31 */
};

static bool leap_year(int year)
{
    return (!(year % 4) && (year % 100)) || !(year % 400);
}

static int month_days(int year, int month)
{
    static const unsigned char days[12] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (month < 1 || month > 12)
        return 30;
    return days[month - 1] + (month == 2 && leap_year(year));
}

/* Days since 2000-01-01 (matches extras_alarm_ring.c). */
static long days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    long era = y / 400;
    unsigned yoe = y - era * 400;
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 730425;
}

static void civil_from_days(long z, struct cal_date *out)
{
    z += 730425;
    long era = z / 146097;
    unsigned doe = z - era * 146097;
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    out->day = (int)(doy - (153 * mp + 2) / 5 + 1);
    out->month = (int)(mp < 10 ? mp + 3 : mp - 9);
    out->year = (int)(yoe + era * 400) + (out->month <= 2);
}

/* 0 for Sunday. */
static int week_day(const struct cal_date *d)
{
    static const unsigned char shift[12] = {
        0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    int y = d->year - (d->month < 3);
    return (y + y / 4 - y / 100 + y / 400 + shift[d->month - 1] + d->day) % 7;
}

static void step_days(struct cal_date *d, int days)
{
    d->day += days;
    while (d->day < 1)
    {
        if (--d->month < 1)
        {
            d->month = 12;
            d->year--;
        }
        d->day += month_days(d->year, d->month);
    }
    while (d->day > month_days(d->year, d->month))
    {
        d->day -= month_days(d->year, d->month);
        if (++d->month > 12)
        {
            d->month = 1;
            d->year++;
        }
    }
}

static struct cal_date today(void)
{
    const struct tm *tm = get_time();
    return (struct cal_date){ tm->tm_year + 1900, tm->tm_mon + 1,
                              tm->tm_mday };
}

/* `lang` with M, D and Y replaced by `month`, `day` and `year`. */
static void format_date(char *buf, size_t size, int lang, const char *month,
                        int day, int year)
{
    size_t used = 0;
    buf[0] = '\0';
    for (const char *f = str(lang); *f && used < size; f++)
    {
        if (*f == 'M')
            used += snprintf(buf + used, size - used, "%s", month);
        else if (*f == 'D')
            used += snprintf(buf + used, size - used, "%d", day);
        else if (*f == 'Y')
            used += snprintf(buf + used, size - used, "%d", year);
        else
            used += snprintf(buf + used, size - used, "%c", *f);
    }
}

static void format_long_date(char *buf, size_t size, const struct cal_date *d)
{
    char month[24];
    int m = MIN(MAX(d->month, 1), 12);
    strlcpy(month, str(LANG_EXTRAS_CALENDARS_JANUARY + m - 1), sizeof(month));
    format_date(buf, size, LANG_EXTRAS_CALENDARS_DAY_TITLE, month,
                d->day, d->year);
}

static void format_clock_hm(char *buf, size_t size, int hour, int minute)
{
    if (global_settings.timeformat == 0)
        snprintf(buf, size, "%02d:%02d", hour, minute);
    else
        snprintf(buf, size, "%d:%02d %s", (hour + 11) % 12 + 1, minute,
                 hour < 12 ? "AM" : "PM");
}

/* ---- persistent alarm state ---- */

/* 0 = Beep, 1 = None (visual only), 2 = Off */
static int alarm_sound;
static bool state_loaded;

struct cal_state_disk {
    uint32_t magic;
    uint8_t alarm_sound;
    uint8_t reserved[3];
};

static void cal_state_load(void)
{
    if (state_loaded)
        return;
    state_loaded = true;
    int fd = open(CAL_STATE_FILE, O_RDONLY);
    if (fd < 0)
        return;
    struct cal_state_disk st;
    if (read(fd, &st, sizeof(st)) == (ssize_t)sizeof(st) &&
        st.magic == CAL_STATE_MAGIC && st.alarm_sound <= 2)
        alarm_sound = st.alarm_sound;
    close(fd);
}

static void cal_state_save(void)
{
    int fd = open(CAL_STATE_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return;
    struct cal_state_disk st = {
        .magic = CAL_STATE_MAGIC,
        .alarm_sound = (uint8_t)alarm_sound,
    };
    write(fd, &st, sizeof(st));
    close(fd);
}

/* ---- .ics data model & reminder cache ---- */

struct cal_file {
    char name[40];
    char path[80];
};

struct cal_rrule {
    uint8_t freq;
    uint8_t interval;
    uint16_t count;
    uint16_t until_year;
    uint8_t until_month;
    uint8_t until_day;
};

struct cal_event {
    uint16_t year, end_year;
    uint8_t month, day, hour, minute;      /* hour == 255 -> all-day */
    uint8_t end_month, end_day, end_hour, end_minute;
    int8_t cal_idx;                        /* -1 for .memo */
    uint8_t has_alarm;
    struct cal_rrule rrule;
    char summary[64];
    char location[48];
    char attendees[64];
    char notes[128];
};

struct cal_todo {
    uint16_t due_year;
    uint8_t due_month, due_day;
    uint8_t status;
    uint8_t priority;
    char summary[64];
    char notes[128];
};

/* Compact reminder cache kept in static RAM for extras_alarm_ring_poll(). */
struct cal_reminder {
    uint16_t year;
    uint8_t month, day, hour, minute;      /* hour == 255 -> all-day (00:00) */
    int16_t offset_min;                    /* VALARM TRIGGER in minutes */
    struct cal_rrule rrule;
    char summary[48];
};

struct cal_store {
    struct cal_event events[CAL_MAX_EVENTS];
    int event_count;
    struct cal_todo todos[CAL_MAX_TODOS];
    int todo_count;
};

static struct cal_file cal_files[CAL_MAX_FILES];
static int cal_file_count;

static struct cal_reminder cal_reminders[CAL_MAX_REMINDERS];
static int cal_reminder_count;
static bool reminders_loaded;

static struct cal_store *active_store;

/* ---- .memo fallback ---- */

struct memo {
    unsigned short year;
    unsigned char day, month, wday, type;
};

static struct memo memos[MEMO_MAX];
static int memo_count;

static bool memo_parse(const char *line, struct memo *m)
{
    for (int i = 0; i < 10; i++)
        if (line[i] < '0' || line[i] > '9')
            return false;
    m->day = (line[0] - '0') * 10 + line[1] - '0';
    m->month = (line[2] - '0') * 10 + line[3] - '0';
    m->year = ((line[4] - '0') * 10 + line[5] - '0') * 100 +
              (line[6] - '0') * 10 + line[7] - '0';
    m->wday = line[8] - '0';
    m->type = line[9] - '0';
    return m->day >= 1 && m->day <= 31 && m->month >= 1 && m->month <= 12 &&
           m->wday <= 6 && m->type <= 3;
}

static bool memo_on(const struct memo *m, const struct cal_date *d)
{
    switch (m->type)
    {
        case 0:
            return m->wday == (week_day(d) + 6) % 7;
        case 1:
            return m->day == d->day;
        case 2:
            return m->day == d->day && m->month == d->month;
        default:
            return m->day == d->day && m->month == d->month &&
                   m->year == d->year;
    }
}

static void memo_scan(const struct cal_date *d,
                      void (*found)(const char *text, void *data), void *data)
{
    char line[96];
    int fd = open(MEMO_FILE, O_RDONLY);
    if (!d)
        memo_count = 0;
    while (fd >= 0 && read_line(fd, line, sizeof(line)) > 0)
    {
        struct memo m;
        if (!memo_parse(line, &m))
            continue;
        if (!d && memo_count < MEMO_MAX)
            memos[memo_count++] = m;
        else if (d && memo_on(&m, d))
            found(line + 10, data);
    }
    if (fd >= 0)
        close(fd);
}

static bool has_memo(const struct cal_date *d)
{
    for (int i = 0; i < memo_count; i++)
        if (memo_on(&memos[i], d))
            return true;
    return false;
}

/* ---- recurrence & date matching ---- */

static bool date_matches_rrule(int sy, int sm, int sd,
                               int ey, int em, int ed, bool all_day,
                               const struct cal_rrule *r,
                               const struct cal_date *d)
{
    if (sy == 0 || sm < 1 || sm > 12 || sd < 1 || sd > 31)
        return false;
    long start = days_from_civil(sy, sm, sd);
    long target = days_from_civil(d->year, d->month, d->day);
    if (target < start)
        return false;
    if (r->until_year > 0)
    {
        long until = days_from_civil(r->until_year, r->until_month,
                                     r->until_day);
        if (target > until)
            return false;
    }
    if (r->freq == RRULE_NONE)
    {
        if (target == start)
            return true;
        if (ey > 0 && em >= 1 && ed >= 1)
        {
            long end = days_from_civil(ey, em, ed);
            if (all_day)
                return target >= start && target < end;
            return target >= start && target <= end;
        }
        return false;
    }
    int iv = r->interval ? r->interval : 1;
    long diff = target - start;
    if (r->freq == RRULE_DAILY)
    {
        if (diff % iv != 0)
            return false;
        return !r->count || diff / iv < r->count;
    }
    if (r->freq == RRULE_WEEKLY)
    {
        long step = 7L * iv;
        if (diff % step != 0)
            return false;
        return !r->count || diff / step < r->count;
    }
    if (r->freq == RRULE_MONTHLY)
    {
        int mdiff = (d->year - sy) * 12 + (d->month - sm);
        if (mdiff < 0 || mdiff % iv != 0 || d->day != sd)
            return false;
        return !r->count || mdiff / iv < r->count;
    }
    if (r->freq == RRULE_YEARLY)
    {
        int ydiff = d->year - sy;
        if (ydiff < 0 || ydiff % iv != 0 || d->month != sm || d->day != sd)
            return false;
        return !r->count || ydiff / iv < r->count;
    }
    return false;
}

static bool event_on_date(const struct cal_event *ev, const struct cal_date *d)
{
    return date_matches_rrule(ev->year, ev->month, ev->day,
                              ev->end_year, ev->end_month, ev->end_day,
                              ev->hour == 255, &ev->rrule, d);
}

/* ---- iCalendar (.ics) parser ---- */

static int parse_digits(const char *s, int n)
{
    int v = 0;
    for (int i = 0; i < n; i++)
    {
        if (s[i] < '0' || s[i] > '9')
            return -1;
        v = v * 10 + (s[i] - '0');
    }
    return v;
}

/* Parse YYYYMMDD or YYYYMMDDTHHMM[SS][Z]. Returns true on valid date. */
static bool ics_parse_datetime(const char *val, uint16_t *year, uint8_t *month,
                               uint8_t *day, uint8_t *hour, uint8_t *minute)
{
    int y = parse_digits(val, 4);
    int m = parse_digits(val + 4, 2);
    int d = parse_digits(val + 6, 2);
    if (y < 1970 || y > 2199 || m < 1 || m > 12 || d < 1 || d > 31)
        return false;
    *year = (uint16_t)y;
    *month = (uint8_t)m;
    *day = (uint8_t)d;
    if (hour && minute)
    {
        if (val[8] == 'T' || val[8] == 't')
        {
            int hh = parse_digits(val + 9, 2);
            int mm = parse_digits(val + 11, 2);
            if (hh >= 0 && hh <= 23 && mm >= 0 && mm <= 59)
            {
                *hour = (uint8_t)hh;
                *minute = (uint8_t)mm;
                return true;
            }
        }
        *hour = 255;
        *minute = 0;
    }
    return true;
}

/* Parse ISO 8601 duration like -PT15M, -PT1H30M, -P1D, PT0S into minutes. */
static bool ics_parse_trigger(const char *val, int16_t *offset_min)
{
    int sign = 1;
    if (*val == '-')
    {
        sign = -1;
        val++;
    }
    else if (*val == '+')
        val++;
    if (*val != 'P' && *val != 'p')
        return false;
    val++;
    long total = 0;
    bool in_time = false;
    bool any = false;
    while (*val)
    {
        if (*val == 'T' || *val == 't')
        {
            in_time = true;
            val++;
            continue;
        }
        if (*val < '0' || *val > '9')
            break;
        long num = 0;
        while (*val >= '0' && *val <= '9')
        {
            num = num * 10 + (*val - '0');
            val++;
        }
        char unit = (char)toupper((unsigned char)*val);
        if (!unit)
            break;
        val++;
        any = true;
        if (!in_time && unit == 'W')
            total += num * 7L * 1440L;
        else if (!in_time && unit == 'D')
            total += num * 1440L;
        else if (in_time && unit == 'H')
            total += num * 60L;
        else if (in_time && unit == 'M')
            total += num;
        else if (in_time && unit == 'S')
            total += num / 60L;
    }
    if (!any)
        return false;
    total *= sign;
    if (total < -30000)
        total = -30000;
    if (total > 30000)
        total = 30000;
    *offset_min = (int16_t)total;
    return true;
}

static void ics_parse_rrule(const char *val, struct cal_rrule *r)
{
    memset(r, 0, sizeof(*r));
    r->interval = 1;
    const char *p = val;
    while (*p)
    {
        const char *semi = strchr(p, ';');
        size_t len = semi ? (size_t)(semi - p) : strlen(p);
        if (len > 5 && strncasecmp(p, "FREQ=", 5) == 0)
        {
            const char *f = p + 5;
            size_t fl = len - 5;
            if (fl == 5 && strncasecmp(f, "DAILY", 5) == 0)
                r->freq = RRULE_DAILY;
            else if (fl == 6 && strncasecmp(f, "WEEKLY", 6) == 0)
                r->freq = RRULE_WEEKLY;
            else if (fl == 7 && strncasecmp(f, "MONTHLY", 7) == 0)
                r->freq = RRULE_MONTHLY;
            else if (fl == 6 && strncasecmp(f, "YEARLY", 6) == 0)
                r->freq = RRULE_YEARLY;
        }
        else if (len > 9 && strncasecmp(p, "INTERVAL=", 9) == 0)
        {
            int iv = atoi(p + 9);
            r->interval = (uint8_t)MIN(MAX(iv, 1), 255);
        }
        else if (len > 6 && strncasecmp(p, "COUNT=", 6) == 0)
        {
            int cnt = atoi(p + 6);
            if (cnt > 0)
                r->count = (uint16_t)MIN(cnt, 65535);
        }
        else if (len >= 14 && strncasecmp(p, "UNTIL=", 6) == 0)
        {
            ics_parse_datetime(p + 6, &r->until_year, &r->until_month,
                               &r->until_day, NULL, NULL);
        }
        if (!semi)
            break;
        p = semi + 1;
    }
}

/* Unescape RFC 2445 TEXT value (\n, \,, \;, \\). */
static void ics_unescape(const char *src, char *dst, size_t size,
                         bool keep_newlines)
{
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 1 < size; i++)
    {
        if (src[i] == '\\' && src[i + 1])
        {
            i++;
            if (src[i] == 'n' || src[i] == 'N')
                dst[j++] = keep_newlines ? '\n' : ' ';
            else
                dst[j++] = src[i];
        }
        else
            dst[j++] = src[i];
    }
    dst[j] = '\0';
}

/* Extract CN= parameter or strip mailto: from ATTENDEE. */
static void ics_append_attendee(const char *params, const char *val,
                                char *dst, size_t size)
{
    char name[48];
    name[0] = '\0';
    if (params)
    {
        const char *p = params;
        while (*p)
        {
            if (strncasecmp(p, "CN=", 3) == 0)
            {
                p += 3;
                bool quoted = (*p == '"');
                if (quoted)
                    p++;
                size_t k = 0;
                while (*p && k + 1 < sizeof(name))
                {
                    if (quoted && *p == '"')
                        break;
                    if (!quoted && *p == ';')
                        break;
                    name[k++] = *p++;
                }
                name[k] = '\0';
                break;
            }
            const char *semi = strchr(p, ';');
            if (!semi)
                break;
            p = semi + 1;
        }
    }
    if (!name[0])
    {
        if (strncasecmp(val, "mailto:", 7) == 0)
            val += 7;
        strlcpy(name, val, sizeof(name));
    }
    if (!name[0])
        return;
    if (dst[0])
        strlcat(dst, ", ", size);
    strlcat(dst, name, size);
}

struct ics_reader {
    int fd;
    bool has_peek;
    char peek[256];
};

static void strip_eol(char *s)
{
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\r' || s[len - 1] == '\n'))
        s[--len] = '\0';
}

/* Read one unfolded RFC 2445 line into `out`. Returns false at EOF. */
static bool ics_next_line(struct ics_reader *r, char *out, size_t size)
{
    if (r->has_peek)
    {
        strlcpy(out, r->peek, size);
        r->has_peek = false;
    }
    else
    {
        if (read_line(r->fd, out, size) <= 0)
            return false;
        strip_eol(out);
    }
    while (read_line(r->fd, r->peek, sizeof(r->peek)) > 0)
    {
        strip_eol(r->peek);
        if (r->peek[0] == ' ' || r->peek[0] == '\t')
            strlcat(out, r->peek + 1, size);
        else
        {
            r->has_peek = true;
            break;
        }
    }
    return true;
}

static void parse_ics_file(int cal_idx, struct cal_store *store,
                           bool read_calname)
{
    int fd = open(cal_files[cal_idx].path, O_RDONLY);
    if (fd < 0)
        return;

    struct ics_reader r = { .fd = fd, .has_peek = false };
    char line[512];
    enum { SEC_ROOT, SEC_VEVENT, SEC_VTODO, SEC_VALARM } sec = SEC_ROOT;
    struct cal_event ev;
    struct cal_todo td;
    bool ev_alarm_valid = false;
    bool ev_alarm_ignore = false;
    int16_t ev_alarm_offset = 0;

    while (ics_next_line(&r, line, sizeof(line)))
    {
        char *colon = strchr(line, ':');
        if (!colon)
            continue;
        *colon = '\0';
        char *val = colon + 1;
        char *semi = strchr(line, ';');
        char *params = NULL;
        if (semi)
        {
            *semi = '\0';
            params = semi + 1;
        }
        const char *prop = line;

        if (strcasecmp(prop, "BEGIN") == 0)
        {
            if (strcasecmp(val, "VEVENT") == 0)
            {
                sec = SEC_VEVENT;
                memset(&ev, 0, sizeof(ev));
                ev.cal_idx = (int8_t)cal_idx;
                ev.hour = 255;
                ev.end_hour = 255;
                ev_alarm_valid = false;
                ev_alarm_ignore = false;
                ev_alarm_offset = 0;
            }
            else if (strcasecmp(val, "VTODO") == 0)
            {
                sec = SEC_VTODO;
                memset(&td, 0, sizeof(td));
            }
            else if (strcasecmp(val, "VALARM") == 0 && sec == SEC_VEVENT)
            {
                sec = SEC_VALARM;
                ev_alarm_ignore = false;
            }
            continue;
        }
        if (strcasecmp(prop, "END") == 0)
        {
            if (strcasecmp(val, "VALARM") == 0 && sec == SEC_VALARM)
            {
                sec = SEC_VEVENT;
                if (ev_alarm_ignore)
                    ev_alarm_valid = false;
            }
            else if (strcasecmp(val, "VEVENT") == 0 && sec == SEC_VEVENT)
            {
                sec = SEC_ROOT;
                if (ev.year > 0)
                {
                    if (!ev.summary[0])
                        strlcpy(ev.summary, "(No Title)", sizeof(ev.summary));
                    ev.has_alarm = ev_alarm_valid ? 1 : 0;
                    if (store && store->event_count < CAL_MAX_EVENTS)
                        store->events[store->event_count++] = ev;
                    if (ev_alarm_valid &&
                        cal_reminder_count < CAL_MAX_REMINDERS)
                    {
                        struct cal_reminder *rem =
                            &cal_reminders[cal_reminder_count++];
                        rem->year = ev.year;
                        rem->month = ev.month;
                        rem->day = ev.day;
                        rem->hour = ev.hour;
                        rem->minute = ev.minute;
                        rem->offset_min = ev_alarm_offset;
                        rem->rrule = ev.rrule;
                        strlcpy(rem->summary, ev.summary,
                                sizeof(rem->summary));
                    }
                }
            }
            else if (strcasecmp(val, "VTODO") == 0 && sec == SEC_VTODO)
            {
                sec = SEC_ROOT;
                if (store && store->todo_count < CAL_MAX_TODOS &&
                    td.summary[0])
                    store->todos[store->todo_count++] = td;
            }
            continue;
        }

        if (sec == SEC_ROOT)
        {
            if (read_calname && strcasecmp(prop, "X-WR-CALNAME") == 0 &&
                val[0])
                ics_unescape(val, cal_files[cal_idx].name,
                             sizeof(cal_files[cal_idx].name), false);
        }
        else if (sec == SEC_VEVENT)
        {
            if (strcasecmp(prop, "DTSTART") == 0)
            {
                ics_parse_datetime(val, &ev.year, &ev.month, &ev.day,
                                   &ev.hour, &ev.minute);
                if (params && strcasestr(params, "VALUE=DATE"))
                    ev.hour = 255;
            }
            else if (strcasecmp(prop, "DTEND") == 0)
            {
                ics_parse_datetime(val, &ev.end_year, &ev.end_month,
                                   &ev.end_day, &ev.end_hour, &ev.end_minute);
                if (params && strcasestr(params, "VALUE=DATE"))
                    ev.end_hour = 255;
            }
            else if (strcasecmp(prop, "SUMMARY") == 0)
                ics_unescape(val, ev.summary, sizeof(ev.summary), false);
            else if (strcasecmp(prop, "LOCATION") == 0)
                ics_unescape(val, ev.location, sizeof(ev.location), false);
            else if (strcasecmp(prop, "DESCRIPTION") == 0)
                ics_unescape(val, ev.notes, sizeof(ev.notes), true);
            else if (strcasecmp(prop, "ATTENDEE") == 0)
                ics_append_attendee(params, val, ev.attendees,
                                    sizeof(ev.attendees));
            else if (strcasecmp(prop, "RRULE") == 0)
                ics_parse_rrule(val, &ev.rrule);
        }
        else if (sec == SEC_VALARM)
        {
            if (strcasecmp(prop, "ACTION") == 0 &&
                strcasecmp(val, "NONE") == 0)
                ev_alarm_ignore = true;
            else if (strcasecmp(prop, "TRIGGER") == 0)
            {
                int16_t off = 0;
                if (ics_parse_trigger(val, &off))
                {
                    ev_alarm_offset = off;
                    ev_alarm_valid = true;
                }
            }
        }
        else if (sec == SEC_VTODO)
        {
            if (strcasecmp(prop, "SUMMARY") == 0)
                ics_unescape(val, td.summary, sizeof(td.summary), false);
            else if (strcasecmp(prop, "DUE") == 0)
                ics_parse_datetime(val, &td.due_year, &td.due_month,
                                   &td.due_day, NULL, NULL);
            else if (strcasecmp(prop, "STATUS") == 0)
            {
                if (strcasecmp(val, "NEEDS-ACTION") == 0)
                    td.status = TODO_STATUS_NEEDS_ACTION;
                else if (strcasecmp(val, "COMPLETED") == 0)
                    td.status = TODO_STATUS_COMPLETED;
                else if (strcasecmp(val, "IN-PROCESS") == 0)
                    td.status = TODO_STATUS_IN_PROCESS;
                else if (strcasecmp(val, "CANCELLED") == 0)
                    td.status = TODO_STATUS_CANCELLED;
            }
            else if (strcasecmp(prop, "PRIORITY") == 0)
            {
                int pr = atoi(val);
                if (pr >= 1 && pr <= 9)
                    td.priority = (uint8_t)pr;
            }
            else if (strcasecmp(prop, "DESCRIPTION") == 0)
                ics_unescape(val, td.notes, sizeof(td.notes), true);
        }
    }
    close(fd);
}

static int cmp_cal_file(const void *a, const void *b)
{
    const struct cal_file *fa = a;
    const struct cal_file *fb = b;
    return strcasecmp(fa->name, fb->name);
}

static int cmp_todo(const void *a, const void *b)
{
    const struct cal_todo *ta = a;
    const struct cal_todo *tb = b;
    return strcasecmp(ta->summary, tb->summary);
}

/* Scan /Calendars .ics files; fills cal_files[], cal_reminders[], and (when
 * non-NULL) `store`. */
static void scan_calendars(struct cal_store *store)
{
    cal_file_count = 0;
    cal_reminder_count = 0;
    reminders_loaded = true;
    if (store)
    {
        store->event_count = 0;
        store->todo_count = 0;
    }

    static const char *const dirs[2] = { "/Calendars", "/calendars" };
    const char *base = NULL;
    DIR *d = NULL;
    for (int i = 0; i < 2 && !d; i++)
    {
        d = opendir(dirs[i]);
        if (d)
            base = dirs[i];
    }
    if (!d)
        return;

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL && cal_file_count < CAL_MAX_FILES)
    {
        if (entry->d_name[0] == '.')
            continue;
        struct dirinfo info = dir_get_info(d, entry);
        if (info.attribute & ATTR_DIRECTORY)
            continue;
        size_t len = strlen(entry->d_name);
        if (len <= 4 || strcasecmp(entry->d_name + len - 4, ".ics") != 0)
            continue;
        struct cal_file *cf = &cal_files[cal_file_count++];
        size_t stem = MIN(len - 4, sizeof(cf->name) - 1);
        memcpy(cf->name, entry->d_name, stem);
        cf->name[stem] = '\0';
        snprintf(cf->path, sizeof(cf->path), "%s/%s", base, entry->d_name);
    }
    closedir(d);

    /* First pass: read X-WR-CALNAME so sorting uses display names. */
    for (int i = 0; i < cal_file_count; i++)
        parse_ics_file(i, NULL, true);

    if (cal_file_count > 1)
        qsort(cal_files, cal_file_count, sizeof(cal_files[0]), cmp_cal_file);

    /* Second pass: populate store and reminders with final sorted cal_idx. */
    cal_reminder_count = 0;
    for (int i = 0; i < cal_file_count; i++)
        parse_ics_file(i, store, false);

    if (store && store->todo_count > 1)
        qsort(store->todos, store->todo_count, sizeof(store->todos[0]),
              cmp_todo);
}

/* ---- calendar reminder API for extras_alarm_ring.c ---- */

bool extras_calendars_alarms_armed(void)
{
    cal_state_load();
    if (alarm_sound == 2)
        return false;
    if (!reminders_loaded)
        scan_calendars(NULL);
    return cal_reminder_count > 0;
}

bool extras_calendars_due_event(long minute, char *summary, size_t size,
                                int *hour, int *min, int *sound_mode)
{
    static long fired_minute = -1;
    static uint32_t fired_mask;

    cal_state_load();
    if (alarm_sound == 2)
        return false;
    if (!reminders_loaded)
        scan_calendars(NULL);
    if (minute != fired_minute)
    {
        fired_minute = minute;
        fired_mask = 0;
    }
    for (int i = 0; i < cal_reminder_count; i++)
    {
        if (i < 32 && (fired_mask & (1u << i)))
            continue;
        const struct cal_reminder *r = &cal_reminders[i];
        long ev_min = minute - r->offset_min;
        if (ev_min < 0)
            continue;
        long ev_day = ev_min / 1440;
        int ev_tod = (int)(ev_min % 1440);
        int ev_h = ev_tod / 60;
        int ev_m = ev_tod % 60;
        int want_h = r->hour == 255 ? 0 : r->hour;
        int want_m = r->hour == 255 ? 0 : r->minute;
        if (ev_h != want_h || ev_m != want_m)
            continue;
        struct cal_date d;
        civil_from_days(ev_day, &d);
        if (date_matches_rrule(r->year, r->month, r->day, 0, 0, 0,
                               r->hour == 255, &r->rrule, &d))
        {
            if (i < 32)
                fired_mask |= (1u << i);
            if (summary && size)
                strlcpy(summary, r->summary, size);
            if (hour)
                *hour = want_h;
            if (min)
                *min = want_m;
            if (sound_mode)
                *sound_mode = alarm_sound;
            return true;
        }
    }
    return false;
}

/* ---- the calendar page and the pane ---- */

static int pane_font(void)
{
    static int font = -1;
    static unsigned generation;
    if (font < 0 || generation != ui_theme_generation())
    {
        font_unload(font);
        font = ui_theme_current_font_load(UI_FONT_PANE_TITLE,
                                          global_settings.glyphs_to_cache);
        generation = ui_theme_generation();
    }
    return font;
}

static int text_width(int font, const char *text)
{
    int w = 0;
    font_getstringsize((const unsigned char *)text, &w, NULL, font);
    return w;
}

static void put_text(struct stock_canvas *c, int font, const char *text,
                     int x, int base, uint32_t rgb)
{
    if (font >= 0)
        stock_canvas_text(c, font, text, x, base - font_get(font)->ascent,
                          rgb);
}

static void put_pixel(struct stock_canvas *c, int x, int y, uint32_t rgb)
{
    int cx = x - c->x;
    int cy = y - c->y;
    if (cx >= 0 && cx < c->width && cy >= 0 && cy < c->height)
        c->pixels[cy * c->width + cx] = rgb;
}

/* Original 9x9 golden bell glyph for cells with a VALARM event. */
static void put_bell(struct stock_canvas *c, int x, int y)
{
    static const char *const glyph[9] = {
        "...###...",
        "..#ooo#..",
        "..#ooo#..",
        ".#ooooo#.",
        ".#ooooo#.",
        "#ooooooo#",
        "#########",
        "...###...",
        "....#....",
    };
    for (int row = 0; row < 9; row++)
        for (int col = 0; col < 9; col++)
        {
            char ch = glyph[row][col];
            if (ch == '#')
                put_pixel(c, x + col, y + row, BELL_EDGE);
            else if (ch == 'o')
                put_pixel(c, x + col, y + row, BELL_GOLD);
        }
}

/* Black digit `n` of the strip, its coverage blended as stock blends text. */
static void put_digit(struct stock_canvas *c, int n, int x, int base)
{
    const struct stock_art *a = &stock_art[STOCK_ART_ASTRAPOD_CALENDARDIGITS];
    if (!stock_art_ready())
        return;
    for (int row = 0; row < a->height; row++)
    {
        int y = base - DIGIT_TOP + row - c->y;
        if (y < 0 || y >= c->height)
            continue;
        for (int col = 0; col < DIGIT_W; col++)
        {
            unsigned cover = stock_art_alpha(a, n * DIGIT_W + col, row);
            int cx = x + col - c->x;
            if (!cover || cx < 0 || cx >= c->width)
                continue;
            uint32_t *p = c->pixels + y * c->width + cx;
            uint32_t out = 0;
            for (int shift = 0; shift < 24; shift += 8)
                out |= (((*p >> shift & 255) * (255 - cover) + 1) * 257 >> 16)
                       << shift;
            *p = out;
        }
    }
}

void extras_calendars_icon_put(struct stock_canvas *canvas, int x, int y,
                               int month, int day)
{
    int font = pane_font();
    const char *name = str(LANG_MONTH_JANUARY + month - 1);
    stock_art_put(canvas, STOCK_ART_SETTINGS_DATETIME_CALENDAR, x, y);
    put_text(canvas, font, name, x + (ICON_W - text_width(font, name)) / 2,
             y + MONTH_BASE, WHITE);
    put_digit(canvas, day / 10, x + DAY_X, y + DAY_BASE);
    put_digit(canvas, day % 10, x + DAY_X + DIGIT_W, y + DAY_BASE);
}

static void put_centred(struct stock_canvas *c, int font, const char *text,
                        int mid, int base, uint32_t rgb)
{
    put_text(c, font, text, mid - text_width(font, text) / 2, base, rgb);
}

static void pane_paint(struct stock_canvas *band)
{
    static const int weekday[7] = {
        LANG_EXTRAS_CALENDARS_SUNDAY, LANG_EXTRAS_CALENDARS_MONDAY,
        LANG_EXTRAS_CALENDARS_TUESDAY, LANG_EXTRAS_CALENDARS_WEDNESDAY,
        LANG_EXTRAS_CALENDARS_THURSDAY, LANG_EXTRAS_CALENDARS_FRIDAY,
        LANG_EXTRAS_CALENDARS_SATURDAY,
    };
    const struct tm *tm = get_time();
    struct cal_date d = today();
    char time[16];
    format_clock_hm(time, sizeof(time), tm->tm_hour, tm->tm_min);
    int font = pane_font();
    put_centred(band, font, time, PANE_MID, TIME_BASE, WHITE);
    extras_calendars_icon_put(band, PANE_ICON_X, PANE_ICON_Y, d.month, d.day);
    put_centred(band, font, str(weekday[week_day(&d)]), PANE_MID,
                WEEKDAY_BASE, WHITE);
}

void extras_calendars_pane(struct menu_pane_content *c)
{
    const struct tm *tm = get_time();
    memset(c, 0, sizeof(*c));
    c->kind = LIST_PANE_PICTURE;
    c->art = -1;
    c->paint = pane_paint;
    c->stamp = (tm->tm_yday * 24 + tm->tm_hour) * 60 + tm->tm_min;
}

/* ---- generic banded page runner ---- */

struct detail_field {
    const char *label;
    char value[160];
    bool extra_gap;
};

struct page {
    void (*compose)(struct stock_canvas *band, struct page *p);
    uint32_t *band;
    struct cal_date shown;
    int cal_filter;                  /* -1 = All Calendars, >=0 = specific */
    int font, head_font, body_font;
    int ret;
    int scroll_y, scroll_max;
    struct detail_field fields[7];
    int field_count;
};

static void page_paint(const struct ui_view *view,
                       const struct ui_view_frame *frame)
{
    struct page *p = view->data;
    struct ui_rect clip = frame->clip;
    for (int y = clip.y; y < clip.y + clip.height; y += BAND_ROWS)
    {
        struct stock_canvas band = {
            p->band, clip.x, y, clip.width,
            MIN(BAND_ROWS, clip.y + clip.height - y),
        };
        for (int i = 0; i < band.width * band.height; i++)
            band.pixels[i] = WHITE;
        p->compose(&band, p);
        stock_canvas_show(&band, clip);
    }
}

static const struct ui_view_ops page_ops = { .paint = page_paint };

static int page_run(struct page *p, char *title, size_t size,
                    bool (*key)(struct page *p, int action, char *title,
                                size_t size))
{
    struct viewport content;
    struct ui_view view;
    struct ui_page page;
    struct ui_scene scene = {0};
    int handle = core_alloc(LCD_WIDTH * BAND_ROWS * sizeof(uint32_t));
    int ret = 0;

    if (handle <= 0)
        return 0;
    p->band = core_get_data_pinned(handle);
    ui_scene_open_page(&scene, "calendars-push");
    sb_set_persistent_title(title, Icon_NOICON, SCREEN_MAIN);
    viewportmanager_theme_enable(SCREEN_MAIN, true, &content);
    struct ui_rect bounds = { content.x, content.y, content.width,
                              content.height };
    ui_view_init(&view, bounds, &page_ops, p);
    ui_page_init(&page, &view);

    for (bool dirty = true; ; )
    {
        if (dirty)
        {
            ui_page_invalidate(&page, bounds);
            ui_page_draw(&page);
        }
        int action = extras_get_action(HZ, true);
        if (action == ACTION_STD_CANCEL || action == ACTION_STD_MENU)
            break;
        if (default_event_handler(action) == SYS_USB_CONNECTED)
        {
            ret = GO_TO_ROOT;
            break;
        }
        dirty = key && key(p, action, title, size);
        if (p->ret)
        {
            ret = p->ret;
            break;
        }
        if (dirty)
        {
            sb_set_persistent_title(title, Icon_NOICON, SCREEN_MAIN);
            sb_update(SCREEN_MAIN, true);
        }
    }

    ui_scene_close_modal(&scene, ret == 0, "calendars-pop");
    viewportmanager_theme_undo(SCREEN_MAIN, false);
    core_put_data_pinned(p->band);
    core_free(handle);
    return ret;
}

/* ---- detail page (shared by Event Detail and To Do Detail) ---- */

static void draw_scrollbar(struct stock_canvas *band, int scroll_y,
                           int scroll_max)
{
    if (scroll_max <= 0)
        return;
    int top = 22;
    int height = LCD_HEIGHT - top - 2;
    int total = height + scroll_max;
    int thumb_h = MAX(20, height * height / total);
    int thumb_y = top + (height - thumb_h) * scroll_y / scroll_max;
    for (int y = MAX(band->y, top); y < MIN(band->y + band->height, top + height);
         y++)
    {
        uint32_t col = (y >= thumb_y && y < thumb_y + thumb_h) ?
                       SCROLL_THUMB : SCROLL_TRACK;
        for (int x = LCD_WIDTH - 6; x < LCD_WIDTH - 2; x++)
            put_pixel(band, x, y, col);
    }
}

static int detail_measure(struct page *p)
{
    struct font *pf = font_get(p->font);
    int val_w = DETAIL_VALUE_R - DETAIL_VALUE_X;
    int base = DETAIL_TOP_BASE;
    font_lock(p->font, true);
    for (int i = 0; i < p->field_count; i++)
    {
        if (p->fields[i].extra_gap && i > 0)
            base += DETAIL_NOTES_GAP;
        const char *at = p->fields[i].value;
        int lines = 0;
        while (*at)
        {
            const char *nl = strchr(at, '\n');
            char seg[160];
            if (nl)
            {
                size_t n = MIN((size_t)(nl - at), sizeof(seg) - 1);
                memcpy(seg, at, n);
                seg[n] = '\0';
            }
            else
                strlcpy(seg, at, sizeof(seg));
            if (!seg[0])
                lines++;
            else
            {
                const char *cur = seg, *end;
                while (*cur)
                {
                    cur = text_wrap_next(cur, val_w, pf, &end);
                    lines++;
                }
            }
            if (!nl)
                break;
            at = nl + 1;
        }
        if (lines < 1)
            lines = 1;
        base += lines * DETAIL_ROW_STEP;
    }
    font_lock(p->font, false);
    int bottom = base + 8;
    return MAX(0, bottom - LCD_HEIGHT);
}

static void detail_compose(struct stock_canvas *band, struct page *p)
{
    struct font *pf = font_get(p->font);
    int val_w = (p->scroll_max > 0 ? DETAIL_VALUE_R - 6 : DETAIL_VALUE_R) -
                DETAIL_VALUE_X;
    int base = DETAIL_TOP_BASE - p->scroll_y;
    char line[160];
    font_lock(p->font, true);
    for (int i = 0; i < p->field_count; i++)
    {
        if (p->fields[i].extra_gap && i > 0)
            base += DETAIL_NOTES_GAP;
        char lbl[32];
        snprintf(lbl, sizeof(lbl), "%s:", p->fields[i].label);
        int lf = extras_font_for(p->font, lbl);
        int lw = text_width(lf, lbl);
        put_text(band, lf, lbl, DETAIL_LABEL_RIGHT - lw, base, GREY);

        const char *at = p->fields[i].value;
        int lines = 0;
        while (*at)
        {
            const char *nl = strchr(at, '\n');
            char seg[160];
            if (nl)
            {
                size_t n = MIN((size_t)(nl - at), sizeof(seg) - 1);
                memcpy(seg, at, n);
                seg[n] = '\0';
            }
            else
                strlcpy(seg, at, sizeof(seg));
            if (!seg[0])
            {
                base += DETAIL_ROW_STEP;
                lines++;
            }
            else
            {
                const char *cur = seg, *end;
                while (*cur)
                {
                    const char *next = text_wrap_next(cur, val_w, pf, &end);
                    strlcpy(line, cur, MIN((size_t)(end - cur) + 1,
                                           sizeof(line)));
                    int vf = extras_font_for(p->font, line);
                    put_text(band, vf, line, DETAIL_VALUE_X, base, BLACK);
                    base += DETAIL_ROW_STEP;
                    lines++;
                    cur = next;
                }
            }
            if (!nl)
                break;
            at = nl + 1;
        }
        if (lines == 0)
            base += DETAIL_ROW_STEP;
    }
    font_lock(p->font, false);
    draw_scrollbar(band, p->scroll_y, p->scroll_max);
}

static bool detail_key(struct page *p, int action, char *title, size_t size)
{
    (void)title; (void)size;
    if (p->scroll_max <= 0)
        return false;
    int prev = p->scroll_y;
    if (action == ACTION_STD_NEXT || action == ACTION_STD_NEXTREPEAT)
        p->scroll_y = MIN(p->scroll_max, p->scroll_y + DETAIL_SCROLL_STEP);
    else if (action == ACTION_STD_PREV || action == ACTION_STD_PREVREPEAT)
        p->scroll_y = MAX(0, p->scroll_y - DETAIL_SCROLL_STEP);
    return p->scroll_y != prev;
}

static void detail_add(struct page *p, int lang, const char *val,
                       bool extra_gap)
{
    if (!val || !val[0] || p->field_count >= (int)ARRAYLEN(p->fields))
        return;
    struct detail_field *f = &p->fields[p->field_count++];
    f->label = str(lang);
    strlcpy(f->value, val, sizeof(f->value));
    f->extra_gap = extra_gap;
}

static int run_detail_page(struct page *p, char *title, size_t size)
{
    p->compose = detail_compose;
    p->font = ui_theme_current_font_load(UI_FONT_LIST_VALUE,
                                         global_settings.glyphs_to_cache);
    if (p->font < 0)
        p->font = pane_font();
    p->scroll_y = 0;
    p->scroll_max = detail_measure(p);
    int ret = page_run(p, title, size, detail_key);
    font_unload(p->font);
    return ret;
}

static int event_detail_page(const struct cal_event *ev,
                             const struct cal_date *d)
{
    static struct page p;
    char title[48], date_str[48], time_str[48];
    memset(&p, 0, sizeof(p));
    format_long_date(title, sizeof(title), d);
    strlcpy(date_str, title, sizeof(date_str));

    if (ev->hour == 255)
        strlcpy(time_str, str(LANG_EXTRAS_CALENDARS_ALL_DAY),
                sizeof(time_str));
    else
    {
        char start_s[20];
        format_clock_hm(start_s, sizeof(start_s), ev->hour, ev->minute);
        if (ev->end_hour != 255)
        {
            char end_s[20];
            format_clock_hm(end_s, sizeof(end_s), ev->end_hour,
                            ev->end_minute);
            snprintf(time_str, sizeof(time_str), "%s - %s", start_s, end_s);
        }
        else
            strlcpy(time_str, start_s, sizeof(time_str));
    }

    detail_add(&p, LANG_EXTRAS_CALENDARS_SUMMARY, ev->summary, false);
    detail_add(&p, LANG_EXTRAS_CALENDARS_DATE, date_str, false);
    detail_add(&p, LANG_EXTRAS_CALENDARS_TIME, time_str, false);
    detail_add(&p, LANG_EXTRAS_CALENDARS_LOCATION, ev->location, false);
    detail_add(&p, LANG_EXTRAS_CALENDARS_ATTENDEES, ev->attendees, false);
    detail_add(&p, LANG_EXTRAS_CALENDARS_NOTES, ev->notes, true);
    return run_detail_page(&p, title, sizeof(title));
}

/* ---- day view ---- */

struct day_list_ctx {
    struct cal_event items[CAL_DAY_MAX];
    int count;
};

static void day_add_memo(const char *text, void *data)
{
    struct day_list_ctx *ctx = data;
    if (ctx->count >= CAL_DAY_MAX)
        return;
    struct cal_event *ev = &ctx->items[ctx->count++];
    memset(ev, 0, sizeof(*ev));
    ev->cal_idx = -1;
    ev->hour = 255;
    ev->end_hour = 255;
    strlcpy(ev->summary, text, sizeof(ev->summary));
}

static int cmp_day_event(const void *a, const void *b)
{
    const struct cal_event *ea = a;
    const struct cal_event *eb = b;
    int ta = ea->hour == 255 ? -1 : ea->hour * 60 + ea->minute;
    int tb = eb->hour == 255 ? -1 : eb->hour * 60 + eb->minute;
    if (ta != tb)
        return ta - tb;
    return strcasecmp(ea->summary, eb->summary);
}

static const char *day_row_name(int i, void *data, char *buf, size_t size)
{
    (void)buf; (void)size;
    struct day_list_ctx *ctx = data;
    return ctx->items[i].summary;
}

static enum list_row_kind day_row_kind(int i, void *data)
{
    (void)i; (void)data;
    return LIST_ROW_MENU;
}

static void empty_day_compose(struct stock_canvas *band, struct page *p)
{
    put_centred(band, p->font, str(LANG_EXTRAS_CALENDARS_NO_EVENTS),
                LCD_WIDTH / 2, NO_EVENTS_BASE, BLACK);
}

static int day_page(struct cal_date *d, int cal_filter, int font)
{
    char title[48];
    format_long_date(title, sizeof(title), d);

    int h = core_alloc(sizeof(struct day_list_ctx));
    if (h <= 0)
        return 0;
    struct day_list_ctx *ctx = core_get_data_pinned(h);
    ctx->count = 0;

    if (active_store)
    {
        for (int i = 0; i < active_store->event_count &&
             ctx->count < CAL_DAY_MAX; i++)
        {
            const struct cal_event *ev = &active_store->events[i];
            if (cal_filter >= 0 && ev->cal_idx != cal_filter)
                continue;
            if (event_on_date(ev, d))
                ctx->items[ctx->count++] = *ev;
        }
    }
    if (cal_filter < 0)
        memo_scan(d, day_add_memo, ctx);

    if (ctx->count == 0)
    {
        core_put_data_pinned(ctx);
        core_free(h);
        static struct page p;
        p = (struct page){ .compose = empty_day_compose, .shown = *d,
                           .font = font };
        return page_run(&p, title, sizeof(title), NULL);
    }

    if (ctx->count > 1)
        qsort(ctx->items, ctx->count, sizeof(ctx->items[0]), cmp_day_event);

    struct gui_synclist list;
    struct ui_scene scene = {0};
    int action, ret = 0;

    ui_scene_open_page(&scene, "calendars-day-push");
    gui_synclist_init(&list, day_row_name, ctx, false, 1, NULL);
    gui_synclist_set_title(&list, title, Icon_NOICON);
    gui_synclist_set_row_callbacks(&list, day_row_kind, NULL, NULL);
    gui_synclist_set_nb_items(&list, ctx->count);
    gui_synclist_select_item(&list, 0);
    sb_set_persistent_title(title, Icon_NOICON, SCREEN_MAIN);
    viewportmanager_theme_enable(SCREEN_MAIN, true, NULL);
    gui_synclist_draw_settled(&list);

    while (!ret)
    {
        extras_list_do_action(HZ, &list, &action);
        if (action == ACTION_STD_OK)
        {
            int sel = gui_synclist_get_sel_pos(&list);
            if (sel >= 0 && sel < ctx->count)
                ret = event_detail_page(&ctx->items[sel], d);
            sb_set_persistent_title(title, Icon_NOICON, SCREEN_MAIN);
            gui_synclist_draw(&list);
        }
        else if (action == ACTION_STD_MENU || action == ACTION_STD_CANCEL)
            break;
        else if (default_event_handler(action) == SYS_USB_CONNECTED)
            ret = GO_TO_ROOT;
    }

    gui_synclist_scroll_stop(&list);
    ui_scene_close_modal(&scene, ret == 0, "calendars-day-pop");
    viewportmanager_theme_undo(SCREEN_MAIN, false);
    core_put_data_pinned(ctx);
    core_free(h);
    return ret;
}

/* ---- the month ---- */

static void month_title(const struct cal_date *d, char *buf, size_t size)
{
    format_date(buf, size, LANG_EXTRAS_CALENDARS_MONTH_TITLE,
                str(LANG_MONTH_JANUARY + d->month - 1), d->day, d->year);
}

static void date_flags(const struct cal_date *d, int cal_filter,
                       bool *has_ev, bool *has_alm)
{
    *has_ev = false;
    *has_alm = false;
    if (active_store)
    {
        for (int i = 0; i < active_store->event_count; i++)
        {
            const struct cal_event *ev = &active_store->events[i];
            if (cal_filter >= 0 && ev->cal_idx != cal_filter)
                continue;
            if (event_on_date(ev, d))
            {
                *has_ev = true;
                if (ev->has_alarm && alarm_sound != 2)
                    *has_alm = true;
                if (*has_alm)
                    return;
            }
        }
    }
    if (cal_filter < 0 && has_memo(d))
        *has_ev = true;
}

static void month_compose(struct stock_canvas *band, struct page *p)
{
    struct cal_date first = { p->shown.year, p->shown.month, 1 };
    int lead = week_day(&first);
    struct cal_date d = first;
    step_days(&d, -(lead ? lead : 7));
    for (int col = 0; col < 7; col++)
    {
        const char *head = str(LANG_WEEKDAY_SUNDAY + col);
        int x = CELL_X + col * CELL_DX, font = extras_font_for(p->font, head);
        put_text(band, font, head,
                 x + (CELL_W - text_width(font, head)) / 2, HEAD_BASE, GREY);
    }
    for (int i = 0; i < 42; i++, step_days(&d, 1))
    {
        int x = CELL_X + i % 7 * CELL_DX, y = CELL_Y + i / 7 * CELL_DY;
        if (y >= band->y + band->height || y + CELL_DY <= band->y)
            continue;
        bool in = d.month == p->shown.month;
        bool selected = in && d.day == p->shown.day;
        stock_art_put(band, selected ? STOCK_ART_CALENDAR_HILITE_CELL :
                      in ? STOCK_ART_CALENDAR_ACTIVE_CELL :
                      STOCK_ART_CALENDAR_INACTIVE_CELL, x, y);
        char num[4];
        snprintf(num, sizeof(num), "%d", d.day);
        put_text(band, p->font, num, x + NUM_X, y + NUM_BASE,
                 selected ? WHITE : in ? BLACK : GREY);
        bool has_ev = false, has_alm = false;
        date_flags(&d, p->cal_filter, &has_ev, &has_alm);
        if (has_ev)
            stock_art_put(band, STOCK_ART_CALENDAR_FLAG, x + FLAG_X,
                          y + FLAG_Y);
        if (has_alm)
            put_bell(band, x + BELL_X, y + BELL_Y);
    }
}

static bool month_key(struct page *p, int action, char *title, size_t size)
{
    switch (action)
    {
        case ACTION_STD_NEXT:
        case ACTION_STD_NEXTREPEAT:
            step_days(&p->shown, 1);
            break;
        case ACTION_STD_PREV:
        case ACTION_STD_PREVREPEAT:
            step_days(&p->shown, -1);
            break;
        case ACTION_STD_OK:
            p->ret = day_page(&p->shown, p->cal_filter, p->body_font);
            break;
        default:
            return false;
    }
    month_title(&p->shown, title, size);
    return true;
}

static int month_page(int cal_filter)
{
    char title[48];
    static struct page p;
    p = (struct page){ .compose = month_compose, .shown = today(),
                       .cal_filter = cal_filter };
    p.font = font_load_ex(CAL_FONT, 0, global_settings.glyphs_to_cache);
    if (p.font < 0)
        p.font = ui_theme_current_font_load(UI_FONT_LIST_VALUE,
                                            global_settings.glyphs_to_cache);
    p.body_font = pane_font();
    memo_scan(NULL, NULL, NULL);
    month_title(&p.shown, title, sizeof(title));
    int ret = page_run(&p, title, sizeof(title), month_key);
    font_unload(p.font);
    return ret;
}

/* ---- To Do's ---- */

static int put_paragraph(struct stock_canvas *c, int font, const char *text,
                         int base)
{
    struct font *pf = font_get(font);
    int pitch = pf->height;
    char line[160];
    font_lock(font, true);
    for (const char *at = text, *end; *at; base += pitch)
    {
        const char *next = text_wrap_next(at, TODO_W, pf, &end);
        strlcpy(line, at, MIN((size_t)(end - at) + 1, sizeof(line)));
        put_text(c, font, line, TODO_X, base, BODY_GREY);
        at = next;
        if (!*at)
            break;
    }
    font_lock(font, false);
    return base;
}

static void todo_howto_compose(struct stock_canvas *band, struct page *p)
{
    put_text(band, p->head_font, str(LANG_EXTRAS_CALENDARS_TODO_HOWTO),
             TODO_X, TODO_HEAD_BASE, BLACK);
    int base = put_paragraph(band, p->body_font,
                             str(LANG_EXTRAS_CALENDARS_TODO_TEXT),
                             TODO_BODY_BASE);
    put_paragraph(band, p->body_font, str(LANG_EXTRAS_NOTES_HOWTO_MORE),
                  base + TODO_PARA_GAP);
}

static int todo_detail_page(const struct cal_todo *td)
{
    static struct page p;
    char title[64], due_str[48], prio_str[8];
    memset(&p, 0, sizeof(p));
    strlcpy(title, td->summary, sizeof(title));

    detail_add(&p, LANG_EXTRAS_CALENDARS_SUMMARY, td->summary, false);
    if (td->due_year > 0)
    {
        struct cal_date d = { td->due_year, td->due_month, td->due_day };
        format_long_date(due_str, sizeof(due_str), &d);
        detail_add(&p, LANG_EXTRAS_CALENDARS_DUE, due_str, false);
    }
    if (td->status != TODO_STATUS_NONE)
    {
        static const int status_lang[] = {
            0,
            LANG_EXTRAS_CALENDARS_NEEDS_ACTION,
            LANG_EXTRAS_CALENDARS_COMPLETED,
            LANG_EXTRAS_CALENDARS_IN_PROCESS,
            LANG_EXTRAS_CALENDARS_CANCELLED,
        };
        detail_add(&p, LANG_EXTRAS_CALENDARS_STATUS,
                   str(status_lang[td->status]), false);
    }
    if (td->priority > 0)
    {
        snprintf(prio_str, sizeof(prio_str), "%u", td->priority);
        detail_add(&p, LANG_EXTRAS_CALENDARS_PRIORITY, prio_str, false);
    }
    detail_add(&p, LANG_EXTRAS_CALENDARS_NOTES, td->notes, true);
    return run_detail_page(&p, title, sizeof(title));
}

static const char *todo_row_name(int i, void *data, char *buf, size_t size)
{
    (void)data; (void)buf; (void)size;
    return active_store->todos[i].summary;
}

static enum list_row_kind todo_row_kind(int i, void *data)
{
    (void)i; (void)data;
    return LIST_ROW_MENU;
}

static int todo_page(void)
{
    if (!active_store || active_store->todo_count == 0)
    {
        char title[48];
        static struct page p;
        p = (struct page){ .compose = todo_howto_compose };
        p.head_font = ui_theme_current_font_load(UI_FONT_TITLE,
                                                 global_settings.glyphs_to_cache);
        p.body_font = ui_theme_current_font_load(UI_FONT_BODY,
                                                 global_settings.glyphs_to_cache);
        strlcpy(title, str(LANG_EXTRAS_NOTES_INSTRUCTIONS), sizeof(title));
        int ret = page_run(&p, title, sizeof(title), NULL);
        font_unload(p.head_font);
        font_unload(p.body_font);
        return ret;
    }

    struct gui_synclist list;
    struct ui_scene scene = {0};
    int action, ret = 0;

    ui_scene_open_page(&scene, "calendars-todo-push");
    gui_synclist_init(&list, todo_row_name, NULL, false, 1, NULL);
    gui_synclist_set_title(&list, str(LANG_EXTRAS_CALENDARS_TODO_LIST),
                           Icon_NOICON);
    gui_synclist_set_row_callbacks(&list, todo_row_kind, NULL, NULL);
    gui_synclist_set_nb_items(&list, active_store->todo_count);
    gui_synclist_select_item(&list, 0);
    sb_set_persistent_title(str(LANG_EXTRAS_CALENDARS_TODO_LIST), Icon_NOICON,
                            SCREEN_MAIN);
    viewportmanager_theme_enable(SCREEN_MAIN, true, NULL);
    gui_synclist_draw_settled(&list);

    while (!ret)
    {
        extras_list_do_action(HZ, &list, &action);
        if (action == ACTION_STD_OK)
        {
            int sel = gui_synclist_get_sel_pos(&list);
            if (sel >= 0 && sel < active_store->todo_count)
                ret = todo_detail_page(&active_store->todos[sel]);
            sb_set_persistent_title(str(LANG_EXTRAS_CALENDARS_TODO_LIST),
                                    Icon_NOICON, SCREEN_MAIN);
            gui_synclist_draw(&list);
        }
        else if (action == ACTION_STD_MENU || action == ACTION_STD_CANCEL)
            break;
        else if (default_event_handler(action) == SYS_USB_CONNECTED)
            ret = GO_TO_ROOT;
    }

    gui_synclist_scroll_stop(&list);
    ui_scene_close_modal(&scene, ret == 0, "calendars-todo-pop");
    viewportmanager_theme_undo(SCREEN_MAIN, false);
    return ret;
}

/* ---- the menu ---- */

static int menu_row_count(void)
{
    return 3 + cal_file_count;
}

static int row_todos_idx(void)
{
    return 1 + cal_file_count;
}

static int row_alarms_idx(void)
{
    return 2 + cal_file_count;
}

static const char *row_name(int i, void *data, char *buf, size_t size)
{
    (void)data; (void)buf; (void)size;
    if (i == 0)
        return str(LANG_EXTRAS_CALENDARS_ALL);
    if (i >= 1 && i <= cal_file_count)
        return cal_files[i - 1].name;
    if (i == row_todos_idx())
        return str(LANG_EXTRAS_CALENDARS_TODOS);
    return str(LANG_EXTRAS_CALENDARS_ALARMS);
}

static const char *row_value(int i, void *data, char *buf, size_t size)
{
    (void)data; (void)buf; (void)size;
    static const int sound[3] = {
        LANG_EXTRAS_CALENDARS_BEEP, LANG_EXTRAS_CALENDARS_NONE, LANG_OFF,
    };
    return i == row_alarms_idx() ? str(sound[alarm_sound]) : NULL;
}

static enum list_row_kind row_kind(int i, void *data)
{
    (void)data;
    return i == row_alarms_idx() ? LIST_ROW_SETTING : LIST_ROW_MENU;
}

static enum list_pane_kind row_pane(int i, void *data, int *art)
{
    (void)i; (void)data; (void)art;
    struct menu_pane_content pane;
    extras_calendars_pane(&pane);
    menu_pane_set_picture(&pane);
    return LIST_PANE_PICTURE;
}

int extras_calendars(void)
{
    static int selected;
    struct gui_synclist list;
    struct menu_pane_content pane;
    struct ui_scene scene = {0};
    int action, ret = 0;

    cal_state_load();
    int store_h = core_alloc(sizeof(struct cal_store));
    active_store = store_h > 0 ? core_get_data_pinned(store_h) : NULL;
    scan_calendars(active_store);

    int count = menu_row_count();
    if (selected >= count)
        selected = 0;

    ui_scene_open_page(&scene, "calendars-list-push");

    gui_synclist_init(&list, row_name, NULL, false, 1, NULL);
    gui_synclist_set_title(&list, str(LANG_EXTRAS_CALENDARS), Icon_NOICON);
    gui_synclist_set_row_callbacks(&list, row_kind, row_value, NULL);
    gui_synclist_set_pane_callback(&list, row_pane);
    gui_synclist_set_nb_items(&list, count);
    gui_synclist_select_item(&list, selected);
    sb_set_persistent_title(str(LANG_EXTRAS_CALENDARS), Icon_NOICON,
                            SCREEN_MAIN);
    viewportmanager_theme_enable(SCREEN_MAIN, true, NULL);
    gui_synclist_draw_settled(&list);
    extras_calendars_pane(&pane);
    int stamp = pane.stamp;

    while (!ret)
    {
        extras_list_do_action(HZ, &list, &action);
        selected = gui_synclist_get_sel_pos(&list);
        if (action == ACTION_STD_OK)
        {
            if (selected == row_alarms_idx())
            {
                alarm_sound = (alarm_sound + 1) % 3;
                cal_state_save();
            }
            else if (selected == row_todos_idx())
                ret = todo_page();
            else if (selected == 0)
                ret = month_page(-1);
            else if (selected >= 1 && selected <= cal_file_count)
                ret = month_page(selected - 1);
            sb_set_persistent_title(str(LANG_EXTRAS_CALENDARS), Icon_NOICON,
                                    SCREEN_MAIN);
            gui_synclist_draw(&list);
        }
        else if (action == ACTION_STD_MENU || action == ACTION_STD_CANCEL)
            break;
        else if (default_event_handler(action) == SYS_USB_CONNECTED)
            ret = GO_TO_ROOT;
        extras_calendars_pane(&pane);
        if (pane.stamp != stamp)
        {
            stamp = pane.stamp;
            gui_synclist_draw(&list);
        }
    }
    gui_synclist_scroll_stop(&list);
    ui_scene_close_modal(&scene, ret == 0, "calendars-list-pop");
    viewportmanager_theme_undo(SCREEN_MAIN, false);
    if (active_store)
    {
        core_put_data_pinned(active_store);
        core_free(store_h);
        active_store = NULL;
    }
    return ret;
}
