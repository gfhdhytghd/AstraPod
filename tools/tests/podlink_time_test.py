"""Compile the production clock, zone catalog, iCalendar and TS parser."""
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]

PORTS = r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "timefuncs.h"
#include "settings/settings.h"

long current_tick;
static unsigned long received;
static bool pending, rtc_fail;
struct clock_settings global_settings;
struct clock_status global_status;
static struct tm rtc;
static int writes;
static char wire[224], logline[192];

bool podlink_take_time(char *record, unsigned long *tick)
{
    if (!pending)
        return false;
    memcpy(record, wire, sizeof(wire));
    *tick = received;
    pending = false;
    return true;
}

int rtc_read_datetime(struct tm *tm) { *tm = rtc; return 0; }
int rtc_write_datetime(const struct tm *tm)
{
    if (rtc_fail)
        return -1;
    rtc = *tm;
    writes++;
    return 0;
}

int settings_save(void) { return 0; }
int read_line(int fd, char *line, int size)
{
    (void) fd; (void) line; (void) size;
    return 0;
}

void remote_log_event(unsigned k, unsigned long tick, unsigned long a,
                      unsigned long b, const void *data, unsigned len)
{
    assert(k == 11);
    (void)tick; (void)a; (void)b;
    assert(len <= sizeof(logline));
    memcpy(logline, data, len);
}
'''

TEST = r'''
#include <stdio.h>
#include "system/podlink_time.h"
#include "system/time_zone.h"
#include "screens/extras/calendar_ics.h"

static void receive(const char *text)
{
    snprintf(wire, sizeof(wire), "%s", text);
    pending = true;
    received = current_tick;
}

static time_t date(int year, int month, int day, int hour, int min, int sec)
{
    struct tm tm = { .tm_year = year - 1900, .tm_mon = month - 1,
                     .tm_mday = day, .tm_hour = hour, .tm_min = min,
                     .tm_sec = sec };
    return clock_civil_seconds(&tm);
}

static void show(time_t utc, int hour, int minute, int dst)
{
    struct tm tm;
    clock_localtime(utc, &tm);
    assert(tm.tm_hour == hour && tm.tm_min == minute && tm.tm_isdst == dst);
}

