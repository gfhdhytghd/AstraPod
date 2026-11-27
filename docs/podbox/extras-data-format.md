# AstraPod Extras Data Format Contract

This document defines the on-disk storage contract shared between AstraPod firmware and desktop companion tools for **Calendars**, **Reminders (To Do's)**, and **Contacts**.

## 1. Directory Layout

| Domain | Read Paths | New Item Target |
|---|---|---|
| Calendar events (`VEVENT`) | `/Calendars/*.ics` | `/Calendars/AstraPod.ics` |
| Reminders / To Do's (`VTODO`) | `/Calendars/*.ics` | `/Calendars/AstraPod.ics` |
| Contacts (`VCARD`) | `/Contacts/*.vcf` and `/Contacts/*/*.vcf` (1 subdir level) | `/Contacts/AstraPod/<UID>.vcf` |

Rules:
- Filenames starting with `.` (such as macOS resource forks `._*.ics`) and temporary files ending in `.tmp` are ignored during scanning.
- Extension matching (`.ics`, `.vcf`) is ASCII case-insensitive.
- Device-created items create `/Calendars`, `/Contacts`, and `/Contacts/AstraPod` on demand if missing.
- Editing an existing item **always rewrites its source file in place** — items imported from third-party files (e.g., `/Calendars/Work.ics`, `/Contacts/Synced.vcf`) are never relocated to `AstraPod.ics` or `/Contacts/AstraPod/`.

## 2. Atomic Write Protocol

Every create, edit, or component-removal write MUST be power-cut safe:

1. Ensure the target directory exists.
2. Open `<target_path>.tmp` in the **same directory** (`O_WRONLY | O_CREAT | O_TRUNC`).
3. Write the complete file contents.
4. Flush with `fsync(fd)` and `close(fd)`.
5. Atomically replace the target with `rename("<target_path>.tmp", "<target_path>")`.
6. If any step fails before `rename()`, close the descriptor and `remove("<target_path>.tmp")`. The original file remains untouched.

Desktop sync tools SHOULD follow the same temp-file-and-rename protocol when writing to a mounted player and ignore `*.tmp` files.

## 3. UID Generation & Filename Safety

Every device-created `VEVENT`, `VTODO`, and `VCARD` receives a globally unique `UID`:

```text
astrapod-<YYYYMMDD>T<HHMMSS>-<8-hex>@astrapod
```

Example: `astrapod-20261127T143022-0001a4f2@astrapod`.

- Characters are strictly restricted to `[A-Za-z0-9._-@]` so `<UID>.vcf` is always valid on FAT32, Windows, macOS, and POSIX filesystems.
- When editing an imported component that already has a `UID`, that `UID` is preserved verbatim.
- When editing an imported component that lacks a `UID`, the device assigns a new `UID` using the format above.

## 4. Unknown Property & Sibling Component Preservation

On-device edits MUST be lossless with respect to data the player does not edit:

1. **Sibling components and file wrappers:** All bytes before the edited component's `BEGIN:...` line and all bytes after its `END:...` line (including `VCALENDAR` headers, `VTIMEZONE` definitions, other `VEVENT`/`VTODO`/`VCARD` components, and comments) are copied **verbatim byte-for-byte**.
2. **Unknown properties inside an edited component:** Any property line not owned by the player's editor (`X-*`, `ATTENDEE`, `ORGANIZER`, `LOCATION`, `CATEGORIES`, `URL`, `GEO`, `CLASS`, `TRANSP`, `EXDATE`, `RDATE`, `CREATED`, `PHOTO`, `IMPP`, `TITLE`, `ROLE`, `NICKNAME`, `GENDER`, `KIND`, `PRODID`, etc.) is copied **verbatim byte-for-byte**, including its original parameters, folding, and line ending.
3. **Unmodified complex properties:**
   - `RRULE`: preserved verbatim if the user does not change the Repeat field (preserving rules such as `FREQ=WEEKLY;INTERVAL=2;BYDAY=MO,WE,FR`).
   - `DTSTART` / `DTEND` / `DURATION`: preserved verbatim if date, time, and all-day fields are not modified.
   - `PRIORITY`: raw integer (`1..9`) is preserved verbatim if the user does not change Priority.
   - `VALARM`: secondary `VALARM` blocks are always preserved verbatim. The primary `VALARM` block is preserved verbatim if the Alarm field is not modified; if modified to a different offset, only its `TRIGGER` line is updated while any other properties inside that `VALARM` are preserved verbatim.
   - `N` (vCard): additional names, honorific prefixes, and honorific suffixes (components 3–5) are preserved when editing First/Last name.

## 5. Line Endings, Folding & Escaping

- **Reading:** Both `CRLF` (`\r\n`) and bare `LF` (`\n`) are accepted. Continuation lines beginning with a single `SPACE` (`0x20`) or `HTAB` (`0x09`) are unfolded per RFC 5545 §3.1 and RFC 6350 §3.2.
- **Writing (newly emitted or modified properties):**
  - Terminated with `CRLF` (`\r\n`).
  - Folded at 75 octets using `\r\n ` without splitting multi-byte UTF-8 code points.
  - Text escaping: `\` -> `\\`, `;` -> `\;`, `,` -> `\,`, newline -> `\n`.
- **Encoding:** UTF-8 for all written files. On read, vCard 2.1 `CHARSET=ISO-8859-1` / `WINDOWS-1252` and `ENCODING=QUOTED-PRINTABLE` are decoded to UTF-8.

## 6. iCalendar Specification (RFC 5545)

### 6.1 File Wrapper (`/Calendars/AstraPod.ics`)

When creating `/Calendars/AstraPod.ics` for the first time:

```ics
BEGIN:VCALENDAR
VERSION:2.0
PRODID:-//AstraPod//Extras Calendar//EN
CALSCALE:GREGORIAN
...components...
END:VCALENDAR
```

When appending a new `VEVENT` or `VTODO` to an existing `/Calendars/AstraPod.ics`, the new component is inserted immediately before the final `END:VCALENDAR` line.

### 6.2 Time Zones & Timestamps

| Source / Situation | Written Representation |
|---|---|
| All-day event (`DTSTART;VALUE=DATE:...`) | `DTSTART;VALUE=DATE:YYYYMMDD` and exclusive `DTEND;VALUE=DATE:YYYYMMDD` (next day for 1-day event) |
| Existing timed event with `TZID=<id>` | Preserves `;TZID=<id>` on both `DTSTART` and `DTEND`: `DTSTART;TZID=<id>:YYYYMMDDTHHMMSS` |
| Existing timed event in UTC (`Z`) | Preserves UTC `Z` suffix: `DTSTART:YYYYMMDDTHHMMSSZ` |
| Existing floating local timed event | Preserves floating local time: `DTSTART:YYYYMMDDTHHMMSS` |
| Device-created timed event (with TZID) | `DTSTART;TZID=<tzid>:YYYYMMDDTHHMMSS` / `DTEND;TZID=<tzid>:YYYYMMDDTHHMMSS` |
| Device-created timed event (no TZID yet) | Floating local time `DTSTART:YYYYMMDDTHHMMSS` / `DTEND:YYYYMMDDTHHMMSS` |

**Time Zone Hook (`t3/time-zone` integration):**
Firmware resolves the device time zone through `extras_device_tzid()`. Until the parallel `t3/time-zone` branch lands a Time Zone setting, `extras_device_tzid()` returns `NULL` and newly created timed events use RFC 5545 floating local time (`YYYYMMDDTHHMMSS`). Once `t3/time-zone` supplies an IANA TZID (e.g., `America/New_York`), `extras_device_tzid()` returns it and newly created timed events automatically emit `;TZID=<tzid>`.

`DTSTAMP`, `LAST-MODIFIED`, and `COMPLETED` are formatted as `YYYYMMDDTHHMMSSZ` (currently stamped from the device clock; once `t3/time-zone` provides UTC offset, converted to UTC via the same hook).

### 6.3 `VEVENT` Field Mapping

| On-Device Field | RFC 5545 Property | Notes |
|---|---|---|
| Title | `SUMMARY` | Required on create (defaults to `Untitled` if left empty) |
| Date + Start Time | `DTSTART` | `VALUE=DATE` when All Day is On; timed otherwise |
| End Time | `DTEND` | Exclusive next-day `VALUE=DATE` for 1-day all-day events; if `DTEND` is edited on an event that originally used `DURATION`, `DURATION` is replaced by `DTEND` |
| Repeat | `RRULE` | `None` (property omitted), `FREQ=DAILY`, `FREQ=WEEKLY`, `FREQ=MONTHLY`, `FREQ=YEARLY`. Preserved verbatim if Repeat is not edited |
| Alarm offset | `BEGIN:VALARM` ... `END:VALARM` | `ACTION:DISPLAY`, `DESCRIPTION:<summary>`, `TRIGGER:<offset>`: `None` (no VALARM), `At time` (`PT0S`), `5 min` (`-PT5M`), `15 min` (`-PT15M`), `30 min` (`-PT30M`), `1 hour` (`-PT1H`), `1 day` (`-P1D`) |
| Notes | `DESCRIPTION` | Omitted if empty |
| Identifier | `UID` | Preserved or generated |
| Revision counter | `SEQUENCE` | `0` on create; incremented by `+1` on every edit |
| Timestamps | `DTSTAMP`, `LAST-MODIFIED` | Set/updated on every create and edit |

### 6.4 `VTODO` (Reminders / To Do's) Field Mapping

| On-Device Field | RFC 5545 Property | Notes |
|---|---|---|
| Title | `SUMMARY` | Required on create |
| Completed | `STATUS`, `COMPLETED`, `PERCENT-COMPLETE` | Completed: `STATUS:COMPLETED`, `PERCENT-COMPLETE:100`, `COMPLETED:<YYYYMMDDTHHMMSSZ>`. Incomplete: `STATUS:NEEDS-ACTION` (`COMPLETED` and `PERCENT-COMPLETE` removed) |
| Due Date | `DUE` | `DUE;VALUE=DATE:YYYYMMDD` (or preserves source's timed `DUE` format if date is edited on a timed `DUE`? Date-only edits emit `DUE;VALUE=DATE:YYYYMMDD`; untouched timed `DUE` is preserved verbatim). Omitted if None |
| Priority | `PRIORITY` | RFC 5545 `0`–`9`: `0` = None (omitted on create), `1`–`4` = High (written as `1`), `5` = Medium (`5`), `6`–`9` = Low (`9`). Exact integer preserved if Priority is not edited |
| Notes | `DESCRIPTION` | Omitted if empty |
| Identifier | `UID` | Preserved or generated |
| Revision counter | `SEQUENCE` | `0` on create; incremented by `+1` on every edit |
| Timestamps | `DTSTAMP`, `LAST-MODIFIED` | Set/updated on every create and edit |

### 6.5 Deleting a `VEVENT` or `VTODO`

- Removes only the `BEGIN:VEVENT`..`END:VEVENT` (or `BEGIN:VTODO`..`END:VTODO`) byte range from its source `.ics` file.
- The enclosing `BEGIN:VCALENDAR`..`END:VCALENDAR` and all other components remain intact byte-for-byte (even if 0 components remain in the file).

## 7. vCard Specification (3.0 / 4.0)

### 7.1 Supported Versions

- **Read:** vCard 2.1, 3.0 (RFC 2426), and 4.0 (RFC 6350).
- **Create:** vCard `3.0` (`VERSION:3.0`) written to `/Contacts/AstraPod/<UID>.vcf`.
- **Edit:** Preserves `VERSION:3.0` or `VERSION:4.0`. If an imported vCard `2.1` card is edited on-device, it is upgraded to `VERSION:3.0` (UTF-8, backslash-escaped) while preserving unknown properties verbatim.

### 7.2 `VCARD` Field Mapping

| On-Device Field | vCard Property | Notes |
|---|---|---|
| First Name / Last Name | `N` and `FN` | `N:<Last>;<First>;<Middle>;<Prefix>;<Suffix>` (Middle/Prefix/Suffix preserved on edit). `FN` updated to `<First> <Last>` (or `<First>`, `<Last>`, or `<Org>` if name is empty) |
| Organisation | `ORG` | `ORG:<Company>` (additional `;unit` parts preserved if Org is not edited; omitted if cleared) |
| Phone numbers (up to 8) | `TEL;TYPE=<type>:<num>` | Supported types: `CELL` (Mobile), `HOME`, `WORK`, `MAIN`, `FAX`, `PAGER`, `OTHER` (vCard 4.0 uses lowercase `TYPE=cell`, `TYPE=home`, etc. or uppercase; parser is case-insensitive) |
| Email addresses (up to 6) | `EMAIL;TYPE=<type>:<addr>` | Supported types: `HOME`, `WORK`, `OTHER` |
| Addresses (up to 4) | `ADR;TYPE=<type>:;;<Street>;<City>;<Region>;<Postal>;<Country>` | Preserves PO Box & Extended Address (parts 0–1) if present on read |
| Birthday | `BDAY` | `BDAY:YYYY-MM-DD` (vCard 3.0) or `BDAY:YYYYMMDD` (vCard 4.0); parser accepts both `YYYY-MM-DD` and `YYYYMMDD` |
| Note | `NOTE` | Escaped UTF-8 text; omitted if empty |
| Identifier | `UID` | Preserved verbatim on edit; generated on create or if absent |
| Revision timestamp | `REV` | Updated to `YYYYMMDDTHHMMSSZ` on every create and edit |

### 7.3 Deleting a `VCARD`

- In a multi-card `.vcf` file, removes only that `BEGIN:VCARD`..`END:VCARD` byte range and rewrites the file atomically.
- If zero `VCARD` components remain in the file (including single-card files in `/Contacts/AstraPod/<UID>.vcf`), deletes the `.vcf` file with `remove()`.

## 8. Round-Trip Example

Original `/Calendars/Work.ics`:

```ics
BEGIN:VCALENDAR
VERSION:2.0
PRODID:-//External Sync//EN
BEGIN:VEVENT
UID:work-123@example.com
DTSTAMP:20261120T100000Z
SEQUENCE:2
DTSTART;TZID=America/New_York:20261127T090000
DTEND;TZID=America/New_York:20261127T100000
SUMMARY:Design Review
LOCATION:Room 4B
ATTENDEE;CN=Alice:mailto:alice@example.com
X-CUSTOM-TAG:keep-me-exact
END:VEVENT
END:VCALENDAR
```

After editing the title on AstraPod to `Design Review v2`:

```ics
BEGIN:VCALENDAR
VERSION:2.0
PRODID:-//External Sync//EN
BEGIN:VEVENT
UID:work-123@example.com
DTSTAMP:20261127T143022Z
SEQUENCE:3
LAST-MODIFIED:20261127T143022Z
DTSTART;TZID=America/New_York:20261127T090000
DTEND;TZID=America/New_York:20261127T100000
SUMMARY:Design Review v2
LOCATION:Room 4B
ATTENDEE;CN=Alice:mailto:alice@example.com
X-CUSTOM-TAG:keep-me-exact
END:VEVENT
END:VCALENDAR
```

Notice `UID`, `TZID`, `LOCATION`, `ATTENDEE`, `X-CUSTOM-TAG`, and `PRODID` are preserved byte-for-byte while `SEQUENCE` bumped `2 -> 3`, `DTSTAMP` updated, and `LAST-MODIFIED` was added.
