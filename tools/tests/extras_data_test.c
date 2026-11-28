/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Host unit tests for Extras RFC 5545 iCalendar (VEVENT/VTODO) and
 * vCard (2.1/3.0/4.0) parser, recurrence matching, and lossless atomic
 * create/edit/delete round-trips.
 ****************************************************************************/
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "extras_store.h"
#include "extras_ical.h"
#include "extras_vcard.h"

static void write_file_str(const char *path, const char *data)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    assert(fd >= 0);
    size_t len = strlen(data);
    assert(write(fd, data, len) == (ssize_t)len);
    close(fd);
}

static char *read_file_str(const char *path)
{
    int fd = open(path, O_RDONLY);
    assert(fd >= 0);
    struct stat st;
    assert(fstat(fd, &st) == 0);
    char *buf = malloc((size_t)st.st_size + 1);
    assert(buf != NULL);
    ssize_t r = read(fd, buf, (size_t)st.st_size);
    assert(r == st.st_size);
    buf[st.st_size] = '\0';
    close(fd);
    return buf;
}

static void test_ical_event_roundtrip_preserves_unknown(const char *dir)
{
    char work_path[MAX_PATH];
    snprintf(work_path, sizeof(work_path), "%s/Work.ics", dir);

    const char *orig =
        "BEGIN:VCALENDAR\r\n"
        "VERSION:2.0\r\n"
        "PRODID:-//External Sync//EN\r\n"
        "X-WR-CALNAME:Work\r\n"
        "BEGIN:VTIMEZONE\r\n"
        "TZID:America/New_York\r\n"
        "END:VTIMEZONE\r\n"
        "BEGIN:VEVENT\r\n"
        "UID:work-123@example.com\r\n"
        "DTSTAMP:20261120T100000Z\r\n"
        "SEQUENCE:2\r\n"
        "DTSTART;TZID=America/New_York:20261127T090000\r\n"
        "DTEND;TZID=America/New_York:20261127T100000\r\n"
        "SUMMARY:Design Review\r\n"
        "RRULE:FREQ=WEEKLY;INTERVAL=2;BYDAY=MO,FR\r\n"
        "LOCATION:Room 4B\r\n"
        "ATTENDEE;CN=Alice:mailto:alice@example.com\r\n"
        "X-CUSTOM-FOLDED:line one of custom\r\n"
        "  preserved property\r\n"
        "BEGIN:VALARM\r\n"
        "ACTION:DISPLAY\r\n"
        "TRIGGER:-PT15M\r\n"
        "X-ALARM-KEEP:yes-keep-me\r\n"
        "END:VALARM\r\n"
        "BEGIN:VALARM\r\n"
        "ACTION:EMAIL\r\n"
        "TRIGGER:-P1D\r\n"
        "SUMMARY:Secondary Alarm\r\n"
        "END:VALARM\r\n"
        "END:VEVENT\r\n"
        "BEGIN:VEVENT\r\n"
        "UID:sibling-456@example.com\r\n"
        "DTSTAMP:20261120T100000Z\r\n"
        "DTSTART:20261128T150000Z\r\n"
        "DTEND:20261128T160000Z\r\n"
        "SUMMARY:Sibling Untouched\r\n"
        "X-SIBLING:exact-bytes\r\n"
        "END:VEVENT\r\n"
        "END:VCALENDAR\r\n";

    write_file_str(work_path, orig);
    extras_test_set_now(2026, 11, 27, 14, 30, 22);

    struct ical_index idx;
    ical_scan(&idx, dir);
    assert(idx.event_count == 2);

    /* Check recurrence with INTERVAL=2;BYDAY=MO,FR */
    struct extras_date d_fri = { 2026, 11, 27 }; /* Friday */
    struct extras_date d_next_fri = { 2026, 12, 4 }; /* +1 week (skipped by INTERVAL=2) */
    struct extras_date d_two_fri = { 2026, 12, 11 }; /* +2 weeks (matches!) */
    assert(ical_event_occurs_on(&idx, &idx.events[0], &d_fri));
    assert(!ical_event_occurs_on(&idx, &idx.events[0], &d_next_fri));
    assert(ical_event_occurs_on(&idx, &idx.events[0], &d_two_fri));

    /* Edit first event: change summary, start time, and primary alarm offset,
     * leaving RRULE untouched so INTERVAL=2;BYDAY=MO,FR survives verbatim. */
    struct ical_event ev;
    assert(ical_load_event(work_path, idx.events[0].comp_start,
                           idx.events[0].comp_end, &ev));
    assert(strcmp(ev.uid, "work-123@example.com") == 0);
    assert(ev.sequence == 2);
    assert(ev.time_mode == ICAL_TIME_TZID);
    assert(strcmp(ev.tzid, "America/New_York") == 0);
    assert(ev.alarm == ICAL_ALARM_15M);

    strcpy(ev.summary, "Design Review, v2; Final");
    ev.start_hour = 10; ev.start_min = 30;
    ev.end_hour = 11; ev.end_min = 30;
    ev.alarm = ICAL_ALARM_30M;
    assert(ical_save_event(dir, &ev));

    char *after = read_file_str(work_path);
    /* Verify byte-for-byte preservation of unknown props, sibling, wrapper,
     * complex RRULE, TZID, secondary VALARM, and custom prop inside VALARM 0. */
    assert(strstr(after, "X-WR-CALNAME:Work\r\n") != NULL);
    assert(strstr(after, "BEGIN:VTIMEZONE\r\nTZID:America/New_York\r\nEND:VTIMEZONE\r\n") != NULL);
    assert(strstr(after, "UID:work-123@example.com\r\n") != NULL);
    assert(strstr(after, "SEQUENCE:3\r\n") != NULL);
    assert(strstr(after, "DTSTAMP:20261127T143022Z\r\n") != NULL);
    assert(strstr(after, "LAST-MODIFIED:20261127T143022Z\r\n") != NULL);
    assert(strstr(after, "DTSTART;TZID=America/New_York:20261127T103000\r\n") != NULL);
    assert(strstr(after, "DTEND;TZID=America/New_York:20261127T113000\r\n") != NULL);
    assert(strstr(after, "SUMMARY:Design Review\\, v2\\; Final\r\n") != NULL);
    assert(strstr(after, "RRULE:FREQ=WEEKLY;INTERVAL=2;BYDAY=MO,FR\r\n") != NULL);
    assert(strstr(after, "LOCATION:Room 4B\r\n") != NULL);
    assert(strstr(after, "ATTENDEE;CN=Alice:mailto:alice@example.com\r\n") != NULL);
    assert(strstr(after, "X-CUSTOM-FOLDED:line one of custom\r\n  preserved property\r\n") != NULL);
    assert(strstr(after, "TRIGGER:-PT30M\r\nX-ALARM-KEEP:yes-keep-me\r\n") != NULL);
    assert(strstr(after, "BEGIN:VALARM\r\nACTION:EMAIL\r\nTRIGGER:-P1D\r\nSUMMARY:Secondary Alarm\r\nEND:VALARM\r\n") != NULL);
    assert(strstr(after, "BEGIN:VEVENT\r\nUID:sibling-456@example.com\r\n"
                         "DTSTAMP:20261120T100000Z\r\nDTSTART:20261128T150000Z\r\n"
                         "DTEND:20261128T160000Z\r\nSUMMARY:Sibling Untouched\r\n"
                         "X-SIBLING:exact-bytes\r\nEND:VEVENT\r\n") != NULL);
    free(after);

    /* Edit second event (UTC Z) and verify Z suffix is preserved. */
    ical_scan(&idx, dir);
    assert(idx.event_count == 2);
    assert(ical_load_event(work_path, idx.events[1].comp_start,
                           idx.events[1].comp_end, &ev));
    assert(ev.time_mode == ICAL_TIME_UTC);
    ev.start_hour = 16; ev.end_hour = 17;
    assert(ical_save_event(dir, &ev));
    after = read_file_str(work_path);
    assert(strstr(after, "DTSTART:20261128T160000Z\r\n") != NULL);
    assert(strstr(after, "DTEND:20261128T170000Z\r\n") != NULL);
    assert(strstr(after, "X-SIBLING:exact-bytes\r\n") != NULL);
    free(after);

    /* Delete ONLY first event from Work.ics; sibling & wrapper stay intact. */
    ical_scan(&idx, dir);
    assert(idx.event_count == 2);
    assert(ical_delete_component(work_path, idx.events[0].comp_start,
                                 idx.events[0].comp_end));
    after = read_file_str(work_path);
    assert(strstr(after, "work-123@example.com") == NULL);
    assert(strstr(after, "sibling-456@example.com") != NULL);
    assert(strstr(after, "X-WR-CALNAME:Work\r\n") != NULL);
    assert(strstr(after, "END:VCALENDAR\r\n") != NULL);
    free(after);
    printf("PASS: test_ical_event_roundtrip_preserves_unknown\n");
}

