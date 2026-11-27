#!/usr/bin/env python3
"""Headless simulator tests for Classic / Fruit Company / Scrim dialogs.

Arguments: ROCKBOXUI SIMDISK OUTPUT_DIRECTORY [--themes classic,fruit-company,scrim]

For each theme, opens and screenshots:
  - USB Disk Mode consent (one-press MENU = Charge Only, SELECT = Connect; PLAY stays open)
  - Yes/No confirmation (pop-up under Classic/Fruit; one-press LEFT/MENU = No,
    RIGHT/SELECT = Yes; PLAY stays open; wheel does not shift selection)
  - Transient dark-glass splash notice (captured while visible, restores cleanly)
  - Prose dialog (Setting -> Context -> Explain; pop-up under Classic/Fruit;
    one-press SELECT/MENU/LEFT/RIGHT dismiss; PLAY stays open)
  - Search dialog (full-screen stock page under Classic/Fruit)

Verifies every dialog opens, answers with one key press per mapped key, and
dismisses cleanly restoring the screen underneath.
"""

import argparse
import json
import os
import tempfile
import time
from pathlib import Path

from PIL import ImageChops, ImageStat

from ui_menu_sim import FPS, Screen, process
from ui_quickscreen_sim import ULTRA_FALLBACK_THEME

STATUS_H = 20


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
            s.tap("u")
            usb = settle(s)
            usb.save(out / f"{theme}-01-usb.png")
            res["usb_opened"] = differ(home, usb)
            if classic:
                res["usb_is_popup"] = is_dimmed_popup(home, usb)
                # PLAY stays playback, wheel & LEFT do not dismiss or shift
                for k in ("space", "Down", "Left"):
                    s.tap(k)
                    time.sleep(0.15)
                res["usb_play_wheel_left_inert"] = not differ(usb, settle(s))
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
            s.tap("u")
            usb2 = settle(s)
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("binary", "simdisk", "out"):
        parser.add_argument(name, type=lambda p: Path(p).resolve())
    parser.add_argument("--themes", default="classic,fruit-company,scrim")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    result = {}
    with tempfile.TemporaryDirectory(prefix="era-dlg-x-") as temp, \
            tempfile.TemporaryFile() as display, \
            (args.out / "xvfb.log").open("w") as log:
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
            for theme in args.themes.split(","):
                print(f"Testing {theme}...", flush=True)
                result[theme] = run_theme(args.binary, args.simdisk, env, args.out, theme)
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
