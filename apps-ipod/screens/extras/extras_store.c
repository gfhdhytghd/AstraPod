/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Shared storage primitives for Extras (Calendars, To Do's, Contacts).
 ****************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "extras_store.h"

#ifdef EXTRAS_HOST_TEST
#include <errno.h>
#include <fcntl.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static size_t host_strlcpy(char *dst, const char *src, size_t size)
{
    size_t len = strlen(src);
    if (size)
    {
        size_t copy = len < size - 1 ? len : size - 1;
        memcpy(dst, src, copy);
        dst[copy] = '\0';
    }
    return len;
}
#define strlcpy host_strlcpy
#else
#include "config.h"
#include "system.h"
#include "string-extra.h"
#include "kernel.h"
#include "dir.h"
#include "file.h"
#include "timefuncs.h"
#if CONFIG_RTC && __has_include("system/time_zone.h")
#include "settings/settings.h"
#include "system/time_zone.h"
#define EXTRAS_HAS_TIME_ZONE
#endif
#endif

/* ---- Date arithmetic ---- */

bool extras_leap_year(int year)
{
    return (!(year % 4) && (year % 100)) || !(year % 400);
}

int extras_month_days(int year, int month)
{
    static const unsigned char days[12] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };
    if (month < 1 || month > 12)
        return 30;
    return days[month - 1] + (month == 2 && extras_leap_year(year));
}

int extras_week_day(const struct extras_date *d)
{
    static const unsigned char shift[12] = {
        0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4
    };
    int y = d->year - (d->month < 3);
    int w = (y + y / 4 - y / 100 + y / 400 + shift[d->month - 1] + d->day) % 7;
    return w < 0 ? w + 7 : w;
}

void extras_step_days(struct extras_date *d, int days)
{
    d->day += days;
    while (d->day < 1)
    {
        if (--d->month < 1)
        {
            d->month = 12;
            d->year--;
        }
        d->day += extras_month_days(d->year, d->month);
    }
    while (d->day > extras_month_days(d->year, d->month))
    {
        d->day -= extras_month_days(d->year, d->month);
        if (++d->month > 12)
        {
            d->month = 1;
            d->year++;
        }
    }
}

void extras_step_month(struct extras_date *d, int months)
{
    d->month += months;
    while (d->month < 1)
    {
        d->month += 12;
        d->year--;
    }
    while (d->month > 12)
    {
        d->month -= 12;
        d->year++;
    }
    int maxd = extras_month_days(d->year, d->month);
    if (d->day > maxd)
        d->day = maxd;
}

int extras_date_cmp(const struct extras_date *a, const struct extras_date *b)
{
    if (a->year != b->year)
        return a->year - b->year;
    if (a->month != b->month)
        return a->month - b->month;
    return a->day - b->day;
}

/* Civil date to Rata Die (days since 0000-03-01), Howard Hinnant's algorithm. */
static int civil_to_days(const struct extras_date *d)
{
    int y = d->year - (d->month <= 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (unsigned)(d->month + (d->month > 2 ? -3 : 9)) + 2) / 5
                   + (unsigned)d->day - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int)doe;
}

int extras_date_days_between(const struct extras_date *a,
                             const struct extras_date *b)
{
    return civil_to_days(b) - civil_to_days(a);
}

/* ---- Clock & TZID placeholder hook ---- */

#ifdef EXTRAS_HOST_TEST
static bool test_now_set;
static int test_y, test_m, test_d, test_H, test_M, test_S;
static const char *test_tzid;
static unsigned uid_seq;

void extras_test_set_now(int year, int month, int day,
                         int hour, int min, int sec)
{
    test_now_set = true;
    test_y = year;
    test_m = month;
    test_d = day;
    test_H = hour;
    test_M = min;
    test_S = sec;
}

void extras_test_set_tzid(const char *tzid)
{
    test_tzid = tzid;
}

void extras_test_reset_uid_seq(void)
{
    uid_seq = 0;
}

