#!/usr/bin/env python3
"""Fruit Company's Extras > Contacts, Sort By and Time in Title, in SDL.

Arguments: ROCKBOXUI SIMDISK OUTPUT_DIRECTORY
SIMDISK is a simulator root with .rockbox in it, as build-sim.sh leaves.
Xvfb is private to this process. /Contacts holds the vCards stock was
checked with: one card a file, a vCard 2.1 card in quoted-printable UTF-8
with a soft line break, and one file of four cards, one of them a company
with no name. The list must read, by OCR, in stock's order by first name;
John Appleseed's page must show his numbers, email and address; Settings >
Sort By must step to Last and the list then read "Last, First" in order of
last name. Settings > Date & Time > Time in Title on must put the clock
(--screenshot holds it at 9:00 AM) in the bar once a screen has been open
for a few seconds, and the screen's title just after it opens.
Writes the frames and result.json to the output directory.
"""

import argparse
import json
import os
import re
import subprocess
import tempfile
import time
from pathlib import Path

from PIL import Image, ImageOps

from ui_menu_sim import FPS, Screen, process
from fruit_tree_sim import press, settle

CARDS = {
    "john.vcf":
        "BEGIN:VCARD\r\nVERSION:3.0\r\nN:Appleseed;John;;;\r\n"
        "FN:John Appleseed\r\nORG:Apple Inc.;\r\n"
        "TEL;type=CELL;type=pref:+1 408 555 0100\r\n"
        "TEL;type=WORK:+1 408 555 0101\r\n"
        "EMAIL;type=INTERNET;type=WORK:john@example.com\r\n"
        "ADR;type=WORK:;;1 Infinite Loop;Cupertino;CA;95014;"
        "United States\r\nEND:VCARD\r\n",
    "zoe.vcf":
        "BEGIN:VCARD\r\nVERSION:2.1\r\n"
        "N;CHARSET=UTF-8;ENCODING=QUOTED-PRINTABLE:Br=C3=BCnner;Zo=C3=AB;;;"
        "\r\nFN;CHARSET=UTF-8;ENCODING=QUOTED-PRINTABLE:Zo=C3=AB Br=C3=BCnner"
        "\r\nTEL;HOME:020 7946 0000\r\n"
        "ADR;HOME;ENCODING=QUOTED-PRINTABLE:;;12 High Street=0D=0AFlat 3;"
        "London;;N1 9GU;=\r\nUK\r\nEND:VCARD\r\n",
    "all.vcf":
        "BEGIN:VCARD\r\nVERSION:3.0\r\nN:Carter;Bob;;;\r\nFN:Bob Carter\r\n"
        "TEL;TYPE=HOME:555-0102\r\nEMAIL:bob@example.org\r\n"
        "NOTE:A long note that is fold\r\n ed across two lines\r\n"
        "END:VCARD\r\n"
        "BEGIN:VCARD\r\nVERSION:3.0\r\nN:Adams;Charlie;;;\r\n"
        "FN:Charlie Adams\r\nEMAIL;TYPE=HOME:charlie@example.net\r\n"
        "END:VCARD\r\n"
        "BEGIN:VCARD\r\nVERSION:3.0\r\nN:Zimmer;Anna;;;\r\nFN:Anna Zimmer\r\n"
        "TEL;TYPE=WORK:555-0199\r\nEND:VCARD\r\n"
        "BEGIN:VCARD\r\nVERSION:3.0\r\nN:;;;;\r\nFN:Acme Corporation\r\n"
        "ORG:Acme Corporation\r\nTEL;TYPE=WORK:555-0123\r\nEND:VCARD\r\n",
}

# What stock 1.1.2 lists for the cards above, top to bottom (its own
# Zoë reads as Latin-1 mojibake; AstraPod reads the UTF-8)
BY_FIRST = ["Acme Corporation", "Anna Zimmer", "Bob Carter",
            "Charlie Adams", "John Appleseed", "Zoë Brünner"]
BY_LAST = ["Acme Corporation", "Adams, Charlie", "Appleseed, John",
           "Brünner, Zoë", "Carter, Bob", "Zimmer, Anna"]
JOHN_PAGE = ["John Appleseed", "Mobile:", "+1 408 555 0100", "Work:",
             "+1 408 555 0101", "john@example.com", "1 Infinite Loop",
             "Cupertino, CA 95014", "United States"]

ROW_H = 44
LIST_TOP = 20
CLOCK = re.compile(r"\b9:00 ?AM\b")


def ocr(img, scale=4):
    """Text in `img`, dark on light or light on dark"""
    grey = ImageOps.grayscale(img)
    if sum(grey.histogram()[i] * i for i in range(256)) < \
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


def loose(text):
    """For comparing OCR: no case, spaces or accents"""
    table = str.maketrans("ëéüÜËÉ", "eeuUEE")
    return re.sub(r"[^a-z0-9,:@.+-]", "", text.translate(table).lower())


def list_names(frame, rows):
    """The name line of each row on screen"""
    names = []
    for i in range(rows):
        top = LIST_TOP + i * ROW_H
        names.append(ocr(frame.crop((0, top + 2, 300, top + 23))).strip())
    return names


def bar_text(frame):
    """The bar's left part: beside a menu's pane it spans the rows only"""
    return ocr(frame.crop((0, 0, 125, 19))).strip()


def open_contacts(s):
    """From the main menu, Extras > Contacts"""
    for _ in range(4):
        press(s, "Down")
    press(s, "Return")
    for _ in range(2):
        press(s, "Down")
    s.tap("Return")
    time.sleep(1.5)
    return s.frame()


