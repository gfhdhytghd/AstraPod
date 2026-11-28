#!/usr/bin/env python3
"""Headless simulator tests for navy glass dialogs and Scrim.

Arguments: ROCKBOXUI SIMDISK OUTPUT_DIRECTORY [--themes THEME,THEME,...]

For each theme, opens and screenshots:
  - USB Disk Mode consent (one-press MENU = Charge Only, SELECT = Connect; RIGHT stays open)
  - Yes/No confirmation (pop-up under Classic/Fruit; one-press LEFT/MENU = No,
    RIGHT/SELECT = Yes; PLAY stays open; wheel does not shift selection)
  - Transient dark-glass splash notice (captured while visible, restores cleanly)
  - Prose dialog (Setting -> Context -> Explain; pop-up under Classic/Fruit;
    one-press SELECT/MENU/LEFT/RIGHT dismiss; PLAY stays open)
  - Search dialog (full-screen stock page under Classic/Fruit)

Verifies every dialog opens, answers with one key press per mapped key, and
dismisses cleanly restoring the screen underneath. For navy glass capsules,
measures the visible ink of each action and hardware legend against the button
centre, and compares the fill across buttons in the same dialog.
"""

import argparse
from contextlib import contextmanager
import json
import os
import tempfile
import time
from pathlib import Path
from statistics import median

from PIL import Image, ImageChops, ImageStat

from ui_menu_sim import FPS, Screen, process
from ui_quickscreen_sim import ULTRA_FALLBACK_THEME

STATUS_H = 20
CAPSULE_H = 28
PAIR_H = 38
PAIR_PAD = 8


def differ(a, b, box=None):
    if box:
        a, b = a.crop(box), b.crop(box)
    return bool(ImageChops.difference(a, b).getbbox())


def is_dimmed_popup(before, during):
    """True when the screen underneath stays visible (dimmed) in the outer margin."""
    # Sample top-left status bar area (4..40, 4..16): a full-screen page keeps
    # the status bar bright (~200+ luma) and paints pure white below y=20,
    # whereas a pop-up dims the whole screen (including status bar & edges).
    b_mean = ImageStat.Stat(before.convert("L").crop((4, 4, 40, 16))).mean[0]
    d_mean = ImageStat.Stat(during.convert("L").crop((4, 4, 40, 16))).mean[0]
    # Dimming by ~35% drops brightness noticeably (>15% darker, still >20 luma).
    return d_mean < b_mean * 0.85 and d_mean > 20


def capsule_tops(frame, left, right, height=CAPSULE_H):
    """Find the rim, split gloss and catchlight."""
    first_row = 70
    mid = height // 2
    green = [
        median(frame.getpixel((x, y))[1] for x in range(left + 8, right - 8))
        for y in range(first_row, 239)
    ]
    return [
        y + first_row for y in range(len(green) - height)
        if green[y + 1] - green[y] >= 20
        and green[y + mid - 1] - green[y + mid] >= 8
        and green[y + height - 2] - green[y + height - 1] >= 20
        and green[y + height] - green[y + height - 1] >= 20
    ]


def capsule_pixels(frame, left, top, right, hint_left):
    # The bevel occupies the outer rows. White cores identify the action's
    # horizontal span, so its antialiased ink and the muted legend can be
    # measured separately. The red threshold is above the gloss fill.
    pixels = [
        (x, y, frame.getpixel((x, y))[0])
        for y in range(top + 2, top + CAPSULE_H - 2)
        for x in range(left + 5, right - 5)
    ]
    cores = [x for x, _, red in pixels if red >= 224]
    if not cores:
        return {"action_dy": None, "hint_dy": None}
    x0, x1 = min(cores), max(cores)
    centre = top + (CAPSULE_H - 1) / 2

    def offset(ys):
        return (min(ys) + max(ys)) / 2 - centre if ys else None

    hint = [(x, y) for x, y, red in pixels if red >= 112
            and (x < x0 - 1 if hint_left else x > x1 + 1)]
    return {
        "action_width": x1 - x0 + 1,
        "hint_width": max(x for x, _ in hint) - min(x for x, _ in hint) + 1 if hint else 0,
        "action_dy": offset([y for x, y, red in pixels
                             if x0 - 1 <= x <= x1 + 1 and red >= 112]),
        "hint_dy": offset([y for _, y in hint]),
    }