static void get_now_parts(bool utc, int *y, int *m, int *d,
                          int *H, int *M, int *S)
{
    if (test_now_set)
    {
        *y = test_y; *m = test_m; *d = test_d;
        *H = test_H; *M = test_M; *S = test_S;
        return;
    }
    time_t t = time(NULL);
    struct tm *tm = utc ? gmtime(&t) : localtime(&t);
    *y = tm->tm_year + 1900;
    *m = tm->tm_mon + 1;
    *d = tm->tm_mday;
    *H = tm->tm_hour;
    *M = tm->tm_min;
    *S = tm->tm_sec;
}

const char *extras_device_tzid(void)
{
    return test_tzid;
}
#else
static unsigned uid_seq;

static void get_now_parts(bool utc, int *y, int *m, int *d,
                          int *H, int *M, int *S)
{
    const struct tm *tm;
#ifdef EXTRAS_HAS_TIME_ZONE
    struct tm utc_parts;
    if (utc)
    {
        time_t stamp = get_time_utc();
        tm = gmtime_r(&stamp, &utc_parts);
    }
    else
#else
    (void)utc;
#endif
        tm = get_time();
    *y = tm->tm_year + 1900;
    *m = tm->tm_mon + 1;
    *d = tm->tm_mday;
    *H = tm->tm_hour;
    *M = tm->tm_min;
    *S = tm->tm_sec;
}

/* The time-zone branch stores either an IANA ID or "ID|POSIX rule". */
const char *extras_device_tzid(void)
{
#ifdef EXTRAS_HAS_TIME_ZONE
    static char tzid[TIME_ZONE_NAME_SIZE];
    const char *setting = global_settings.time_zone;
    size_t len = strcspn(setting, "|");
    if (len && len < sizeof(tzid))
    {
        memcpy(tzid, setting, len);
        tzid[len] = '\0';
        return tzid;
    }
#endif
    return NULL;
}
#endif

struct extras_date extras_today(void)
{
    int y, m, d, H, M, S;
    get_now_parts(false, &y, &m, &d, &H, &M, &S);
    return (struct extras_date){ y, m, d };
}

void extras_now_utc_stamp(char *buf, size_t size)
{
    int y, m, d, H, M, S;
    get_now_parts(true, &y, &m, &d, &H, &M, &S);
    snprintf(buf, size, "%04d%02d%02dT%02d%02d%02dZ", y, m, d, H, M, S);
}

void extras_gen_uid(char *buf, size_t size)
{
    int y, m, d, H, M, S;
    get_now_parts(true, &y, &m, &d, &H, &M, &S);
    uid_seq++;
#ifdef EXTRAS_HOST_TEST
    unsigned salt = uid_seq;
#else
    unsigned salt = ((unsigned)current_tick & 0xffffu) | (uid_seq << 16);
#endif
    snprintf(buf, size, "astrapod-%04d%02d%02dT%02d%02d%02d-%08x@astrapod",
             y, m, d, H, M, S, salt);
}

/* ---- Atomic writes & directory helpers ---- */

bool extras_ensure_dir(const char *path)
{
    char tmp[MAX_PATH];
    size_t len = strlcpy(tmp, path, sizeof(tmp));
    if (len == 0 || len >= sizeof(tmp))
        return false;

    for (char *p = tmp + 1; *p; p++)
    {
        if (*p == '/')
        {
            *p = '\0';
#ifdef EXTRAS_HOST_TEST
            mkdir(tmp, 0755);
#else
            mkdir(tmp);
#endif
            *p = '/';
        }
    }
#ifdef EXTRAS_HOST_TEST
    if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
        return false;
    return true;
#else
    if (!dir_exists(tmp))
        mkdir(tmp);
    return dir_exists(tmp);
#endif
}