static void test_ical_create_and_tzid_hook(const char *dir)
{
    extras_test_set_now(2026, 11, 27, 15, 0, 0);
    extras_test_reset_uid_seq();

    /* Default (no TZID set): floating local time. */
    extras_test_set_tzid(NULL);
    struct ical_event ev;
    struct extras_date d = { 2026, 11, 27 };
    ical_event_init_new(&ev, &d);
    strcpy(ev.summary, "Local Event");
    ev.start_hour = 14; ev.start_min = 15;
    ev.end_hour = 15; ev.end_min = 0;
    ev.repeat = ICAL_REPEAT_DAILY;
    ev.alarm = ICAL_ALARM_5M;
    assert(ical_save_event(dir, &ev));

    /* With TZID hook set (simulating t3/time-zone): emits TZID. */
    extras_test_set_tzid("Asia/Tokyo");
    ical_event_init_new(&ev, &d);
    strcpy(ev.summary, "Tokyo Event");
    ev.start_hour = 18; ev.start_min = 0;
    ev.end_hour = 19; ev.end_min = 0;
    assert(ical_save_event(dir, &ev));
    extras_test_set_tzid(NULL);

    char ap_path[MAX_PATH];
    snprintf(ap_path, sizeof(ap_path), "%s/AstraPod.ics", dir);
    char *txt = read_file_str(ap_path);
    assert(strstr(txt, "DTSTART:20261127T141500\r\n") != NULL);
    assert(strstr(txt, "RRULE:FREQ=DAILY\r\n") != NULL);
    assert(strstr(txt, "TRIGGER:-PT5M\r\n") != NULL);
    assert(strstr(txt, "DTSTART;TZID=Asia/Tokyo:20261127T180000\r\n") != NULL);
    assert(strstr(txt, "DTEND;TZID=Asia/Tokyo:20261127T190000\r\n") != NULL);
    free(txt);
    printf("PASS: test_ical_create_and_tzid_hook\n");
}