int main(int argc, char **argv)
{
    if (argc == 2)
    {
        struct podlink_clock value;
        if (!podlink_time_parse(argv[1], &value))
            return 1;
        printf("%ld %d %s %s\n", (long)value.utc, value.offset,
               value.zone ? value.zone : "-", value.rule ? value.rule : "-");
        return 0;
    }

    time_zone_cfg_default(global_settings.time_zone, "Etc/UTC");
    time_zone_init();
    time_t initial = date(2026, 11, 14, 0, 2, 3);
    assert(set_time_utc(initial) == 0);
    show(initial, 0, 2, 0);
    time_zone_cfg_load(global_settings.time_zone, "America/New_York");
    assert(get_time_utc() == initial && writes == 1);
    show(initial, 19, 2, 0);

    time_t spring = date(2026, 3, 8, 7, 0, 0);
    show(spring - 1, 1, 59, 0);
    show(spring, 3, 0, 1);
    time_t fall = date(2026, 11, 1, 6, 0, 0);
    show(fall - 1, 1, 59, 1);
    show(fall, 1, 0, 0);
    struct tm tm;
    clock_localtime(fall + 30 * 60, &tm);
    assert(time_zone_current_to_utc(&tm) == fall - 30 * 60);
    clock_localtime(spring + 30 * 60, &tm);
    tm.tm_hour = 2;
    assert(time_zone_current_to_utc(&tm) == spring + 30 * 60);

    struct zone_rule posix;
    assert(zone_rule_parse("<+1030>-10:30<+11>-11,M10.1.0,M4.1.0", &posix));
    int dst;
    assert(zone_rule_offset(&posix, date(2026, 1, 15, 0, 0, 0), &dst) == 39600);
    assert(dst);
    assert(zone_rule_offset(&posix, date(2026, 7, 15, 0, 0, 0), &dst) == 37800);
    assert(!dst);
    assert(zone_rule_parse("STD0DST,J60/0,300/24", &posix));
    assert(!zone_rule_parse("STD0DST,M3.5.9,M11.1.0", &posix));
    assert(!zone_rule_parse("STD0DST", &posix));

    struct calendar_stamp event;
    assert(calendar_ics_stamp("", "20261114T000203Z", &event));
    assert(event.absolute && event.seconds == initial);
    assert(calendar_ics_stamp(";TZID=America/New_York", "20261113T190203", &event));
    assert(event.absolute && event.seconds == initial);
    assert(calendar_ics_stamp(";TZID=America/New_York", "20261101T013000", &event));
    assert(event.seconds == fall - 30 * 60);
    assert(calendar_ics_stamp(";VALUE=DATE", "20261114", &event));
    assert(!event.absolute && event.all_day);
    assert(!calendar_ics_stamp(";TZID=Missing", "20261113T190203", &event));
    assert(!calendar_ics_stamp("", "20260231T190203", &event));

    const char *legacy = "TS\t1\t2026-11-13T19:02:03\t-18000";
    const char *extended = "TS\t1\t2026-11-13T19:02:03\t-18000"
        "\t2026-11-14T00:02:03Z\tAmerica/New_York\tEST5EDT,M3.2.0,M11.1.0";
    const char *bad[] = {
        "",
        "TS\t2\t2026-11-13T19:02:03\t+0",
        "TS\t1\t2026-02-29T19:02:03\t+0",
        "TS\t1\t2000-02-30T00:00:00\t+0",
        "TS\t1\t2038-01-01T00:00:00\t+0",
        "TS\t1\t1999-01-01T00:00:00\t+0",
        "TS\t1\t2026-11-13T24:02:03\t+0",
        "TS\t1\t2026-11-13T19:60:03\t+0",
        "TS\t1\t2026-11-13T19:02:60\t+0",
        "TS\t1\t2026-11-13T19:02:03\t+50401",
        "TS\t1\t2026-11-13T19:02:03\t-50401",
        "TS\t1\t2026-11-13T19:02:03\t0",
        "TS\t1\t2026-11-13T19:02:03\t+0\n",
        "TS\t1\t2026-11-13T19:02:03\t+000000",
        "TS\t1\t2026-11-13T19:02:03\t+0\tjunk",
        "TS\t1\t2026-11-13T19:02:03\t+0\t2026-11-14T00:02:03Z\tEtc/UTC\tUTC0",
        "TS\t1\t2026-11-13T19:02:03\t+0\t2026-11-13T19:02:03Z\tEtc/UTC\tbad",
    };
    global_settings.sync_time_from_computer = true;
    global_settings.follow_computer_zone = true;
    for (unsigned i = 0; i < sizeof(bad)/sizeof(*bad); i++)
    {
        receive(bad[i]);
        podlink_time_sync();
        assert(strstr(logline, "ignored-invalid"));
    }
    for (int drift = -3; drift <= 3; drift++)
    {
        assert(set_time_utc(initial + drift) == 0);
        int before = writes;
        receive(legacy);
        podlink_time_sync();
        assert(writes == before + (abs(drift) > 2));
        assert(strstr(logline, abs(drift) > 2 ? "applied" : "ignored-drift"));
    }

    time_zone_cfg_load(global_settings.time_zone, "Asia/Kathmandu");
    global_settings.follow_computer_zone = false;
    receive(extended);
    podlink_time_sync();
    assert(!strcmp(global_settings.time_zone, "Asia/Kathmandu"));
    global_settings.follow_computer_zone = true;
    receive(extended);
    podlink_time_sync();
    assert(!strcmp(global_settings.time_zone, "America/New_York"));
    assert(get_time_utc() == initial);

    time_zone_cfg_load(global_settings.time_zone, "Asia/Kathmandu");
    receive(legacy);
    podlink_time_sync();
    assert(strstr(global_settings.time_zone, "Legacy|"));
    show(initial, 19, 2, 0);
    assert(get_time_utc() == initial);

    global_settings.sync_time_from_computer = false;
    receive(extended);
    podlink_time_sync();
    assert(strstr(logline, "ignored-off"));
    global_settings.sync_time_from_computer = true;
    receive(extended);
    current_tick += 501;
    podlink_time_sync();
    assert(strstr(logline, "ignored-stale"));
    receive(extended);
    current_tick += 300;
    rtc.tm_year = 0;
    global_status.debug_log_since = 123;
    podlink_time_sync();
    assert(get_time_utc() == initial + 3);
    assert(global_status.debug_log_since == initial + 3);
    rtc_fail = true;
    assert(get_time_utc() == initial + 3);
    receive(extended);
    podlink_time_sync();
    assert(strstr(logline, "ignored-rtc-error"));

    puts("UTC RTC, IANA/POSIX DST, ICS dates, TS legacy/extended/drift/off/stale/age PASS");
    return 0;
}
'''

HEADERS = {
    "config.h": "#define CONFIG_RTC 1\n#define ROCKBOX_DIR \"/unused\"\n",
    "kernel.h": "#include <stdbool.h>\n#define HZ 100\nextern long current_tick;\n"
                "#define TIME_AFTER(a,b) ((a) > (b))\n",
    "rtc.h": "#include <time.h>\nint rtc_read_datetime(struct tm *);\nint rtc_write_datetime(const struct tm *);\n",
    "debug.h": "",
    "file.h": "#include <fcntl.h>\n#include <unistd.h>\n",
    "system.h": "#define disable_irq_save() 0\n#define restore_irq(i) (void)(i)\n",
    "podlink.h": "#define PODLINK_TIME_RECORD 224\n"
                 "bool podlink_take_time(char *, unsigned long *);\n",
    "remote_log.h": "#define REMOTE_LOG_TIME 11\n#define REMOTE_LOG_DATA 192\n"
                    "void remote_log_event(unsigned, unsigned long, unsigned long, "
                    "unsigned long, const void *, unsigned);\n",
    "settings/settings.h": "#include <stdbool.h>\n#define CLOCK_SETTING_SIZE 160\n"
                           "extern struct clock_settings { char time_zone[160]; "
                           "bool sync_time_from_computer, follow_computer_zone; } global_settings;\n"
                           "extern struct clock_status { int debug_log_since; } global_status;\n"
                           "int settings_save(void);\n",
}

SOURCES = (
    "firmware/common/timefuncs.c", "firmware/common/clock_utc.c",
    "apps-ipod/system/time_zone.c", "apps-ipod/system/time_zone_catalog.c",
    "apps-ipod/system/time_zone_rule.c", "apps-ipod/system/podlink_time.c",
    "apps-ipod/screens/extras/calendar_ics.c",
)


def build(out):
    out.mkdir(parents=True, exist_ok=True)
    for name, contents in HEADERS.items():
        target = out / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(contents)
    (out / "timefuncs.h").write_bytes(
        (ROOT / "firmware/include/timefuncs.h").read_bytes())
    harness = out / "clock-test.c"
    harness.write_text(PORTS + TEST)
    exe = out / "clock"
    subprocess.run([
        os.environ.get("CC", "cc"), "-std=gnu11", "-D_GNU_SOURCE", "-Wall", "-Wextra",
        "-Werror", "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
        "-I", str(out),
        "-I", str(ROOT / "apps-ipod"), str(harness),
        *(str(ROOT / source) for source in SOURCES), "-o", str(exe),
    ], check=True)
    return exe


if __name__ == "__main__":
    exe = build(Path(sys.argv[1]).resolve())
    subprocess.run([str(exe)], check=True)
