#!/usr/bin/env python3
"""Headless SDL simulator round-trip tests for Extras editing (Calendars,
To Do's, Contacts) in all four native themes.

Verifies create/edit/delete flows and byte-for-byte preservation of unknown
RFC 5545 and vCard 3.0/4.0 properties and sibling components.
"""

import argparse
import json
import mmap
import os
import struct
import subprocess
import tempfile
import time
from pathlib import Path

from PIL import Image, ImageChops

from ui_menu_sim import FPS, process

SIZE = (320, 240)
FRAME_BYTES = SIZE[0] * SIZE[1] * 3
HEADER = struct.Struct("=III")
WORD = struct.Struct("=I")
STATE_BYTES = HEADER.size + 8 * (WORD.size + FRAME_BYTES)


class Screen:
    def __init__(self, state, firmware):
        self.state = state
        self.firmware = firmware

    def key(self, edge, key):
        assert self.firmware.poll() is None, "simulator exited early"
        self.firmware.stdin.write(f"{edge} {key}\n")
        self.firmware.stdin.flush()

    def tap(self, key):
        self.key("down", key)
        time.sleep(4 / FPS)
        self.key("up", key)

    def frame(self):
        for _ in range(120):
            seq = HEADER.unpack_from(self.state)[1]
            if seq:
                off = HEADER.size + ((seq - 1) % 8) * (WORD.size + FRAME_BYTES)
                before = WORD.unpack_from(self.state, off)[0]
                pixels = self.state[off + WORD.size:off + WORD.size + FRAME_BYTES]
                if before == seq == WORD.unpack_from(self.state, off)[0]:
                    return Image.frombytes("RGB", SIZE, pixels)
            time.sleep(1 / FPS)
        raise RuntimeError("simulator produced no complete LCD frame")

WORK_ICS = (
    "BEGIN:VCALENDAR\r\n"
    "VERSION:2.0\r\n"
    "PRODID:-//External Sync//EN\r\n"
    "X-WR-CALNAME:Work\r\n"
    "BEGIN:VTIMEZONE\r\n"
    "TZID:America/New_York\r\n"
    "END:VTIMEZONE\r\n"
    "BEGIN:VEVENT\r\n"
    "UID:work-ev-1@example.com\r\n"
    "DTSTAMP:20231220T100000Z\r\n"
    "SEQUENCE:2\r\n"
    "DTSTART;TZID=America/New_York:20240101T090000\r\n"
    "DTEND;TZID=America/New_York:20240101T100000\r\n"
    "SUMMARY:Design Review\r\n"
    "RRULE:FREQ=WEEKLY;INTERVAL=2;BYDAY=MO\r\n"
    "LOCATION:Room 4B\r\n"
    "ATTENDEE;CN=Alice:mailto:alice@example.com\r\n"
    "X-CUSTOM-PRESERVE:byte-exact-value\r\n"
    "BEGIN:VALARM\r\n"
    "ACTION:DISPLAY\r\n"
    "TRIGGER:-PT15M\r\n"
    "X-ALARM-KEEP:yes\r\n"
    "END:VALARM\r\n"
    "END:VEVENT\r\n"
    "BEGIN:VEVENT\r\n"
    "UID:work-ev-2@example.com\r\n"
    "DTSTAMP:20231220T100000Z\r\n"
    "DTSTART:20240102T150000Z\r\n"
    "DTEND:20240102T160000Z\r\n"
    "SUMMARY:Sibling Untouched\r\n"
    "X-SIBLING:untouched\r\n"
    "END:VEVENT\r\n"
    "BEGIN:VTODO\r\n"
    "UID:work-todo-1@example.com\r\n"
    "DTSTAMP:20231220T100000Z\r\n"
    "SEQUENCE:0\r\n"
    "SUMMARY:Ship release\r\n"
    "DUE;VALUE=DATE:20240105\r\n"
    "PRIORITY:2\r\n"
    "STATUS:NEEDS-ACTION\r\n"
    "X-TODO-KEEP:exact-bytes\r\n"
    "END:VTODO\r\n"
    "END:VCALENDAR\r\n"
)