static void test_ical_vtodo_roundtrip(const char *dir)
{
    char todo_path[MAX_PATH];
    snprintf(todo_path, sizeof(todo_path), "%s/Tasks.ics", dir);

    const char *orig =
        "BEGIN:VCALENDAR\r\n"
        "VERSION:2.0\r\n"
        "BEGIN:VTODO\r\n"
        "UID:todo-1@example.com\r\n"
        "DTSTAMP:20261120T080000Z\r\n"
        "SEQUENCE:0\r\n"
        "SUMMARY:Ship firmware\r\n"
        "DUE;VALUE=DATE:20261130\r\n"
        "PRIORITY:2\r\n"
        "STATUS:NEEDS-ACTION\r\n"
        "X-TASK-META:keep-byte-exact\r\n"
        "END:VTODO\r\n"
        "END:VCALENDAR\r\n";

    write_file_str(todo_path, orig);
    extras_test_set_now(2026, 11, 27, 16, 45, 0);

    struct ical_index idx;
    ical_scan(&idx, dir);
    assert(idx.todo_count == 1);
    assert(idx.todos[0].completed == 0);
    assert(idx.todos[0].priority == 2);

    /* Toggle completed in place: PRIORITY:2 and X-TASK-META must survive verbatim. */
    struct ical_todo td;
    assert(ical_load_todo(todo_path, idx.todos[0].comp_start,
                          idx.todos[0].comp_end, &td));
    td.completed = true;
    assert(ical_save_todo(dir, &td));

    char *after = read_file_str(todo_path);
    assert(strstr(after, "STATUS:COMPLETED\r\n") != NULL);
    assert(strstr(after, "PERCENT-COMPLETE:100\r\n") != NULL);
    assert(strstr(after, "COMPLETED:20261127T164500Z\r\n") != NULL);
    assert(strstr(after, "PRIORITY:2\r\n") != NULL);
    assert(strstr(after, "SEQUENCE:1\r\n") != NULL);
    assert(strstr(after, "X-TASK-META:keep-byte-exact\r\n") != NULL);
    free(after);

    /* Toggle back to incomplete: COMPLETED and PERCENT-COMPLETE removed. */
    ical_scan(&idx, dir);
    assert(idx.todos[0].completed == 1);
    assert(ical_load_todo(todo_path, idx.todos[0].comp_start,
                          idx.todos[0].comp_end, &td));
    td.completed = false;
    assert(ical_save_todo(dir, &td));
    after = read_file_str(todo_path);
    assert(strstr(after, "STATUS:NEEDS-ACTION\r\n") != NULL);
    assert(strstr(after, "COMPLETED:2026") == NULL);
    assert(strstr(after, "PERCENT-COMPLETE:") == NULL);
    assert(strstr(after, "SEQUENCE:2\r\n") != NULL);
    assert(strstr(after, "X-TASK-META:keep-byte-exact\r\n") != NULL);
    free(after);

    /* Create new VTODO -> lands in AstraPod.ics. */
    ical_todo_init_new(&td);
    strcpy(td.summary, "Buy cables");
    td.has_due = true;
    td.due_date = (struct extras_date){ 2026, 12, 1 };
    td.priority = 1;
    assert(ical_save_todo(dir, &td));

    ical_scan(&idx, dir);
    assert(idx.todo_count == 2);
    printf("PASS: test_ical_vtodo_roundtrip\n");
}