def leave_contacts(s):
    s.tap("Escape")
    time.sleep(1)
    for _ in range(2):
        press(s, "Up")
    press(s, "Escape")
    for _ in range(4):
        press(s, "Up")


def check_list(frame, expected):
    seen = list_names(frame, 5)
    ok = all(loose(a) == loose(b) for a, b in zip(seen, expected))
    return {"seen": seen, "expected": expected[:5], "ok": ok}


def run(s, out, result):
    settle(s)
    # 1. The list by first name, and John's page
    frame = open_contacts(s)
    frame.save(out / "contacts-by-first.png")
    result["list_by_first"] = check_list(frame, BY_FIRST)
    for _ in range(4):
        press(s, "Down")
    frame = s.frame()
    frame.save(out / "contacts-john-row.png")
    s.tap("Return")
    time.sleep(1.5)
    page = s.frame()
    page.save(out / "contacts-john-page.png")
    text = loose(ocr(page.crop((0, 20, 320, 240))))
    missing = [t for t in JOHN_PAGE if loose(t) not in text]
    result["john_page"] = {"missing": missing, "ok": not missing}
    s.tap("Escape")
    time.sleep(1)
    s.tap("Down")
    time.sleep(1)
    last = s.frame()
    last.save(out / "contacts-zoe-row.png")
    names = list_names(last, 5)
    result["list_scrolled"] = {"last_row": names[-1],
                               "ok": loose(names[-1]) == loose(BY_FIRST[5])}
    for _ in range(5):
        s.tap("Up")
        time.sleep(0.3)
    time.sleep(1)
    leave_contacts(s)

    # 2. Settings > Sort By steps to Last
    for _ in range(5):
        press(s, "Down")
    press(s, "Return")
    for _ in range(13):
        press(s, "Down")
    frame = press(s, "Return")
    frame.save(out / "sort-by-last.png")
    top = next(y for y in range(20, 240) if frame.getpixel((2, y))[2] > 150
               and frame.getpixel((2, y))[0] < 80)
    row = ocr(frame.crop((0, top, 160, top + 24)))
    result["sort_by_row"] = {
        "row": row.strip(),
        "ok": bool(re.search(r"sort\s*by\s*last", row.lower()))}
    for _ in range(13):
        press(s, "Up")
    press(s, "Escape")
    for _ in range(5):
        press(s, "Up")
    frame = open_contacts(s)
    frame.save(out / "contacts-by-last.png")
    result["list_by_last"] = check_list(frame, BY_LAST)
    leave_contacts(s)

    # 3. Time in Title
    for _ in range(5):
        press(s, "Down")
    press(s, "Return")
    for _ in range(12):
        press(s, "Down")
    press(s, "Return")
    for _ in range(4):
        press(s, "Down")
    frame = press(s, "Return")
    frame.save(out / "time-in-title-on.png")
    time.sleep(4.5)
    frame = s.frame()
    frame.save(out / "time-in-title-settled.png")
    bar = bar_text(frame)
    result["time_in_title_bar"] = {"bar": bar, "ok": bool(CLOCK.search(bar))}
    # Back in Settings: its title first, then the clock
    s.tap("Escape")
    time.sleep(0.8)
    early = s.frame()
    early.save(out / "time-in-title-pop-early.png")
    time.sleep(4.5)
    late = s.frame()
    late.save(out / "time-in-title-pop-late.png")
    result["time_in_title_pop"] = {
        "early": bar_text(early), "late": bar_text(late),
        "ok": "settings" in bar_text(early).lower() and
              bool(CLOCK.search(bar_text(late)))}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("binary", "simdisk", "out"):
        parser.add_argument(name, type=lambda p: Path(p).resolve())
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    result = {}
    with tempfile.TemporaryDirectory(prefix="fruit-contacts-") as temp, \
            tempfile.TemporaryFile() as display, \
            (args.out / "xvfb.log").open("w") as log:
        root = Path(temp) / "root"
        rockbox = root / ".rockbox"
        rockbox.mkdir(parents=True)
        for entry in (args.simdisk / ".rockbox").iterdir():
            if entry.name != "config.cfg":
                (rockbox / entry.name).symlink_to(entry)
        (rockbox / "config.cfg").write_text(
            (args.simdisk / ".rockbox/default-config.cfg").read_text() +
            "\nnative theme: fruit-company\nstart in screen: root\n"
            "backlight timeout: off\nvoice menus: off\nanimations: off\n")
        (root / "Contacts").mkdir()
        for name, card in CARDS.items():
            (root / "Contacts" / name).write_bytes(card.encode())
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
                raise RuntimeError("Xvfb did not start")
            env = dict(os.environ, DISPLAY=":" + number,
                       XDG_RUNTIME_DIR=str(runtime), SDL_VIDEODRIVER="x11",
                       SDL_AUDIODRIVER="dummy", SDL_RENDER_DRIVER="software")
            with (args.out / "sim.log").open("w") as slog, process(
                    [str(args.binary), "--root", str(root), "--nobackground",
                     "--zoom", "1", "--screenshot"],
                    slog, env=env, cwd=root) as firmware:
                time.sleep(150 / FPS)
                s = Screen(env, firmware)
                time.sleep(5)
                try:
                    run(s, args.out, result)
                finally:
                    (args.out / "result.json").write_text(
                        json.dumps(result, indent=1, ensure_ascii=False))
                assert firmware.poll() is None, "simulator exited"
    (args.out / "result.json").write_text(
        json.dumps(result, indent=1, ensure_ascii=False))
    failed = [k for k, v in result.items() if not v["ok"]]
    for key in failed:
        print("FAIL", key, result[key])
    print(f"{len(result) - len(failed)} of {len(result)} checks pass")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