def capsule_fill(frame, left, top, right):
    # Sample near both side edges, away from the ink. RGB565 rounding and
    # the backdrop visible through the tint can shift channels slightly.
    xs = (left + 3, left + 4, right - 5, right - 4)
    return [tuple(median(frame.getpixel((x, top + dy))[ch] for x in xs)
                  for ch in range(3))
            for dy in (4, 10, 14, 23)]


def ink_bounds(frame, box, flat=False):
    """Return visible text bounds, excluding the button's paint."""
    left, top, right, bottom = box
    points = []
    for y in range(top + 2, bottom - 2):
        row = [frame.getpixel((x, y))[0] for x in range(left + 2, right - 2)]
        fill = median(row)
        points.extend((x, y) for x, red in enumerate(row, left + 2)
                      if (abs(red - fill) >= 32 if flat else red >= 112))
    if points:
        xs, ys = zip(*points)
        return [min(xs), min(ys), max(xs) + 1, max(ys) + 1]
    return None


def has_ink_padding(box, ink, padding):
    return ink is not None and ink[0] > box[0] + padding and ink[2] < box[2] - padding


def flat_button_readings(frame, boxes):
    readings = []
    for left, top, right, bottom in boxes:
        box = (left, top, right, bottom)
        edge = frame.getpixel((left, top))
        border = edge in ((99, 101, 99), (214, 211, 214)) and all(
            frame.getpixel((x, y)) == edge
            for x, y in ((right - 1, top), (left, bottom - 1), (right - 1, bottom - 1))
        )
        ink = ink_bounds(frame, box, flat=True)
        readings.append({"button": box, "ink": ink,
                         "padding_ok": border and has_ink_padding(box, ink, PAIR_PAD)})
    return readings


def pair_pixels(frame, columns, arrows):
    """Read each line independently, including any hardware-key triangle."""
    pairs = []
    for (left, right), arrow in zip(columns, arrows):
        tops = capsule_tops(frame, left, right, PAIR_H)
        if not tops:
            return []
        top = tops[-1]
        box = [left, top, right, top + PAIR_H]
        ink = ink_bounds(frame, box)
        action = ink_bounds(frame, [left, top, right, top + 21])
        hint = ink_bounds(frame, [left, top + 18, right, top + PAIR_H])
        arrow_pixels = None
        if hint and arrow:
            x = hint[0] if arrow < 0 else hint[2] - 4
            arrow_pixels = [sum(frame.getpixel((col, y))[0] >= 112
                                for y in range(top + 24, top + 31))
                            for col in range(x, x + 4)]
        pairs.append({
            "button": box, "ink": ink, "action": action, "hint": hint,
            "padding_ok": has_ink_padding(box, ink, PAIR_PAD),
            "action_dy": (action[1] + action[3]) / 2 - (top + 11) if action else None,
            "hint_dy": (hint[1] + hint[3]) / 2 - (top + 27) if hint else None,
            "arrow_pixels": arrow_pixels, "fill": capsule_fill(frame, left, top, right),
        })
    return pairs


def check_pair_pixels(frame, columns, arrows):
    readings = pair_pixels(frame, columns, arrows)
    valid = len(readings) == 2 and readings[0]["button"][1] == readings[1]["button"][1]
    centered = valid and all(
        p[key] is not None and abs(p[key]) <= 1
        for p in readings for key in ("action_dy", "hint_dy")
    )
    padding = valid and all(p["padding_ok"] for p in readings)
    full_arrows = valid and all(
        not arrow or p["arrow_pixels"] == ([1, 3, 5, 7] if arrow < 0 else [7, 5, 3, 1])
        for p, arrow in zip(readings, arrows)
    )
    same_fill = valid and all(
        abs(a - b) <= 16
        for row_left, row_right in zip(readings[0]["fill"], readings[1]["fill"])
        for a, b in zip(row_left, row_right)
    )
    return readings, {
        "button_ink_centered": centered, "button_ink_padding": padding,
        "button_arrows_complete": full_arrows, "buttons_same_fill": same_fill,
    }