static void test_vcard_roundtrip_and_multicard(const char *dir)
{
    char multi_path[MAX_PATH];
    snprintf(multi_path, sizeof(multi_path), "%s/team.vcf", dir);

    const char *orig =
        "BEGIN:VCARD\r\n"
        "VERSION:4.0\r\n"
        "UID:urn:uuid:alice-001\r\n"
        "N:Smith;Alice;Marie;Dr.;Jr.\r\n"
        "FN:Alice Smith\r\n"
        "ORG:Widgets Corp;Engineering\r\n"
        "TEL;TYPE=cell:+1 555 0100\r\n"
        "EMAIL;TYPE=work:alice@widgets.example\r\n"
        "ADR;TYPE=work:;;100 Main St;Boston;MA;02101;USA\r\n"
        "BDAY:19880514\r\n"
        "PHOTO;MEDIATYPE=image/jpeg:data:image/jpeg;base64,AAAA\r\n"
        " BBBBCCCCDDDD\r\n"
        "X-SOCIALPROFILE;type=mastodon:https://example.social/@alice\r\n"
        "REV:20260101T000000Z\r\n"
        "END:VCARD\r\n"
        "BEGIN:VCARD\r\n"
        "VERSION:3.0\r\n"
        "UID:bob-002\r\n"
        "N:Jones;Bob;;;\r\n"
        "FN:Bob Jones\r\n"
        "TEL;TYPE=HOME:555-0200\r\n"
        "X-BOB-KEEP:untouched\r\n"
        "END:VCARD\r\n";

    write_file_str(multi_path, orig);
    extras_test_set_now(2026, 11, 27, 17, 10, 0);

    struct vcard_contact c;
    assert(vcard_load(multi_path, 0, &c));
    assert(c.version == VCARD_VER_40);
    assert(strcmp(c.uid, "urn:uuid:alice-001") == 0);
    assert(strcmp(c.first, "Alice") == 0);
    assert(strcmp(c.last, "Smith") == 0);
    assert(strcmp(c.middle, "Marie") == 0);
    assert(strcmp(c.prefix, "Dr.") == 0);
    assert(strcmp(c.suffix, "Jr.") == 0);
    assert(c.phone_count == 1 && c.phones[0].type == VCARD_TEL_MOBILE);
    assert(c.has_bday && c.bday.year == 1988 && c.bday.month == 5 && c.bday.day == 14);

    /* Edit Alice: add second phone, edit note, leave ORG/BDAY untouched. */
    c.phones[1].type = VCARD_TEL_WORK;
    strcpy(c.phones[1].value, "+1 555 0199");
    c.phone_count = 2;
    strcpy(c.note, "Updated on iPod");
    assert(vcard_save(dir, &c));

    char *after = read_file_str(multi_path);
    assert(strstr(after, "VERSION:4.0\r\n") != NULL);
    assert(strstr(after, "UID:urn:uuid:alice-001\r\n") != NULL);
    assert(strstr(after, "N:Smith;Alice;Marie;Dr.;Jr.\r\n") != NULL);
    assert(strstr(after, "ORG:Widgets Corp;Engineering\r\n") != NULL);
    assert(strstr(after, "TEL;TYPE=cell:+1 555 0100\r\n") != NULL);
    assert(strstr(after, "TEL;TYPE=work;VALUE=text:+1 555 0199\r\n") != NULL);
    assert(strstr(after, "PHOTO;MEDIATYPE=image/jpeg:data:image/jpeg;base64,AAAA\r\n BBBBCCCCDDDD\r\n") != NULL);
    assert(strstr(after, "X-SOCIALPROFILE;type=mastodon:https://example.social/@alice\r\n") != NULL);
    assert(strstr(after, "REV:20261127T171000Z\r\n") != NULL);
    assert(strstr(after, "NOTE:Updated on iPod\r\n") != NULL);
    /* Bob untouched byte-for-byte */
    assert(strstr(after, "BEGIN:VCARD\r\nVERSION:3.0\r\nUID:bob-002\r\nN:Jones;Bob;;;\r\n"
                         "FN:Bob Jones\r\nTEL;TYPE=HOME:555-0200\r\nX-BOB-KEEP:untouched\r\n"
                         "END:VCARD\r\n") != NULL);
    free(after);

    /* Delete ONLY Alice from team.vcf -> Bob remains. */
    assert(vcard_load(multi_path, 0, &c));
    assert(vcard_delete(multi_path, c.card_start, c.card_end));
    after = read_file_str(multi_path);
    assert(strstr(after, "alice-001") == NULL);
    assert(strstr(after, "bob-002") != NULL);
    free(after);

    /* Create new contact -> /Contacts/AstraPod/<UID>.vcf */
    extras_test_reset_uid_seq();
    vcard_init_new(&c);
    strcpy(c.first, "Grace");
    strcpy(c.last, "Hopper");
    strcpy(c.org, "US Navy");
    c.phones[0].type = VCARD_TEL_WORK;
    strcpy(c.phones[0].value, "555-1944");
    c.phone_count = 1;
    c.has_bday = true;
    c.bday = (struct extras_date){ 1906, 12, 9 };
    assert(vcard_save(dir, &c));
    assert(strstr(c.path, "/AstraPod/astrapod-") != NULL);
    assert(vcard_any_in(dir));

    after = read_file_str(c.path);
    assert(strstr(after, "VERSION:3.0\r\n") != NULL);
    assert(strstr(after, "N:Hopper;Grace;;;\r\n") != NULL);
    assert(strstr(after, "FN:Grace Hopper\r\n") != NULL);
    assert(strstr(after, "BDAY:1906-12-09\r\n") != NULL);
    assert(strstr(after, "REV:20261127T171000Z\r\n") != NULL);
    free(after);

    /* Delete single-card file -> file itself is removed. */
    assert(vcard_load(c.path, 0, &c));
    assert(vcard_delete(c.path, c.card_start, c.card_end));
    assert(access(c.path, F_OK) != 0);

    /* vCard 2.1 QP UTF-8 read + edit -> upgraded to 3.0, unknown preserved. */
    char v21_path[MAX_PATH];
    snprintf(v21_path, sizeof(v21_path), "%s/zoe21.vcf", dir);
    write_file_str(v21_path,
        "BEGIN:VCARD\r\n"
        "VERSION:2.1\r\n"
        "N;CHARSET=UTF-8;ENCODING=QUOTED-PRINTABLE:Br=C3=BCnner;Zo=C3=AB;;;\r\n"
        "FN;CHARSET=UTF-8;ENCODING=QUOTED-PRINTABLE:Zo=C3=AB Br=C3=BCnner\r\n"
        "TEL;HOME:020 7946 0000\r\n"
        "X-LEGACY-FIELD:preserve-me\r\n"
        "END:VCARD\r\n");
    assert(vcard_load(v21_path, 0, &c));
    assert(c.version == VCARD_VER_21);
    assert(strcmp(c.first, "Zo\xc3\xab") == 0);
    assert(strcmp(c.last, "Br\xc3\xbcnner") == 0);
    strcpy(c.note, "Long UTF-8 note with \xc3\xa9\xc3\xa9\xc3\xa9 and enough text "
                   "to exceed seventy-five octets so folding is exercised cleanly.");
    assert(vcard_save(dir, &c));
    after = read_file_str(v21_path);
    assert(strstr(after, "VERSION:3.0\r\n") != NULL);
    assert(strstr(after, "X-LEGACY-FIELD:preserve-me\r\n") != NULL);
    assert(strstr(after, "\r\n ") != NULL); /* verified line folding */
    free(after);
    /* Re-load folded UTF-8 note and verify exact round-trip */
    assert(vcard_load(v21_path, 0, &c));
    assert(strstr(c.note, "\xc3\xa9\xc3\xa9\xc3\xa9") != NULL);
    assert(strstr(c.note, "exercised cleanly.") != NULL);

    printf("PASS: test_vcard_roundtrip_and_multicard\n");
}