bool extras_atomic_open(struct extras_atomic *a, const char *target)
{
    memset(a, 0, sizeof(*a));
    a->fd = -1;
    if (strlcpy(a->target, target, sizeof(a->target)) >= sizeof(a->target))
        return false;
    int n = snprintf(a->tmp, sizeof(a->tmp), "%s.tmp", target);
    if (n <= 0 || (size_t)n >= sizeof(a->tmp))
        return false;

    /* Ensure parent directory exists. */
    char parent[MAX_PATH];
    strlcpy(parent, target, sizeof(parent));
    char *slash = strrchr(parent, '/');
    if (slash && slash != parent)
    {
        *slash = '\0';
        if (!extras_ensure_dir(parent))
            return false;
    }

    a->fd = open(a->tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    return a->fd >= 0;
}

bool extras_atomic_commit(struct extras_atomic *a)
{
    if (a->fd < 0)
        return false;
    bool ok = fsync(a->fd) == 0;
    if (close(a->fd) < 0)
        ok = false;
    a->fd = -1;
    if (ok && rename(a->tmp, a->target) == 0)
        return true;
    remove(a->tmp);
    return false;
}

void extras_atomic_abort(struct extras_atomic *a)
{
    if (a->fd >= 0)
    {
        close(a->fd);
        a->fd = -1;
    }
    if (a->tmp[0])
        remove(a->tmp);
}

bool extras_write_all(int fd, const void *buf, size_t len)
{
    const unsigned char *p = buf;
    while (len > 0)
    {
        ssize_t w = write(fd, p, len);
        if (w <= 0)
            return false;
        p += w;
        len -= (size_t)w;
    }
    return true;
}

bool extras_copy_range(int src_fd, int dst_fd, uint32_t start, uint32_t end)
{
    if (end <= start)
        return true;
    if (lseek(src_fd, (off_t)start, SEEK_SET) != (off_t)start)
        return false;
    char buf[512];
    uint32_t rem = end - start;
    while (rem > 0)
    {
        size_t chunk = rem < sizeof(buf) ? rem : sizeof(buf);
        ssize_t r = read(src_fd, buf, chunk);
        if (r <= 0)
            return false;
        if (!extras_write_all(dst_fd, buf, (size_t)r))
            return false;
        rem -= (uint32_t)r;
    }
    return true;
}

bool extras_copy_to_eof(int src_fd, int dst_fd, uint32_t start)
{
    if (lseek(src_fd, (off_t)start, SEEK_SET) != (off_t)start)
        return false;
    char buf[512];
    for (;;)
    {
        ssize_t r = read(src_fd, buf, sizeof(buf));
        if (r < 0)
            return false;
        if (r == 0)
            return true;
        if (!extras_write_all(dst_fd, buf, (size_t)r))
            return false;
    }
}

/* ---- Buffered property reader ---- */

void extras_reader_init(struct extras_reader *r, int fd, uint32_t start_offset)
{
    r->fd = fd;
    r->len = 0;
    r->pos = 0;
    r->offset = start_offset;
    if (fd >= 0)
        lseek(fd, (off_t)start_offset, SEEK_SET);
}

static int rd_peek(struct extras_reader *r)
{
    if (r->pos >= r->len)
    {
        r->len = (int)read(r->fd, r->buf, sizeof(r->buf));
        r->pos = 0;
        if (r->len <= 0)
        {
            r->len = 0;
            return -1;
        }
    }
    return r->buf[r->pos];
}

static int rd_getc(struct extras_reader *r)
{
    int c = rd_peek(r);
    if (c >= 0)
    {
        r->pos++;
        r->offset++;
    }
    return c;
}

static bool has_word_ci(const char *hay, size_t n, const char *needle)
{
    size_t len = strlen(needle);
    for (size_t i = 0; i + len <= n; i++)
        if (!strncasecmp(hay + i, needle, len))
            return true;
    return false;
}

bool extras_read_prop(struct extras_reader *r, char *line, size_t line_size,
                      uint32_t *raw_start, uint32_t *raw_end,
                      bool qp_continue)
{
    size_t n = 0;
    int c;

    if (raw_start)
        *raw_start = r->offset;
    if (rd_peek(r) < 0)
    {
        if (raw_end)
            *raw_end = r->offset;
        return false;
    }
    for (;;)
    {
        while ((c = rd_getc(r)) >= 0 && c != '\n')
        {
            if (c != '\r' && n + 1 < line_size)
                line[n++] = (char)c;
        }
        c = rd_peek(r);
        if (c == ' ' || c == '\t')
        {
            rd_getc(r);
            continue;
        }
        if (qp_continue && c >= 0 && n > 0 && line[n - 1] == '=')
        {
            const char *colon = memchr(line, ':', n);
            if (colon && has_word_ci(line, (size_t)(colon - line),
                                     "QUOTED-PRINTABLE"))
            {
                n--;
                continue;
            }
        }
        break;
    }
    if (line_size > 0)
    {
        line[n] = '\0';
        /* Strip leading UTF-8 BOM on the first line (Windows/Outlook .ics). */
        if (raw_start && *raw_start == 0 && n >= 3 &&
            (unsigned char)line[0] == 0xef &&
            (unsigned char)line[1] == 0xbb &&
            (unsigned char)line[2] == 0xbf)
        {
            memmove(line, line + 3, n - 2);
        }
    }
    if (raw_end)
        *raw_end = r->offset;
    return true;
}

/* ---- Folding & escaping ---- */

bool extras_write_folded(int fd, const char *prop_line)
{
    size_t len = strlen(prop_line);
    size_t pos = 0;
    bool first = true;

    while (pos < len || first)
    {
        size_t limit = first ? 75 : 74;
        if (!first)
        {
            if (!extras_write_all(fd, "\r\n ", 3))
                return false;
        }
        size_t rem = len - pos;
        size_t take = rem <= limit ? rem : limit;
        /* Never split a multi-byte UTF-8 sequence. */
        if (take < rem)
        {
            while (take > 1 &&
                   (((unsigned char)prop_line[pos + take]) & 0xc0) == 0x80)
                take--;
        }
        if (take > 0 && !extras_write_all(fd, prop_line + pos, take))
            return false;
        pos += take;
        first = false;
        if (pos >= len)
            break;
    }
    return extras_write_all(fd, "\r\n", 2);
}

void extras_escape_text(const char *in, char *out, size_t out_size)
{
    size_t w = 0;
    if (out_size == 0)
        return;
    for (const char *r = in; *r && w + 1 < out_size; r++)
    {
        if (*r == '\\' || *r == ';' || *r == ',')
        {
            if (w + 2 >= out_size)
                break;
            out[w++] = '\\';
            out[w++] = *r;
        }
        else if (*r == '\n')
        {
            if (w + 2 >= out_size)
                break;
            out[w++] = '\\';
            out[w++] = 'n';
        }
        else if (*r != '\r')
            out[w++] = *r;
    }
    out[w] = '\0';
}

void extras_unescape_text(char *s)
{
    char *w = s;
    for (; *s; s++)
    {
        if (*s == '\\' && s[1])
        {
            s++;
            *w++ = (*s == 'n' || *s == 'N') ? '\n' : *s;
        }
        else
            *w++ = *s;
    }
    *w = '\0';
}

bool extras_write_text_prop(int fd, const char *prefix, const char *value)
{
    char buf[2 * 512 + 32];
    size_t plen = strlcpy(buf, prefix, sizeof(buf));
    if (plen >= sizeof(buf))
        return false;
    extras_escape_text(value, buf + plen, sizeof(buf) - plen);
    return extras_write_folded(fd, buf);
}

char *extras_param_separator(char *s, char delimiter)
{
    bool quoted = false;
    for (; *s; s++)
    {
        if (*s == '"')
            quoted = !quoted;
        else if (*s == delimiter && !quoted)
            return s;
    }
    return NULL;
}

char *extras_trim(char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
        s++;
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' ||
                       s[len - 1] == '\n' || s[len - 1] == '\r'))
        s[--len] = '\0';
    return s;
}

bool extras_has_ext(const char *name, const char *ext)
{
    size_t nlen = strlen(name);
    size_t elen = strlen(ext);
    if (name[0] == '.' || nlen <= elen)
        return false;
    /* Ignore .tmp files */
    if (nlen > 4 && !strcasecmp(name + nlen - 4, ".tmp"))
        return false;
    return !strcasecmp(name + nlen - elen, ext);
}
