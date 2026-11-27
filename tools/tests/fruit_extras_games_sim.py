#!/usr/bin/env python3
"""Headless simulator verification for P8 Games catalogue & P7 Extras tools
under Fruit Company (stock 2.0.5 parity).

Arguments: ROCKBOXUI SIMDISK OUTPUT_DIRECTORY

Verifies and captures screenshots for:
  1. Games catalogue (55px rows, untinted 55x55 icon tile at x=0..54,
     blue selection bar at x=55..319, "Never Played" / "Played 3 Times"
     play-count subline, corrupt .rock dark #313431 Error page
     "This game cannot be launched.").
  2. Calendars (/Calendars/Home.ics + Work.ics: multi-calendar menu,
     month grid with bottom-right red flag + top-right bell, selectable
     day event list, Event Detail, VTODO "List of To Do's" & To Do Detail,
     Alarms setting persistence).
  3. Contacts (TITLE subtitle under name, URL group, CAR phone, scrollbar).
  4. Alarms (24-hour clock keeps 3 wells H/M/AM-PM with 24h caption,
     Sounds > Playlists lists user .m3u8 playlist).
  5. Notes (<TITLE> extraction, extensionless file, SubDir navigation,
     <BR>/<P>/<A HREF> rendering & hyperlink following).
  6. Screen Lock (wheel moves between Lock and Reset, locked screen ignores
     MENU/Escape, unlocking succeeds).
  7. Stopwatch (multi-lap log has exact lap count with no phantom trailing
     lap, plus persistence to stopwatch.dat).
"""

import argparse
import json
import os
import re
import subprocess
import tempfile
import time
from pathlib import Path

from PIL import Image, ImageChops, ImageOps

from ui_menu_sim import FPS, Screen, process

WORK_ICS = (
    "BEGIN:VCALENDAR\r\n"
    "VERSION:2.0\r\n"
    "X-WR-CALNAME:Work\r\n"
    "BEGIN:VEVENT\r\n"
    "DTSTART:20240101T140000\r\n"
    "DTEND:20240101T153000\r\n"
    "SUMMARY:Design Review\r\n"
    "LOCATION:Room 4B\r\n"
    "ATTENDEE;CN=\"Alice Smith\":mailto:alice@example.com\r\n"
    "DESCRIPTION:Check fruit extras layout\r\n"
    "BEGIN:VALARM\r\n"
    "ACTION:DISPLAY\r\n"
    "TRIGGER:-PT15M\r\n"
    "END:VALARM\r\n"
    "END:VEVENT\r\n"
    "BEGIN:VEVENT\r\n"
    "DTSTART;VALUE=DATE:20240102\r\n"
    "SUMMARY:Sprint Demo\r\n"
    "END:VEVENT\r\n"
    "BEGIN:VTODO\r\n"
    "SUMMARY:Ship Extras\r\n"
    "DUE:20240105T170000\r\n"
    "STATUS:NEEDS-ACTION\r\n"
    "PRIORITY:1\r\n"
    "DESCRIPTION:Verify P7 and P8 screens\r\n"
    "END:VTODO\r\n"
    "BEGIN:VTODO\r\n"
    "SUMMARY:Done Item\r\n"
    "STATUS:COMPLETED\r\n"
    "PRIORITY:9\r\n"
    "END:VTODO\r\n"
    "END:VCALENDAR\r\n"
)

HOME_ICS = (
    "BEGIN:VCALENDAR\r\n"
    "VERSION:2.0\r\n"
    "X-WR-CALNAME:Home\r\n"
    "BEGIN:VEVENT\r\n"
    "DTSTART:20240101T190000\r\n"
    "DTEND:20240101T203000\r\n"
    "SUMMARY:Dinner\r\n"
    "LOCATION:Kitchen\r\n"
    "END:VEVENT\r\n"
    "END:VCALENDAR\r\n"
)