TEAM_VCF = (
    "BEGIN:VCARD\r\n"
    "VERSION:4.0\r\n"
    "UID:urn:uuid:alice-sim\r\n"
    "N:Smith;Alice;Marie;Dr.;Jr.\r\n"
    "FN:Alice Smith\r\n"
    "ORG:Widgets Corp;Engineering\r\n"
    "TEL;TYPE=cell:+1 555 0100\r\n"
    "EMAIL;TYPE=work:alice@widgets.example\r\n"
    "ADR;TYPE=work:;;100 Main St;Boston;MA;02101;USA\r\n"
    "PHOTO;MEDIATYPE=image/jpeg:data:image/jpeg;base64,AAAA\r\n"
    " BBBBCCCC\r\n"
    "X-KEEP-CONTACT:exact-bytes\r\n"
    "REV:20230101T000000Z\r\n"
    "END:VCARD\r\n"
    "BEGIN:VCARD\r\n"
    "VERSION:3.0\r\n"
    "UID:bob-sim\r\n"
    "N:Jones;Bob;;;\r\n"
    "FN:Bob Jones\r\n"
    "TEL;TYPE=HOME:555-0200\r\n"
    "X-BOB-KEEP:untouched\r\n"
    "END:VCARD\r\n"
)

BOB_EXACT = (
    "BEGIN:VCARD\r\n"
    "VERSION:3.0\r\n"
    "UID:bob-sim\r\n"
    "N:Jones;Bob;;;\r\n"
    "FN:Bob Jones\r\n"
    "TEL;TYPE=HOME:555-0200\r\n"
    "X-BOB-KEEP:untouched\r\n"
    "END:VCARD\r\n"
)

SIBLING_EV_EXACT = (
    "BEGIN:VEVENT\r\n"
    "UID:work-ev-2@example.com\r\n"
    "DTSTAMP:20231220T100000Z\r\n"
    "DTSTART:20240102T150000Z\r\n"
    "DTEND:20240102T160000Z\r\n"
    "SUMMARY:Sibling Untouched\r\n"
    "X-SIBLING:untouched\r\n"
    "END:VEVENT\r\n"
)


def same(a, b):
    return not ImageChops.difference(a.crop((0, 20, 320, 240)),
                                     b.crop((0, 20, 320, 240))).getbbox()


def settle(s, max_steps=25):
    last = s.frame()
    for _ in range(max_steps):
        time.sleep(0.1)
        cur = s.frame()
        if same(last, cur):
            return cur
        last = cur
    return last


def tap(s, key, wait=0.35):
    s.tap(key)
    time.sleep(wait)
    return settle(s, 10)


def tap_until_changed(s, key):
    before = settle(s)
    for _ in range(3):
        s.tap(key)
        for _ in range(20):
            time.sleep(0.075)
            if not same(before, s.frame()):
                return settle(s)
    return s.frame()


def hold_select(s, duration=0.85):
    s.key("down", "select")
    time.sleep(duration)
    s.key("up", "select")
    time.sleep(0.45)
    return settle(s, 12)