static void test_recurrence_sets(const char *dir)
{
    /* Provider-style export with two VCALENDAR wrappers, a folded RRULE,
     * a quoted TZID, an excluded occurrence and a separately added period. */
    static const char fixture[] =
        "BEGIN:VCALENDAR\r\nVERSION:2.0\r\n"
        "BEGIN:VTIMEZONE\r\nTZID:Office:East\r\n"
        "BEGIN:STANDARD\r\nDTSTART:19701101T020000\r\n"
        "TZOFFSETFROM:-0400\r\nTZOFFSETTO:-0500\r\n"
        "END:STANDARD\r\nEND:VTIMEZONE\r\n"
        "BEGIN:VEVENT\r\nUID:monthly@example.org\r\n"
        "DTSTART;TZID=\"Office:East\":20240628T090000\r\n"
        "SUMMARY:Production review\r\n"
        "RRULE:FREQ=MONTHLY;COUNT=3;BYDAY=MO,TU,WE,TH,FR;\r\n"
        " BYSETPOS=-1\r\n"
        "EXDATE;TZID=\"Office:East\":20240731T090000\r\n"
        "RDATE;VALUE=PERIOD;TZID=\"Office:East\":20240902T090000/P2D\r\n"
        "END:VEVENT\r\nEND:VCALENDAR\r\n"
        "BEGIN:VCALENDAR\nVERSION:2.0\n"
        "BEGIN:VEVENT\nUID:annual@example.org\n"
        "DTSTART;VALUE=DATE:20240527\n"
        "RRULE:FREQ=YEARLY;BYMONTH=5;BYDAY=-1MO;UNTIL=20250526\n"
        "SUMMARY:Annual\nEND:VEVENT\nEND:VCALENDAR\n";
    char path[MAX_PATH];
    assert(snprintf(path, sizeof(path), "%s/Thunderbird.ICS", dir) <
           (int)sizeof(path));
    write_file_str(path, fixture);

    struct ical_index idx;
    ical_scan(&idx, dir);
    const struct ical_event_idx *monthly = NULL, *annual = NULL;
    for (int i = 0; i < idx.event_count; i++)
    {
        const char *title = ical_pool_str(&idx, idx.events[i].summary_off);
        if (!strcmp(title, "Production review")) monthly = &idx.events[i];
        if (!strcmp(title, "Annual")) annual = &idx.events[i];
    }
    assert(monthly && annual);
    assert(ical_event_occurs_on(&idx, monthly,
                                &(struct extras_date){2024, 6, 28}));
    assert(!ical_event_occurs_on(&idx, monthly,
                                 &(struct extras_date){2024, 7, 31}));
    assert(ical_event_occurs_on(&idx, monthly,
                                &(struct extras_date){2024, 8, 30}));
    assert(ical_event_occurs_on(&idx, monthly,
                                &(struct extras_date){2024, 9, 3}));
    assert(!ical_event_occurs_on(&idx, monthly,
                                 &(struct extras_date){2024, 9, 30}));
    assert(ical_event_occurs_on(&idx, annual,
                                &(struct extras_date){2025, 5, 26}));
    assert(!ical_event_occurs_on(&idx, annual,
                                 &(struct extras_date){2026, 5, 25}));

    struct ical_event ev;
    assert(ical_load_event(path, monthly->comp_start, monthly->comp_end, &ev));
    assert(!strcmp(ev.tzid, "Office:East"));
    strcpy(ev.summary, "Production review updated");
    assert(ical_save_event(dir, &ev));
    char *updated = read_file_str(path);
    assert(!memcmp(updated, fixture, monthly->comp_start));
    assert(!strcmp(strstr(updated, "END:VEVENT\r\n") + strlen("END:VEVENT\r\n"),
                   fixture + monthly->comp_end));
    assert(strstr(updated, "EXDATE;TZID=\"Office:East\":20240731T090000\r\n"));
    assert(strstr(updated, "RRULE:FREQ=MONTHLY;COUNT=3;BYDAY=MO,TU,WE,TH,FR;\r\n"
                           " BYSETPOS=-1\r\n"));
    assert(strstr(updated, "RDATE;VALUE=PERIOD;TZID=\"Office:East\":"
                           "20240902T090000/P2D\r\n"));
    assert(!strstr(updated, "DTEND"));
    free(updated);
    assert(remove(path) == 0);
    puts("PASS: test_recurrence_sets");
}