def check_button_pixels(out, theme):
    measurements = {}
    results = {}
    for kind, suffix, columns, expected in (
        ("usb", "01-usb", [(36, 284, True)], [2]),
        ("prose", "04-prose", [(82, 238, False)], [1]),
    ):
        with Image.open(out / f"{theme}-{suffix}.png") as image:
            frame = image.convert("RGB")
        capsules = []
        counts = []
        for left, right, hint_left in columns:
            tops = capsule_tops(frame, left, right)
            counts.append(len(tops))
            for top in tops:
                reading = capsule_pixels(frame, left, top, right, hint_left)
                reading["button"] = [left, top, right, top + CAPSULE_H]
                reading["fill"] = capsule_fill(frame, left, top, right)
                capsules.append(reading)
        measurements[kind] = capsules
        results[f"{kind}_button_ink_centered"] = counts == expected and all(
            reading[key] is not None and abs(reading[key]) <= 1
            for reading in capsules for key in ("action_dy", "hint_dy")
        )
        if kind == "usb":
            # MENU is shorter than SELECT and Charge Only is longer than Connect.
            results["usb_menu_above_select"] = counts == expected and (
                capsules[0].get("hint_width", 0) > 0
                and capsules[0].get("hint_width", 0) < capsules[1].get("hint_width", 0)
                and capsules[0].get("action_width", 0) > capsules[1].get("action_width", 0)
            )
            results[f"{kind}_buttons_same_fill"] = counts == expected and all(
                abs(a - b) <= 16
                for reading in capsules[1:]
                for ref, actual in zip(capsules[0]["fill"], reading["fill"])
                for a, b in zip(ref, actual)
            )
    with Image.open(out / f"{theme}-02-yesno.png") as image:
        readings, checked = check_pair_pixels(image.convert("RGB"),
                                              [(36, 156), (164, 284)], [-1, 1])
    measurements["yesno"] = readings
    results.update({f"yesno_{test}": passed for test, passed in checked.items()})
    (out / f"{theme}-button-pixels.json").write_text(
        json.dumps(measurements, indent=2) + "\n")
    return results


def settle(s, steps=45):
    time.sleep(3 / FPS)
    last = s.frame()
    same = 0
    for _ in range(steps):
        time.sleep(5 / FPS)
        cur = s.frame()
        if not differ(last, cur):
            same += 1
            if same >= 2:
                return cur
        else:
            same = 0
            last = cur
    return last


def open_usb(s, before):
    s.tap("u")
    # The USB event is posted by the simulator's USB thread. Give it time to
    # reach the UI before deciding that a stable frame is the prompt.
    for _ in range(10):
        time.sleep(0.1)
        if differ(before, s.frame(), (0, 36, 320, 240)):
            break
    return settle(s)


def open_explain(s):
    s.key("keydown", "Return")
    time.sleep(0.9)
    s.key("keyup", "Return")
    settle(s)
    s.tap("Return")  # Explain
    return settle(s)