def run_sim_session(binary, simdisk, theme, setup_fn, run_fn, out_dir, tag):
    with tempfile.TemporaryDirectory(prefix=f"extras-{tag}-") as temp:
        root = Path(temp) / "root"
        rockbox = root / ".rockbox"
        rockbox.mkdir(parents=True)
        for entry in (simdisk / ".rockbox").iterdir():
            if entry.name not in ("config.cfg", "native-theme.log"):
                (rockbox / entry.name).symlink_to(entry)
        (rockbox / "config.cfg").write_text(
            (simdisk / ".rockbox/default-config.cfg").read_text() +
            f"\nnative theme: {theme}\nstart in screen: root\n"
            "backlight timeout: off\nvoice menus: off\nanimations: off\n")
        setup_fn(root)
        state_path = Path(temp) / "lcd.state"
        env = dict(os.environ, SDL_VIDEODRIVER="dummy",
                   SDL_AUDIODRIVER="dummy", SDL_RENDER_DRIVER="software",
                   PODBOX_UI_WALK_STATE=str(state_path))
        with state_path.open("w+b") as memory, \
                (out_dir / f"sim-{tag}.log").open("w") as slog:
            memory.truncate(STATE_BYTES)
            with mmap.mmap(memory.fileno(), STATE_BYTES, access=mmap.ACCESS_READ) as state, \
                    process([str(binary), "--root", str(root), "--nobackground",
                             "--zoom", "1", "--screenshot", "--keys", "-"],
                            slog, env=env, cwd=root, stdin=subprocess.PIPE,
                            text=True) as firmware:
                time.sleep(4.5)
                s = Screen(state, firmware)
                settle(s)
                theme_log = rockbox / "native-theme.log"
                if theme_log.exists():
                    assert "Native theme rejected" not in theme_log.read_text()
                checks = run_fn(s, root, out_dir)
                assert firmware.poll() is None, "simulator exited early"
                return checks


def seed_data(root):
    (root / "Calendars").mkdir(exist_ok=True)
    (root / "Contacts").mkdir(exist_ok=True)
    (root / "Calendars" / "Work.ics").write_bytes(WORK_ICS.encode("utf-8"))
    (root / "Contacts" / "team.vcf").write_bytes(TEAM_VCF.encode("utf-8"))


def read_raw(path):
    return path.read_bytes().decode("utf-8") if path.exists() else ""


def wait_changed(path, before):
    for _ in range(60):
        data = read_raw(path)
        if data != before:
            return data
        time.sleep(0.05)
    return read_raw(path)