ALICE_VCF = (
    "BEGIN:VCARD\r\n"
    "VERSION:3.0\r\n"
    "N:Smith;Alice;;;\r\n"
    "FN:Alice Smith\r\n"
    "TITLE:Chief Engineer\r\n"
    "ORG:Acme Corp\r\n"
    "TEL;TYPE=CELL:+1 408 555 0100\r\n"
    "TEL;TYPE=CAR:+1 408 555 0109\r\n"
    "EMAIL;TYPE=WORK:alice@acme.example\r\n"
    "URL;TYPE=WORK:https://acme.example\r\n"
    "ADR;TYPE=WORK:;;100 Main St;Cupertino;CA;95014;USA\r\n"
    "NOTE:Line one\\nLine two\\nLine three\\nLine four\\nLine five\\nLine six\r\n"
    "END:VCARD\r\n"
)

NOTES = {
    "links.txt": (
        "<TITLE>Custom Title</TITLE>"
        "Hello<BR>World<P>Visit <A HREF=\"target.txt\">Target Note</A>"
    ),
    "target.txt": (
        "<TITLE>Target Page</TITLE>"
        "You followed the hyperlink!"
    ),
    "NoExt": "Extensionless note body",
    "SubDir/inside.txt": "Note inside a subdirectory",
}


def same_full(a, b):
    return not ImageChops.difference(a, b).getbbox()


def settle_full(s, frames=20):
    prev = s.frame()
    steady = 0
    for _ in range(frames * 3):
        time.sleep(2 / FPS)
        cur = s.frame()
        if same_full(prev, cur):
            steady += 1
            if steady >= 6:
                return cur
        else:
            prev = cur
            steady = 0
    return prev


def press_full(s, key, tries=4):
    before = settle_full(s)
    for _ in range(tries):
        s.tap(key)
        for _ in range(45):
            time.sleep(4 / FPS)
            if not same_full(before, s.frame()):
                return settle_full(s)
    raise RuntimeError(f"{key} changed nothing")


def ocr(img, scale=4, invert=False):
    grey = ImageOps.grayscale(img)
    if invert or sum(grey.histogram()[i] * i for i in range(256)) < \
            128 * grey.width * grey.height:
        grey = ImageOps.invert(grey)
    grey = grey.resize((grey.width * scale, grey.height * scale),
                       Image.LANCZOS)
    path = Path(tempfile.mktemp(suffix=".png"))
    grey.save(path)
    try:
        return subprocess.run(["tesseract", str(path), "-", "--psm", "6"],
                              capture_output=True, text=True).stdout
    finally:
        path.unlink()


def ocr_bar(frame):
    return ocr(frame.crop((0, 0, 220, 20))).strip()


def ocr_row0(frame, right=148):
    return ocr(frame.crop((2, 21, right, 45)), invert=True).strip()


def has_all(text, needles):
    low = text.lower()
    return all(n.lower() in low for n in needles)


def go_extras_row(s, row_idx):
    for _ in range(row_idx):
        press_full(s, "Down")
    return press_full(s, "Return")


def back_to_extras_top(s, row_idx, escapes=1):
    for _ in range(escapes):
        press_full(s, "Escape")
    for _ in range(row_idx):
        press_full(s, "Up")