static void test_vcard_row_preservation(const char *dir)
{
    static const char fixture[] =
        "BEGIN:VCARD\r\nVERSION:4.0\r\nFN:Jules\r\nN:;Jules;;;\r\n"
        "item1.TEL;TYPE=cell;PREF=1:tel:+12025550131;ext=42\r\n"
        "item1.X-ABLabel:Emergency\n"
        "TEL;TYPE=work;VALUE=text:0123\r\n"
        "EMAIL;TYPE=other:mail@example.org\r\n"
        "ADR;LABEL=\"Paris: 75001\";TYPE=home:PO Box;Suite;Street;Paris;;75001;France\r\n"
        "NOTE;LANGUAGE=en:An unchanged note\r\nEND:VCARD\r\n";
    char path[MAX_PATH];
    assert(snprintf(path, sizeof(path), "%s/Grouped.vcf", dir) <
           (int)sizeof(path));
    write_file_str(path, fixture);
    struct vcard_contact c;
    assert(vcard_load(path, 0, &c));
    assert(c.phone_count == 2 && c.email_count == 1 && c.addr_count == 1);
    assert(c.emails[0].type == VCARD_KIND_OTHER);
    assert(!strcmp(c.addrs[0].pobox, "PO Box"));
    c.phones[1].type = VCARD_TEL_HOME;
    assert(vcard_save(dir, &c));
    char *updated = read_file_str(path);
    assert(strstr(updated, "item1.TEL;TYPE=cell;PREF=1:tel:+12025550131;ext=42\r\n"
                           "item1.X-ABLabel:Emergency\n"));
    assert(strstr(updated, "TEL;TYPE=home;VALUE=text:0123\r\n"));
    assert(strstr(updated, "ADR;LABEL=\"Paris: 75001\";TYPE=home:"
                           "PO Box;Suite;Street;Paris;;75001;France\r\n"));
    assert(strstr(updated, "NOTE;LANGUAGE=en:An unchanged note\r\n"));
    free(updated);
    assert(remove(path) == 0);
    puts("PASS: test_vcard_row_preservation");
}