def run_roundtrips(s, root, out, fruit):
    checks = {}
    work_ics = root / "Calendars" / "Work.ics"
    ap_ics = root / "Calendars" / "AstraPod.ics"
    team_vcf = root / "Contacts" / "team.vcf"

    # Fruit Company Main Menu: Music(0), Videos(1), Photos(2), Podcasts(3), Extras(4)
    if fruit:
        for _ in range(4):
            tap(s, "down", 0.25)
        tap(s, "select", 0.5)  # Enter Extras
        tap(s, "down", 0.25)   # Calendars
    tap(s, "select", 0.5)  # Enter Calendars: All Calendars(0), To Do's(1), Alarms(2)

    # 1. All Calendars -> Month View (Jan 2024, selected = Jan 1)
    frame = tap(s, "select", 0.6)
    frame.save(out / "month.png")
    if fruit:
        before_transport = frame
        unchanged = {
            key: same(before_transport, tap(s, key))
            for key in ("back", "right", "play")
        }
        checks["transport_stays_on_month"] = {
            "ok": all(unchanged.values()), **unchanged
        }

    # 2. Short SELECT -> Day View (Jan 1, 2024)
    frame = tap(s, "select", 0.6)
    frame.save(out / "day.png")

    # 3. Short SELECT on Design Review -> Event Editor
    frame = tap(s, "select", 0.6)
    frame.save(out / "event-editor.png")

    # Rows: Title(0), Date(1), All Day(2), Start(3), End(4), Repeat(5), Alarm(6), Notes(7), Save(8), Delete(9)
    for _ in range(6):
        tap(s, "down", 0.2)
    tap(s, "select", 0.4)  # Open Alarm choice list (cur=15 min before -> idx 3)
    tap(s, "down", 0.2)    # idx 4 = 30 min before
    tap(s, "select", 0.4)  # Confirm 30 min before
    tap(s, "down", 0.2)    # Notes(7)
    tap(s, "down", 0.2)    # Save(8)
    tap(s, "select", 0.6)  # Save! Back to Day View

    txt = wait_changed(work_ics, WORK_ICS)
    checks["event_edit_roundtrip"] = {
        "ok": (
            "UID:work-ev-1@example.com\r\n" in txt and
            "SEQUENCE:3\r\n" in txt and
            "LAST-MODIFIED:20240101T09" in txt and
            "DTSTART;TZID=America/New_York:20240101T090000\r\n" in txt and
            "RRULE:FREQ=WEEKLY;INTERVAL=2;BYDAY=MO\r\n" in txt and
            "LOCATION:Room 4B\r\n" in txt and
            "ATTENDEE;CN=Alice:mailto:alice@example.com\r\n" in txt and
            "X-CUSTOM-PRESERVE:byte-exact-value\r\n" in txt and
            "TRIGGER:-PT30M\r\nX-ALARM-KEEP:yes\r\n" in txt and
            SIBLING_EV_EXACT in txt
        )
    }

    # Delete the imported event via its context menu.
    hold_select(s)           # Context Menu: New(0), Edit(1), Delete(2)
    tap(s, "down", 0.25)
    tap(s, "down", 0.25)
    tap(s, "select", 0.4).save(out / "event-delete-prompt.png")
    tap(s, "select", 0.6)

    txt2 = wait_changed(work_ics, txt)
    settle(s).save(out / "day-after-delete.png")
    checks["event_delete_single_component"] = {
        "ok": (
            "work-ev-1@example.com" not in txt2 and
            SIBLING_EV_EXACT in txt2 and
            "work-todo-1@example.com" in txt2 and
            "X-WR-CALNAME:Work\r\n" in txt2
        )
    }

    # Open the empty day context menu and create an all-day event.
    frame = hold_select(s)
    frame.save(out / "day-context.png")
    tap(s, "select", 0.5)  # "New Event" (idx 0)
    # Toggle All Day (row 2) -> rows shrink: Title(0), Date(1), All Day(2), Repeat(3), Alarm(4), Notes(5), Save(6)
    tap(s, "down", 0.2)
    tap(s, "down", 0.2)
    tap(s, "select", 0.3)  # All Day -> On
    for _ in range(4):
        tap(s, "down", 0.2)  # Save(6)
    tap(s, "select", 0.6)  # Save new event into AstraPod.ics!

    ap_txt = wait_changed(ap_ics, "")
    checks["event_create_astrapod_ics"] = {
        "ok": (
            "BEGIN:VCALENDAR\r\n" in ap_txt and
            "BEGIN:VEVENT\r\n" in ap_txt and
            "UID:astrapod-" in ap_txt and
            "DTSTART;VALUE=DATE:20240101\r\n" in ap_txt and
            "DTEND;VALUE=DATE:20240102\r\n" in ap_txt
        )
    }

    # 6. Back to Month -> Back to Calendars Menu -> To Do's
    tap(s, "menu", 0.5)  # Day -> Month
    tap(s, "menu", 0.5)  # Month -> Calendars Menu
    tap(s, "down", 0.25)   # To Do's (row 1)
    frame = tap(s, "select", 0.6)
    frame.save(out / "todos.png")

    # Short SELECT toggles "Ship release" completed in place!
    frame = tap(s, "select", 0.6)
    frame.save(out / "todos-done.png")
    done_frame = frame
    txt3 = wait_changed(work_ics, txt2)
    checks["todo_toggle_completed_inplace"] = {
        "ok": (
            "STATUS:COMPLETED\r\n" in txt3 and
            "PERCENT-COMPLETE:100\r\n" in txt3 and
            "COMPLETED:20240101T09" in txt3 and
            "PRIORITY:2\r\n" in txt3 and
            "SEQUENCE:1\r\n" in txt3 and
            "X-TODO-KEEP:exact-bytes\r\n" in txt3
        )
    }

    hold_select(s)
    tap(s, "down", 0.25)
    tap(s, "select", 0.45)  # Edit To Do
    tap(s, "select", 0.65)  # Edit title in the native keyboard
    tap(s, "down", 0.2)     # Enter a character
    if fruit:
        tap(s, "select", 0.2)  # Commit the character and advance the caret
    if fruit:
        hold_select(s)
    else:
        tap(s, "play", 0.3)
    tap(s, "select", 0.9)   # Accept title and wait for the keyboard to close
    tap_until_changed(s, "menu").save(out / "save-prompt.png")
    time.sleep(0.5)         # Let the sheet finish entering before navigation
    tap(s, "down", 0.35)    # Discard
    frame = tap(s, "select", 1.0)
    checks["todo_keyboard_discard"] = {
        "ok": read_raw(work_ics) == txt3 and same(frame, done_frame)
    }

    # Delete the imported reminder and keep the rest of its source intact.
    hold_select(s)
    tap(s, "down", 0.25)
    tap(s, "down", 0.25)
    tap(s, "select", 0.4)
    tap(s, "select", 0.6)
    checks["todo_delete"] = {
        "ok": "work-todo-1@example.com" not in wait_changed(work_ics, txt3)
        and SIBLING_EV_EXACT in read_raw(work_ics)
        and read_raw(ap_ics) == ap_txt
    }

    # Wait for the deletion confirmation to return to the empty To Do list.
    time.sleep(0.7)
    tap_until_changed(s, "select").save(out / "todo-new-form.png")
    # Rows: Title(0), Completed(1), Due Date(2), Priority(3), Notes(4), Save(5)
    for _ in range(3):
        tap_until_changed(s, "down")
    tap_until_changed(s, "select").save(out / "todo-priority.png")
    # Priority list: None(0), Low(1), Medium(2), High(3).
    for _ in range(3):
        tap_until_changed(s, "down")
    tap_until_changed(s, "select")
    tap_until_changed(s, "down")    # Notes(4)
    tap_until_changed(s, "down")    # Save(5)
    settle(s).save(out / "todo-new-before-save.png")
    tap_until_changed(s, "select")

    ap_txt2 = wait_changed(ap_ics, ap_txt)
    checks["todo_create_astrapod_ics"] = {
        "ok": (
            "BEGIN:VTODO\r\n" in ap_txt2 and
            "PRIORITY:1\r\n" in ap_txt2 and
            "STATUS:NEEDS-ACTION\r\n" in ap_txt2
        )
    }

    # 7. Back to Extras/Home -> Contacts
    tap(s, "menu", 0.5)  # To Do's -> Calendars Menu
    tap(s, "menu", 0.5)  # Calendars -> Extras Menu
    tap(s, "down", 0.25)   # Contacts (row 2)
    frame = tap(s, "select", 0.6)
    frame.save(out / "contacts-list.png")

    # Alice Smith is row 0 -> Short SELECT opens Detail Page
    frame = tap(s, "select", 0.6)
    frame.save(out / "contact-detail.png")

    # Short SELECT on Detail Page -> Contact Editor
    frame = tap(s, "select", 0.6)
    frame.save(out / "contact-editor.png")

    # Alice rows: First(0), Last(1), Org(2), Mobile(3), AddPhone(4),
    # WorkEmail(5), AddEmail(6), WorkAddr(7), AddAddr(8), Birthday(9), Notes(10), Save(11), Delete(12)
    for _ in range(9):
        tap(s, "down", 0.18)
    frame = tap(s, "select", 0.5)  # Open Date Picker for Birthday (default 1990-01-01)
    frame.save(out / "date-picker.png")
    tap(s, "select", 0.25)  # Month -> Day
    tap(s, "select", 0.25)  # Day -> Year
    tap(s, "select", 0.45)  # Confirm Birthday 1990-01-01
    tap(s, "down", 0.2)     # Notes(10)
    tap(s, "select", 0.65)  # Open the extended keyboard
    tap(s, "up", 0.3)       # @ is directly before space
    tap(s, "down", 0.3)     # Back to space
    tap(s, "up", 0.3)       # @
    settle(s).save(out / "contact-keyboard.png")
    if fruit:
        hold_select(s)
    else:
        tap(s, "play", 0.4)
    tap(s, "select", 1.0)   # Accept the character
    tap(s, "down", 0.35)    # Save(11)
    tap(s, "select", 0.6)   # Save Alice -> back to Detail

    vtxt = wait_changed(team_vcf, TEAM_VCF)
    checks["contact_edit_roundtrip"] = {
        "ok": (
            "VERSION:4.0\r\n" in vtxt and
            "UID:urn:uuid:alice-sim\r\n" in vtxt and
            "N:Smith;Alice;Marie;Dr.;Jr.\r\n" in vtxt and
            "ORG:Widgets Corp;Engineering\r\n" in vtxt and
            "BDAY:19900101\r\n" in vtxt and
            "NOTE:@\r\n" in vtxt and
            "PHOTO;MEDIATYPE=image/jpeg:data:image/jpeg;base64,AAAA\r\n BBBBCCCC\r\n" in vtxt and
            "X-KEEP-CONTACT:exact-bytes\r\n" in vtxt and
            "REV:20240101T09" in vtxt and
            BOB_EXACT in vtxt
        )
    }

    # Back to Contacts list -> Long-press SELECT -> New Contact -> Save
    tap(s, "menu", 0.5)
    hold_select(s)          # Context: New Contact(0), Edit(1), Delete(2)
    tap(s, "select", 0.5)   # New Contact
    # New Contact rows: First(0), Last(1), Org(2), AddPhone(3), AddEmail(4), AddAddr(5), Bday(6), Notes(7), Save(8)
    for _ in range(8):
        tap(s, "down", 0.18)
    tap(s, "select", 0.6)   # Save -> /Contacts/AstraPod/<UID>.vcf

    ap_vcfs = list((root / "Contacts" / "AstraPod").glob("*.vcf")) if (root / "Contacts" / "AstraPod").exists() else []
    new_vtxt = read_raw(ap_vcfs[0]) if ap_vcfs else ""
    checks["contact_create_astrapod_vcf"] = {
        "ok": (
            len(ap_vcfs) == 1 and
            ap_vcfs[0].name.startswith("astrapod-") and
            "VERSION:3.0\r\n" in new_vtxt and
            "UID:astrapod-" in new_vtxt and
            "REV:20240101T09" in new_vtxt
        )
    }

    # Delete Alice Smith (row 0) from multi-card team.vcf -> Bob survives byte-for-byte
    hold_select(s)          # New(0), Edit(1), Delete(2)
    tap(s, "down", 0.2)
    tap(s, "down", 0.2)
    tap(s, "select", 0.4)   # Delete Contact
    tap(s, "select", 0.6)   # Confirm Delete
    vtxt2 = wait_changed(team_vcf, vtxt)
    checks["contact_delete_multicard"] = {
        "ok": (
            "alice-sim" not in vtxt2 and
            vtxt2 == BOB_EXACT
        )
    }

    return checks


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("binary", "simdisk", "out"):
        parser.add_argument(name, type=lambda p: Path(p).resolve())
    parser.add_argument("--ultra-binary", type=lambda p: Path(p).resolve())
    parser.add_argument("--themes", nargs="+",
                        default=["fruit-company", "fruit-company-ultra",
                                 "classic", "scrim"])
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)

    def seed_native_home(root):
        seed_data(root)
        cfg = root / ".rockbox" / "config.cfg"
        cfg.write_text(cfg.read_text() +
                       "\nroot menu order: calendars, contacts, settings\n")

    result = {}
    for theme in args.themes:
        out = args.out / theme
        out.mkdir(exist_ok=True)
        fruit = theme.startswith("fruit-company")
        checks = run_sim_session(
            (args.ultra_binary if theme == "fruit-company-ultra"
             and args.ultra_binary else args.binary), args.simdisk, theme,
            seed_data if fruit else seed_native_home,
            lambda s, root, out: run_roundtrips(s, root, out, fruit),
            out, theme)
        for name, check in checks.items():
            result[f"{theme}/{name}"] = check
            print("PASS" if check["ok"] else "FAIL", theme, name, check,
                  flush=True)

    (args.out / "result.json").write_text(
        json.dumps(result, indent=2, ensure_ascii=False))
    failed = [k for k, v in result.items() if not v["ok"]]
    print(f"{len(result) - len(failed)} of {len(result)} checks pass")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
