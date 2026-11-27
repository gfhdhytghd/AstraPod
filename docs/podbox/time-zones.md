# Clock and calendar

**Settings > Date & Time > Time Zone** selects a region and city. The player
stores the instant in UTC. The status bar, date picker and alarm schedule
use the selected city's local time. Each world clock uses its own city.
Changing zones keeps the UTC instant and updates the display.

The clock supports 2000–2037. Zone data comes from IANA tzdata 2026f, with
transitions from 1999–2037 and aliases for imported events and HerdRemote.
A computer outside the built-in catalog can supply a POSIX rule or a fixed
offset. Long-term rule changes require updated tzdata or a fresh host sync.

An RTC from earlier firmware contains local fields without an offset. The
first boot reads those fields at UTC offset zero and does not rewrite them.
Sync with a computer or select a city and set its local clock to establish
the correct instant. A settings reset restores the UTC display zone and
leaves the clock's instant intact. Firmware that expects local RTC fields
will read UTC after a downgrade or when booted alongside AstraPod.

When entering an ambiguous local time, the clock uses the first occurrence.
A skipped time moves forward by the size of the gap. Alarms fire once in a
repeated minute and skip nonexistent minutes. Snooze uses elapsed minutes.
New play and scrobble timestamps use UTC. Existing play log timestamps and
database serial numbers keep their stored values. Playback Report groups
timestamps by UTC day. FAT continues to write and display the original
local calendar fields. FAT dates carry no zone, so two writes within a
repeated minute can have the same stored time.

Place a calendar export at `/.rockbox/calendar.ics` and open **Extras >
Calendars**. The calendar shows up to 128 single VEVENTs with a supported
date or date/time. UTC and known IANA TZID events follow the player's zone.
Floating times and all-day dates remain on their original local date.
Exports can include a DTEND. Expand recurring events before exporting.
Custom VTIMEZONE definitions, recurrence rules and DURATION are unsupported.