def run_checks(s, root, out, result):
    # Main menu -> Extras (row 4)
    settle_full(s)
    for _ in range(4):
        press_full(s, "Down")
    extras_home = press_full(s, "Return")
    extras_home.save(out / "00-extras-menu.png")

    # ------------------------------------------------------------------
    # 1. P8 Games catalogue (row 4 in Extras)
    # ------------------------------------------------------------------
    games = go_extras_row(s, 4)
    games.save(out / "01-games-list.png")
    games_txt = ocr(games.crop((58, 20, 300, 240)))
    px_icon_bg = games.getpixel((1, 22))
    px_bar = games.getpixel((100, 45))
    blue_bar_ok = px_bar[2] > 180 and px_bar[0] < 60
    icon_untinted_ok = not (px_icon_bg[2] > 180 and px_icon_bg[0] < 60)
    result["games_list"] = {
        "ocr": games_txt.strip(),
        "blue_bar_ok": blue_bar_ok,
        "icon_untinted_ok": icon_untinted_ok,
        "has_never_played": "never played" in games_txt.lower(),
        "has_played_3_times": "played 3 times" in games_txt.lower(),
        "ok": blue_bar_ok and icon_untinted_ok and
              "never played" in games_txt.lower() and
              "played 3 times" in games_txt.lower(),
    }

    # Row 1 is Amaze (corrupt .rock) -> launching shows stock dark Error page
    games_row1 = press_full(s, "Down")
    games_row1.save(out / "01b-games-row1.png")
    err_page = press_full(s, "Return")
    err_page.save(out / "02-games-error.png")
    err_txt = ocr(err_page)
    bg_rgb = err_page.getpixel((10, 150))
    dark_bg_ok = abs(bg_rgb[0] - 49) <= 10 and abs(bg_rgb[1] - 52) <= 10 and \
                 abs(bg_rgb[2] - 49) <= 10
    result["games_error"] = {
        "ocr": err_txt.strip(),
        "bg_rgb": list(bg_rgb),
        "dark_bg_ok": dark_bg_ok,
        "ok": dark_bg_ok and has_all(err_txt, ["error", "cannot be launched"]),
    }
    press_full(s, "Escape")  # back to Games list
    press_full(s, "Escape")  # back to Extras menu (row 4)
    for _ in range(4):
        press_full(s, "Up")

    # ------------------------------------------------------------------
    # 2. P7 Calendars (row 1 in Extras)
    # ------------------------------------------------------------------
    cal_menu = go_extras_row(s, 1)
    cal_menu.save(out / "03-cal-menu.png")
    cal_menu_txt = ocr_row0(cal_menu, 145) + "\n" + \
                   ocr(cal_menu.crop((0, 44, 160, 240)))
    result["cal_menu"] = {
        "ocr": cal_menu_txt.strip(),
        "ok": has_all(cal_menu_txt, ["all calendars", "home", "work", "to do",
                                     "alarms"]),
    }

    # Open All Calendars -> Month grid (Jan 2024, today = Jan 1)
    cal_month = press_full(s, "Return")
    cal_month.save(out / "04-cal-month.png")
    # Open Jan 1 -> Day list (Design Review, Dinner)
    cal_day = press_full(s, "Return")
    cal_day.save(out / "05-cal-day.png")
    cal_day_txt = ocr_row0(cal_day, 280) + "\n" + \
                  ocr(cal_day.crop((0, 44, 320, 240)))
    result["cal_day_list"] = {
        "ocr": cal_day_txt.strip(),
        "ok": has_all(cal_day_txt, ["design review", "dinner"]),
    }

    # Open Design Review -> Event Detail
    cal_detail = press_full(s, "Return")
    cal_detail.save(out / "06-cal-event-detail.png")
    cal_detail_txt = ocr(cal_detail)
    result["cal_event_detail"] = {
        "ocr": cal_detail_txt.strip(),
        "ok": has_all(cal_detail_txt, ["summary", "design review", "room 4b",
                                       "alice", "check fruit"]),
    }
    press_full(s, "Escape")  # back to day list
    press_full(s, "Escape")  # back to month
    press_full(s, "Escape")  # back to Calendars menu

    # Move to To Do's (row 3: All Calendars, Home, Work, To Do's)
    for _ in range(3):
        press_full(s, "Down")
    todo_list = press_full(s, "Return")
    todo_list.save(out / "07-cal-todo-list.png")
    todo_list_txt = ocr_bar(todo_list) + "\n" + ocr_row0(todo_list, 280) + \
                    "\n" + ocr(todo_list.crop((0, 44, 320, 240)))
    result["cal_todo_list"] = {
        "ocr": todo_list_txt.strip(),
        "ok": has_all(todo_list_txt, ["list of to do", "done", "ship extras"]),
    }
    todo_detail = press_full(s, "Return")
    todo_detail.save(out / "08-cal-todo-detail.png")
    todo_detail_txt = ocr(todo_detail)
    result["cal_todo_detail"] = {
        "ocr": todo_detail_txt.strip(),
        "ok": bool(re.search(r"done [il]tem", todo_detail_txt, re.I)) and
              has_all(todo_detail_txt, ["summary", "completed", "9"]),
    }
    press_full(s, "Escape")  # back to To Do list
    press_full(s, "Escape")  # back to Calendars menu

    # Toggle Alarms row (row 4) and verify calendars.dat persisted
    press_full(s, "Down")
    cal_alarms = press_full(s, "Return")
    cal_alarms.save(out / "09-cal-alarms-toggled.png")
    cal_dat = (root / ".rockbox/calendars.dat").exists()
    result["cal_alarms_persist"] = {"file_exists": cal_dat, "ok": cal_dat}

    back_to_extras_top(s, 1, escapes=1)

    # ------------------------------------------------------------------
    # 3. P7 Contacts (row 2 in Extras)
    # ------------------------------------------------------------------
    go_extras_row(s, 2)
    contacts_detail = press_full(s, "Return")
    contacts_detail.save(out / "10-contacts-detail.png")
    contacts_txt = ocr(contacts_detail)
    result["contacts_detail"] = {
        "ocr": contacts_txt.strip(),
        "ok": has_all(contacts_txt, ["alice smith", "chief engineer",
                                     "car:", "acme.example"]),
    }
    back_to_extras_top(s, 2, escapes=2)

    # ------------------------------------------------------------------
    # 4. P7 Alarms (row 3 in Extras): 3-well 24h time editor + Playlists
    # ------------------------------------------------------------------
    go_extras_row(s, 3)
    press_full(s, "Return")  # Create Alarm -> Alarm editor
    for _ in range(2):
        press_full(s, "Down")
    alarm_time = press_full(s, "Return")
    alarm_time.save(out / "11-alarms-time-24h.png")
    alarm_time_txt = ocr(alarm_time)
    result["alarms_time_24h"] = {
        "ocr": alarm_time_txt.strip(),
        "ok": "09:00" in alarm_time_txt and "am" in alarm_time_txt.lower(),
    }
    press_full(s, "Escape")  # back to alarm editor

    # Sound (row 4) -> Playlists (row 1)
    for _ in range(2):
        press_full(s, "Down")
    press_full(s, "Return")  # Sound
    press_full(s, "Down")    # Playlists
    alarm_pl = press_full(s, "Return")
    alarm_pl.save(out / "12-alarms-playlists.png")
    alarm_pl_txt = ocr_row0(alarm_pl, 145) + "\n" + \
                   ocr(alarm_pl.crop((0, 44, 160, 240)))
    result["alarms_playlists"] = {
        "ocr": alarm_pl_txt.strip(),
        "ok": has_all(alarm_pl_txt, ["none", "morningmix"]),
    }
    back_to_extras_top(s, 3, escapes=4)

    # ------------------------------------------------------------------
    # 5. P7 Notes (row 5 in Extras)
    # ------------------------------------------------------------------
    notes_list = go_extras_row(s, 5)
    notes_list.save(out / "13-notes-list.png")
    notes_list_txt = ocr_row0(notes_list, 280) + "\n" + \
                     ocr(notes_list.crop((0, 44, 320, 240)))
    result["notes_list"] = {
        "ocr": notes_list_txt.strip(),
        "ok": has_all(notes_list_txt, ["custom title", "noext", "subdir"]),
    }
    # Row 0 is Custom Title (links.txt)
    note_links = press_full(s, "Return")
    note_links.save(out / "14-notes-links.png")
    note_links_txt = ocr_bar(note_links) + "\n" + \
                     ocr(note_links.crop((0, 20, 320, 240)))
    # Press Return to follow <A HREF="target.txt">Target Note</A>
    note_target = press_full(s, "Return")
    note_target.save(out / "15-notes-target.png")
    note_target_txt = ocr_bar(note_target) + "\n" + \
                      ocr(note_target.crop((0, 20, 320, 240)))
    result["notes_hyperlink"] = {
        "links_ocr": note_links_txt.strip(),
        "target_ocr": note_target_txt.strip(),
        "ok": has_all(note_links_txt, ["custom title", "hello", "world",
                                       "target note"]) and
              has_all(note_target_txt, ["target page", "hyperlink"]),
    }
    press_full(s, "Escape")  # back to links.txt
    press_full(s, "Escape")  # back to Notes list
    # Move to SubDir (row 3: Custom Title, Instructions, NoExt, SubDir)
    for _ in range(3):
        press_full(s, "Down")
    notes_sub = press_full(s, "Return")
    notes_sub.save(out / "16-notes-subdir.png")
    notes_sub_txt = ocr_bar(notes_sub) + "\n" + ocr_row0(notes_sub, 280)
    result["notes_subdir"] = {
        "ocr": notes_sub_txt.strip(),
        "ok": has_all(notes_sub_txt, ["subdir", "inside"]),
    }
    back_to_extras_top(s, 5, escapes=2)

    # ------------------------------------------------------------------
    # 6. P7 Screen Lock (row 6 in Extras)
    # ------------------------------------------------------------------
    go_extras_row(s, 6)  # New Combination
    for _ in range(4):
        press_full(s, "Return")  # 0000 -> Confirm Combination
    for _ in range(4):
        press_full(s, "Return")  # 0000 -> Lock / Reset choice screen
    sl_choice = settle_full(s)
    sl_choice.save(out / "17-screen-lock-choice.png")
    # Wheel Down moves highlight from Lock to Reset!
    sl_choice_reset = press_full(s, "Down")
    sl_choice_reset.save(out / "18-screen-lock-reset-hl.png")
    wheel_moved = not same_full(sl_choice, sl_choice_reset)
    # Wheel Up back to Lock, press Return to Lock
    press_full(s, "Up")
    sl_locked = press_full(s, "Return")
    sl_locked.save(out / "19-screen-lock-locked.png")
    # Press Escape while locked -> must stay on locked screen!
    s.tap("Escape")
    time.sleep(0.5)
    after_esc = s.frame()
    stayed_locked = same_full(after_esc, sl_locked)
    # Press Return to bring up Enter Combination, enter 0000 to unlock
    press_full(s, "Return")
    for _ in range(4):
        press_full(s, "Return")
    # Unlocking returns GO_TO_ROOT (Main Menu)
    after_unlock = settle_full(s)
    result["screen_lock"] = {
        "wheel_moved_to_reset": wheel_moved,
        "stayed_locked_on_menu": stayed_locked,
        "unlocked_to_root": "astrapod" in ocr_bar(after_unlock).lower(),
        "ok": wheel_moved and stayed_locked and
              "astrapod" in ocr_bar(after_unlock).lower(),
    }

    # ------------------------------------------------------------------
    # 7. P7 Stopwatch (Main Menu -> Extras -> row 7)
    # ------------------------------------------------------------------
    # Main Menu remembers Extras (row 4) or starts at Music (row 0)
    px_row4 = after_unlock.getpixel((100, 130))
    if not (px_row4[2] > 180 and px_row4[0] < 60):
        for _ in range(4):
            press_full(s, "Down")
    press_full(s, "Return")       # Extras menu
    # Extras menu may remember Screen Lock (row 6) or start at Clocks (row 0)
    extras_after = settle_full(s)
    px_ex6 = extras_after.getpixel((100, 180))
    if px_ex6[2] > 180 and px_ex6[0] < 60:
        press_full(s, "Down")     # row 6 -> row 7 (Stopwatch)
        press_full(s, "Return")   # enters Timer dial directly (first run)
    else:
        go_extras_row(s, 7)       # enters Timer dial directly (first run)
    time.sleep(0.25)
    s.tap("space")                # PLAY starts timer
    time.sleep(0.35)
    for _ in range(3):
        s.tap("Return")           # 3 laps
        time.sleep(0.22)
    sw_dial_done = s.frame()
    sw_dial_done.save(out / "20-stopwatch-dial.png")
    press_full(s, "space")        # pause -> Stopwatch menu (row 0: Resume)
    for _ in range(3):
        press_full(s, "Down")     # row 1 New Timer, row 2 Clear Logs, row 3 Current Log
    sw_log = press_full(s, "Return")
    sw_log.save(out / "21-stopwatch-log.png")
    sw_log_txt = ocr(sw_log)
    sw_dat = (root / ".rockbox/stopwatch.dat").exists()
    no_lap4 = "lap 4" not in sw_log_txt.lower()
    result["stopwatch"] = {
        "ocr": sw_log_txt.strip(),
        "file_exists": sw_dat,
        "no_phantom_lap4": no_lap4,
        "ok": has_all(sw_log_txt, ["lap 1", "lap 2", "lap 3"]) and
              no_lap4 and sw_dat,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("binary", "simdisk", "out"):
        parser.add_argument(name, type=lambda p: Path(p).resolve())
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    result = {}

    with tempfile.TemporaryDirectory(prefix="fruit-extras-games-") as temp, \
            tempfile.TemporaryFile() as display, \
            (args.out / "xvfb.log").open("w") as log:
        root = Path(temp) / "root"
        rockbox = root / ".rockbox"
        rockbox.mkdir(parents=True)
        for entry in (args.simdisk / ".rockbox").iterdir():
            if entry.name in ("config.cfg", "rocks"):
                continue
            (rockbox / entry.name).symlink_to(entry)
        rocks_dst = rockbox / "rocks"
        rocks_dst.mkdir()
        for entry in (args.simdisk / ".rockbox/rocks").iterdir():
            if entry.name == "games":
                games_dst = rocks_dst / "games"
                games_dst.mkdir()
                for g in entry.iterdir():
                    if g.name == "amaze.rock":
                        (games_dst / g.name).write_bytes(b"\x00" * 32)
                    else:
                        (games_dst / g.name).symlink_to(g)
            else:
                (rocks_dst / entry.name).symlink_to(entry)

        (rockbox / "games_played.txt").write_text("games/xworld\t3\n")
        (rockbox / "config.cfg").write_text(
            (args.simdisk / ".rockbox/default-config.cfg").read_text() +
            "\nnative theme: fruit-company\nstart in screen: root\n"
            "time format: 24hour\n"
            "backlight timeout: off\nvoice menus: off\nanimations: off\n")

        (root / "Calendars").mkdir()
        (root / "Calendars/Work.ics").write_bytes(WORK_ICS.encode())
        (root / "Calendars/Home.ics").write_bytes(HOME_ICS.encode())

        (root / "Contacts").mkdir()
        (root / "Contacts/alice.vcf").write_bytes(ALICE_VCF.encode())

        (root / "Playlists").mkdir()
        (root / "Playlists/MorningMix.m3u8").write_text("#EXTM3U\n")

        (root / "Notes/SubDir").mkdir(parents=True)
        for rel, body in NOTES.items():
            (root / "Notes" / rel).write_text(body)

        runtime = Path(temp) / "runtime"
        runtime.mkdir(mode=0o700)
        with process(["Xvfb", "-displayfd", str(display.fileno()), "-screen",
                      "0", "800x600x24"], log, pass_fds=(display.fileno(),)):
            for _ in range(FPS * 5):
                display.seek(0)
                number = display.read().decode().strip()
                if number:
                    break
                time.sleep(1 / FPS)
            else:
                raise RuntimeError("Xvfb never wrote its display")

            env = dict(os.environ, DISPLAY=f":{number}",
                       XDG_RUNTIME_DIR=str(runtime),
                       SDL_VIDEODRIVER="x11",
                       SDL_AUDIODRIVER="dummy",
                       SDL_RENDER_DRIVER="software")
            with (args.out / "sim.log").open("w") as simlog, \
                    process([str(args.binary), "--root", str(root),
                             "--nobackground", "--zoom", "1",
                             "--screenshot"], simlog,
                            cwd=root, env=env) as sim:
                time.sleep(150 / FPS)
                s = Screen(env, sim)
                time.sleep(4)
                try:
                    run_checks(s, root, args.out, result)
                finally:
                    (args.out / "result.json").write_text(
                        json.dumps(result, indent=1, ensure_ascii=False))
                assert sim.poll() is None, "simulator exited"

    bad = [k for k, v in result.items() if not v.get("ok")]
    for k, v in result.items():
        status = "PASS" if v.get("ok") else "FAIL"
        print(f"{status:4s} {k}")
    if bad:
        raise SystemExit(f"FAIL: {', '.join(bad)}")
    print(f"all {len(result)} checks passed")


if __name__ == "__main__":
    main()