def run_theme(binary, simdisk, env, out, theme):
    res = {}
    classic = theme != "scrim"
    fruit = theme.startswith("fruit-company")
    bar_h = STATUS_H if classic else 36
    with tempfile.TemporaryDirectory(prefix=f"era-dlg-{theme}-") as temp:
        root = Path(temp)
        rb = root / ".rockbox"
        rb.mkdir()
        for entry in (simdisk / ".rockbox").iterdir():
            if entry.name not in ("config.cfg", "ui-perf.txt", "ui-perf.enable"):
                if entry.name != "native-themes" or theme != "fruit-company-ultra":
                    (rb / entry.name).symlink_to(entry)
        if theme == "fruit-company-ultra":
            themes = rb / "native-themes"
            themes.mkdir()
            (themes / "fruit-company-ultra.podtheme").write_text(
                ULTRA_FALLBACK_THEME + "style.dialog=classic\n")
        start = "root" if fruit else "menu"
        (rb / "config.cfg").write_text(
            (simdisk / ".rockbox/default-config.cfg").read_text()
            + f"\nnative theme: {theme}\nstart in screen: {start}\n"
            + "backlight timeout: off\nvoice menus: off\nusb mode: mass storage\n"
            + "animations: on\nshow background tasks: off\nshow disk activity: off\n"
        )
        with (out / f"{theme}.log").open("w") as slog, process(
            [str(binary), "--root", str(root), "--nobackground", "--zoom", "1",
             "--screenshot"],
            slog, env=env, cwd=root,
        ) as fw:
            time.sleep(100 / FPS)
            s = Screen(env, fw)
            time.sleep(60 / FPS)
            home = settle(s)
            home.save(out / f"{theme}-00-home.png")

            # 1. USB Disk Mode consent: MENU = Charge Only (leaves screen intact)
            usb = open_usb(s, home)
            usb.save(out / f"{theme}-01-usb.png")
            res["usb_opened"] = differ(home, usb)
            if classic:
                res["usb_is_popup"] = is_dimmed_popup(home, usb)
            # Transport and wheel input keep the two-choice prompt open.
            for k in ("space", "Down", "Left", "Right"):
                s.tap(k)
                time.sleep(0.15)
            res["usb_transport_and_wheel_keep_prompt"] = not differ(usb, settle(s))
            # One press of MENU -> Charge Only -> restores home cleanly
            s.tap("Escape")
            after_usb_menu = settle(s)
            res["usb_menu_restores_home"] = not differ(
                home, after_usb_menu, (0, bar_h, 160, 240))
            # Unplug USB cable while still in Charge Only so we can re-plug at end
            s.tap("u")
            time.sleep(0.25)

            if fruit:
                # 2. Fruit Company Yes/No: Settings (5) -> Main Menu (3) -> Reset Menu (last)
                for _ in range(5):
                    s.tap("Down")
                    time.sleep(0.12)
                s.tap("Return")
                settle(s)
                for _ in range(3):
                    s.tap("Down")
                    time.sleep(0.12)
                s.tap("Return")
                settle(s)
                s.tap("Up")
                time.sleep(0.15)
                before_yn = settle(s)

                # Open Yes/No pop-up, verify pop-up form, PLAY/wheel inert, LEFT = No
                s.tap("Return")
                yn = settle(s)
                yn.save(out / f"{theme}-02-yesno.png")
                res["yesno_opened"] = differ(before_yn, yn)
                res["yesno_is_popup"] = is_dimmed_popup(before_yn, yn)
                for k in ("space", "Down", "Up"):
                    s.tap(k)
                    time.sleep(0.15)
                res["yesno_play_wheel_inert"] = not differ(yn, settle(s))
                s.tap("Left")
                res["yesno_left_cancels"] = not differ(
                    before_yn, settle(s), (0, STATUS_H, 320, 240))

                # MENU = No (one press)
                s.tap("Return")
                settle(s)
                s.tap("Escape")
                res["yesno_menu_cancels"] = not differ(
                    before_yn, settle(s), (0, STATUS_H, 320, 240))

                # RIGHT = Yes (one press)
                s.tap("Return")
                settle(s)
                s.tap("Right")
                res["yesno_right_accepts"] = not differ(
                    before_yn, settle(s), (0, STATUS_H, 320, 240))

                # SELECT = Yes (one press)
                s.tap("Return")
                settle(s)
                s.tap("Return")
                res["yesno_select_accepts"] = not differ(
                    before_yn, settle(s), (0, STATUS_H, 320, 240))

                # 3. Settings -> Advanced Settings -> Sound Settings -> Volume -> Explain
                s.tap("Escape")
                settle(s)
                for _ in range(4):
                    s.tap("Up")
                    time.sleep(0.12)
                s.tap("Return")
                settle(s)  # Advanced Settings (starts on Sound Settings)
                s.tap("Return")
                settle(s)  # Sound Settings (item 0 = Volume)
                before_prose = settle(s)

                prose = open_explain(s)
                prose.save(out / f"{theme}-04-prose.png")
                res["prose_opened"] = differ(before_prose, prose)
                res["prose_is_popup"] = is_dimmed_popup(before_prose, prose)
                s.tap("space")
                time.sleep(0.15)
                res["prose_play_inert"] = not differ(prose, settle(s))
                s.tap("Return")
                res["prose_select_dismisses"] = not differ(
                    before_prose, settle(s), (0, STATUS_H, 320, 240))

                open_explain(s)
                s.tap("Escape")
                res["prose_menu_dismisses"] = not differ(
                    before_prose, settle(s), (0, STATUS_H, 320, 240))

                open_explain(s)
                s.tap("Left")
                res["prose_left_dismisses"] = not differ(
                    before_prose, settle(s), (0, STATUS_H, 320, 240))

                open_explain(s)
                s.tap("Right")
                res["prose_right_dismisses"] = not differ(
                    before_prose, settle(s), (0, STATUS_H, 320, 240))

                # Menu leaves the nested Advanced Settings screen. Re-enter
                # it from the retained Advanced Settings row.
                s.tap("Escape")
                settle(s)
                s.tap("Return")
                settle(s)

                # 4. Search Settings (index 0, one Up above Sound Settings)
                s.tap("Up")
                time.sleep(0.15)
                before_srch = settle(s)
                s.tap("Return")
                srch = settle(s)
                srch.save(out / f"{theme}-05-search.png")
                res["search_opened"] = differ(before_srch, srch)
                s.tap("Escape")
                settle(s)

                # 5. Library -> Maintenance -> Update Now -> LEFT cancel -> Cancelled splash
                for _ in range(4):
                    s.tap("Down")
                    time.sleep(0.15)
                settle(s)
                s.tap("Return")
                settle(s)  # Library
                s.tap("Up")
                settle(s)
                s.tap("Return")
                settle(s)  # Maintenance
                before_spl = settle(s)
                s.tap("Return")
                settle(s)  # Update Now yesno pop-up
                s.tap("Left")
                time.sleep(0.45)
                spl = s.frame()
                spl.save(out / f"{theme}-03-splash.png")
                res["splash_captured"] = differ(before_spl, spl)
                time.sleep(1.3)
                after_spl = settle(s)
                res["splash_restores"] = not differ(
                    before_spl, after_spl, (0, STATUS_H, 320, 240))
            else:
                # Classic / Scrim: start in Settings (index 1 = Sound Settings)
                # 2. Sound Settings -> Volume -> Explain (prose dialog)
                s.tap("Return")
                settle(s)  # Sound Settings (item 0 = Volume)
                before_prose = settle(s)
                prose = open_explain(s)
                prose.save(out / f"{theme}-04-prose.png")
                res["prose_opened"] = differ(before_prose, prose)
                if classic:
                    res["prose_is_popup"] = is_dimmed_popup(before_prose, prose)
                    s.tap("space")
                    time.sleep(0.15)
                    res["prose_play_inert"] = not differ(prose, settle(s))
                s.tap("Return")
                res["prose_select_dismisses"] = not differ(
                    before_prose, settle(s), (0, bar_h, 320, 240))

                if classic:
                    open_explain(s)
                    s.tap("Escape")
                    res["prose_menu_dismisses"] = not differ(
                        before_prose, settle(s), (0, bar_h, 320, 240))

                    open_explain(s)
                    s.tap("Left")
                    res["prose_left_dismisses"] = not differ(
                        before_prose, settle(s), (0, bar_h, 320, 240))

                    open_explain(s)
                    s.tap("Right")
                    res["prose_right_dismisses"] = not differ(
                        before_prose, settle(s), (0, bar_h, 320, 240))

                s.tap("Left")
                settle(s)  # back one level to Settings (on Sound Settings)

                # 3. Search Settings (index 0, one Up above Sound Settings)
                s.tap("Up")
                time.sleep(0.15)
                before_srch = settle(s)
                s.tap("Return")
                srch = settle(s)
                srch.save(out / f"{theme}-05-search.png")
                res["search_opened"] = differ(before_srch, srch)
                s.tap("Escape")
                settle(s)

                # 4. Library -> Maintenance -> Update Now (yesno + splash)
                for _ in range(4):
                    s.tap("Down")
                    time.sleep(0.15)
                settle(s)
                s.tap("Return")
                settle(s)  # Library
                s.tap("Up")
                settle(s)
                s.tap("Return")
                settle(s)  # Maintenance
                before_yn = settle(s)
                s.tap("Return")
                yn = settle(s)
                yn.save(out / f"{theme}-02-yesno.png")
                res["yesno_opened"] = differ(before_yn, yn)

                if classic:
                    res["yesno_is_popup"] = is_dimmed_popup(before_yn, yn)
                    for k in ("space", "Down", "Up"):
                        s.tap(k)
                        time.sleep(0.15)
                    res["yesno_play_wheel_inert"] = not differ(yn, settle(s))
                    # LEFT = No -> triggers timed "Cancelled" splash (1s)
                    s.tap("Left")
                    time.sleep(0.45)
                    spl_cancel = s.frame()
                    spl_cancel.save(out / f"{theme}-03-splash.png")
                    res["splash_captured"] = differ(before_yn, spl_cancel)
                    time.sleep(1.5)
                    res["yesno_left_cancels"] = not differ(
                        before_yn, settle(s), (0, bar_h, 320, 240))

                    # MENU = No -> triggers "Cancelled" splash -> restores
                    s.tap("Return")
                    settle(s)
                    s.tap("Escape")
                    time.sleep(1.8)
                    res["yesno_menu_cancels"] = not differ(
                        before_yn, settle(s), (0, bar_h, 320, 240))

                    # RIGHT = Yes -> triggers "Started" splash (not "Cancelled") -> restores
                    s.tap("Return")
                    settle(s)
                    s.tap("Right")
                    time.sleep(0.45)
                    spl_right = s.frame()
                    time.sleep(1.5)
                    res["yesno_right_accepts"] = (
                        differ(spl_cancel, spl_right)
                        and not differ(before_yn, settle(s), (0, bar_h, 320, 240))
                    )

                    # SELECT = Yes -> triggers "Started" splash (not "Cancelled") -> restores
                    s.tap("Return")
                    settle(s)
                    s.tap("Return")
                    time.sleep(0.45)
                    spl_sel = s.frame()
                    time.sleep(1.5)
                    res["yesno_select_accepts"] = (
                        differ(spl_cancel, spl_sel)
                        and not differ(before_yn, settle(s), (0, bar_h, 320, 240))
                    )
                else:
                    s.tap("Down")
                    yn2 = settle(s)
                    yn2.save(out / f"{theme}-02b-yesno-no.png")
                    res["yesno_wheel_moves"] = differ(yn, yn2)
                    s.tap("Escape")
                    time.sleep(0.45)
                    spl = s.frame()
                    spl.save(out / f"{theme}-03-splash.png")
                    res["splash_captured"] = differ(before_yn, spl)
                    time.sleep(1.5)
                    res["yesno_restores"] = not differ(
                        before_yn, settle(s), (0, bar_h, 320, 240))

            # Final check: USB SELECT = Connect (one press enters USB connected mode)
            before_usb_sel = settle(s)
            usb2 = open_usb(s, before_usb_sel)
            res["usb_reopened"] = differ(before_usb_sel, usb2)
            s.tap("Return")
            usb_conn = settle(s)
            res["usb_select_connects"] = (
                differ(before_usb_sel, usb_conn) and differ(usb2, usb_conn)
            )
            s.tap("u")
            settle(s)

            assert fw.poll() is None, f"{theme}: simulator exited unexpectedly"
    return res