static void test_vcard_windows_charset(const char *dir)
{
    static const char legacy[] =
        "BEGIN:VCARD\r\nVERSION:2.1\r\n"
        "N;CHARSET=WINDOWS-1252;ENCODING=QUOTED-PRINTABLE:D=92Angelo;Ren=E9;;;\r\n"
        "NOTE;CHARSET=WINDOWS-1252;ENCODING=QUOTED-PRINTABLE:Co=\r\n"
        "sts =80 50\r\n"
        "X-WINDOWS-PRESERVE:yes\r\nEND:VCARD\r\n";
    char path[MAX_PATH];
    assert(snprintf(path, sizeof(path), "%s/Outlook.vcf", dir) <
           (int)sizeof(path));
    write_file_str(path, legacy);
    struct vcard_contact c;
    assert(vcard_load(path, 0, &c));
    assert(!strcmp(c.last, "D’Angelo"));
    assert(!strcmp(c.first, "René"));
    assert(!strcmp(c.note, "Costs € 50"));
    assert(vcard_save(dir, &c));
    char *updated = read_file_str(path);
    assert(strstr(updated, "VERSION:3.0\r\n"));
    assert(strstr(updated, "NOTE:Costs € 50\r\n"));
    assert(strstr(updated, "X-WINDOWS-PRESERVE:yes\r\n"));
    free(updated);
    assert(remove(path) == 0);
    puts("PASS: test_vcard_windows_charset");
}

int main(void)
{
    char tmpl[] = "/tmp/extras-data-test-XXXXXX";
    char *root = mkdtemp(tmpl);
    assert(root != NULL);

    char cal_dir[MAX_PATH], con_dir[MAX_PATH];
    snprintf(cal_dir, sizeof(cal_dir), "%s/Calendars", root);
    snprintf(con_dir, sizeof(con_dir), "%s/Contacts", root);
    assert(extras_ensure_dir(cal_dir));
    assert(extras_ensure_dir(con_dir));

    test_ical_event_roundtrip_preserves_unknown(cal_dir);
    test_ical_create_and_tzid_hook(cal_dir);
    test_ical_vtodo_roundtrip(cal_dir);
    test_vcard_roundtrip_and_multicard(con_dir);
    test_recurrence_sets(cal_dir);
    test_vcard_row_preservation(con_dir);
    test_vcard_windows_charset(con_dir);

    printf("ALL EXTRAS DATA TESTS PASSED\n");
    return 0;
}