@contextmanager
def private_display(log_path):
    with tempfile.TemporaryDirectory(prefix="era-dlg-x-") as temp, \
            tempfile.TemporaryFile() as display, \
            log_path.open("w") as log:
        runtime = Path(temp) / "runtime"
        runtime.mkdir(mode=0o700)
        with process(
            ["Xvfb", "-displayfd", str(display.fileno()), "-screen", "0",
             "800x600x24", "-nolisten", "tcp"],
            log, pass_fds=(display.fileno(),),
        ):
            for _ in range(FPS * 5):
                display.seek(0)
                num = display.read().decode().strip()
                if num:
                    break
                time.sleep(1 / FPS)
            else:
                raise RuntimeError("Xvfb did not start")
            env = dict(os.environ, DISPLAY=":" + num, XDG_RUNTIME_DIR=str(runtime),
                       SDL_VIDEODRIVER="x11", SDL_AUDIODRIVER="dummy",
                       SDL_RENDER_DRIVER="software")
            yield env


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("binary", "simdisk", "out"):
        parser.add_argument(name, type=lambda p: Path(p).resolve())
    parser.add_argument("--themes", default="classic,fruit-company,fruit-company-ultra,scrim")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    result = {}
    with private_display(args.out / "xvfb.log") as env:
        for theme in args.themes.split(","):
            print(f"Testing {theme}...", flush=True)
            result[theme] = run_theme(args.binary, args.simdisk, env, args.out, theme)
            if theme != "scrim":
                result[theme].update(check_button_pixels(args.out, theme))
            else:
                boxes = [(99, 128, 151, 157), (169, 128, 221, 157)]
                for mode, suffix in (("yes", "02-yesno"), ("no", "02b-yesno-no")):
                    with Image.open(args.out / f"scrim-{suffix}.png") as image:
                        result[theme][f"yesno_{mode}_button_ink_padding"] = all(
                            item["padding_ok"] for item in flat_button_readings(image, boxes)
                        )
            (args.out / "result.json").write_text(json.dumps(result, indent=2) + "\n")

    failed = [t for t, r in result.items() if not all(r.values())]
    print(json.dumps(result, indent=2))
    if failed:
        print("FAIL:", failed)
        return 1
    print(f"PASS all {len(result)} themes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
