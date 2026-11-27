#!/usr/bin/env python3
"""Headless simulator tests for Classic / Fruit Company / Scrim dialogs.

Arguments: ROCKBOXUI SIMDISK OUTPUT_DIRECTORY [--themes classic,fruit-company,scrim]

For each theme, opens and screenshots:
  - USB Disk Mode consent (wheel navigation, MENU cancel, Fruit LEFT/RIGHT/PLAY guard)
  - Yes/No confirmation (wheel toggle, Fruit LEFT/RIGHT/PLAY guard, MENU cancel)
  - Transient splash notice (captured while visible)
  - Scrollable prose dialog (Setting -> Context -> Explain)
  - Search dialog (Settings -> Search Settings / Music -> Search)

Verifies every dialog opens, responds to wheel navigation, closes cleanly
restoring the parent screen, and under Fruit Company ignores LEFT/RIGHT/PLAY
for dialog navigation.
"""

import argparse
import json
import os
import tempfile
import time
from pathlib import Path

from PIL import ImageChops

from ui_menu_sim import FPS, Screen, process

STATUS_H = 20


def differ(a, b, box=None):
    if box:
        a, b = a.crop(box), b.crop(box)
    return bool(ImageChops.difference(a, b).getbbox())


def settle(s, steps=35):
    last = s.frame()
    for _ in range(steps):
        time.sleep(5 / FPS)
        cur = s.frame()
        if not differ(last, cur):
            return cur
        last = cur
    return last


def navigate_menu(s, steps):
    for key in steps:
        s.tap(key)
        time.sleep(0.18)
    return settle(s)


def run_theme(binary, simdisk, env, out, theme):
    res = {}
    with tempfile.TemporaryDirectory(prefix=f"era-dlg-{theme}-") as temp:
        root = Path(temp)
        rb = root / ".rockbox"
        rb.mkdir()
        for entry in (simdisk / ".rockbox").iterdir():
            if entry.name not in ("config.cfg", "ui-perf.txt", "ui-perf.enable"):
                (rb / entry.name).symlink_to(entry)
        start = "root" if theme == "fruit-company" else "menu"
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

            # 1. USB Disk Mode consent
            s.tap("u")
            usb1 = settle(s)
            usb1.save(out / f"{theme}-01-usb.png")
            res["usb_opened"] = differ(home, usb1)

            if theme != "scrim":
                # Wheel down moves Connect -> Charge Only
                s.tap("Down")
                usb2 = settle(s)
                usb2.save(out / f"{theme}-01b-usb-sel2.png")
                res["usb_wheel_moves"] = differ(usb1, usb2)
                if theme == "fruit-company":
                    # LEFT / RIGHT / PLAY must not move selection or dismiss
                    for k in ("Left", "Right", "space"):
                        s.tap(k)
                        time.sleep(0.15)
                    after_lrp = settle(s)
                    res["usb_fruit_ignores_lrp"] = not differ(usb2, after_lrp)
            s.tap("Escape")
            after_usb = settle(s)
            res["usb_restores_home"] = not differ(home, after_usb, (0, STATUS_H, 160, 240))

            if theme == "fruit-company":
                # 2. Fruit Company Yes/No: Settings (5) -> Main Menu (3) -> Reset Menu (last via Up)
                for _ in range(5):
                    s.tap("Down"); time.sleep(0.12)
                s.tap("Return"); settle(s)
                for _ in range(3):
                    s.tap("Down"); time.sleep(0.12)
                s.tap("Return"); settle(s)
                s.tap("Up"); time.sleep(0.15)
                before_yn = settle(s)
                s.tap("Return")
                yn1 = settle(s)
                yn1.save(out / f"{theme}-02-yesno.png")
                res["yesno_opened"] = differ(before_yn, yn1)
                # Fruit: LEFT / RIGHT / PLAY must not change selection or close
                for k in ("Left", "Right", "space"):
                    s.tap(k); time.sleep(0.15)
                yn_lrp = settle(s)
                res["yesno_fruit_ignores_lrp"] = not differ(yn1, yn_lrp)
                # Wheel moves Yes -> No
                s.tap("Down")
                yn2 = settle(s)
                yn2.save(out / f"{theme}-02b-yesno-no.png")
                res["yesno_wheel_moves"] = differ(yn1, yn2)
                s.tap("Escape")
                after_yn = settle(s)
                res["yesno_restores"] = not differ(before_yn, after_yn, (0, STATUS_H, 320, 240))

                # 3. Back to Settings (item 3 = Main Menu) -> Advanced Settings (item 17 via 4x Up)
                s.tap("Escape"); settle(s)
                for _ in range(4):
                    s.tap("Up"); time.sleep(0.12)
                s.tap("Return"); settle(s)  # Advanced Settings (starts on Sound Settings)

                # 4. Sound Settings -> Volume -> hold SELECT -> Explain (prose dialog)
                s.tap("Return"); settle(s)  # Sound Settings (item 0 = Volume)
                before_prose = settle(s)
                s.key("keydown", "Return"); time.sleep(0.9); s.key("keyup", "Return")
                settle(s)
                s.tap("Return")  # Explain
                prose = settle(s)
                prose.save(out / f"{theme}-04-prose.png")
                res["prose_opened"] = differ(before_prose, prose)
                # Fruit: LEFT / RIGHT / PLAY must not close or move prose dialog
                for k in ("Left", "Right", "space"):
                    s.tap(k); time.sleep(0.15)
                prose_lrp = settle(s)
                res["prose_fruit_ignores_lrp"] = not differ(prose, prose_lrp)
                s.tap("Return"); settle(s)  # OK returns to Sound Settings
                s.tap("Escape"); settle(s)  # back one level to Advanced Settings (on Sound Settings)

                # 5. Search Settings (index 0, one Up above Sound Settings)
                s.tap("Up"); time.sleep(0.15)
                before_srch = settle(s)
                s.tap("Return")
                srch = settle(s)
                srch.save(out / f"{theme}-05-search.png")
                res["search_opened"] = differ(before_srch, srch)
                s.tap("Escape"); settle(s)

                # 6. Library (3 below Sound Settings -> 4 below Search) -> Maintenance (last) -> Update Now -> Cancelled splash
                for _ in range(4):
                    s.tap("Down"); time.sleep(0.12)
                s.tap("Return"); settle(s)  # Library
                s.tap("Up"); time.sleep(0.15)
                s.tap("Return"); settle(s)  # Maintenance
                before_spl = settle(s)
                s.tap("Return"); settle(s)  # Update Now yesno
                s.tap("Escape")
                time.sleep(0.45)
                spl = s.frame()
                spl.save(out / f"{theme}-03-splash.png")
                res["splash_captured"] = differ(before_spl, spl)
                time.sleep(1.3)
                after_spl = settle(s)
                res["splash_restores"] = not differ(before_spl, after_spl, (0, STATUS_H, 320, 240))
            else:
                # Classic / Scrim: start in Settings (starts on index 1 = Sound Settings)
                # 2. Sound Settings -> Volume -> hold SELECT -> Explain (prose dialog)
                s.tap("Return"); settle(s)  # Sound Settings (item 0 = Volume)
                before_prose = settle(s)
                s.key("keydown", "Return"); time.sleep(0.9); s.key("keyup", "Return")
                settle(s)
                s.tap("Return")  # Explain
                prose = settle(s)
                prose.save(out / f"{theme}-04-prose.png")
                res["prose_opened"] = differ(before_prose, prose)
                s.tap("Return"); settle(s)  # OK returns to Sound Settings
                s.tap("Left"); settle(s)    # back one level to Settings (on Sound Settings)

                # 3. Search Settings (index 0, one Up above Sound Settings)
                s.tap("Up"); time.sleep(0.15)
                before_srch = settle(s)
                s.tap("Return")
                srch = settle(s)
                srch.save(out / f"{theme}-05-search.png")
                res["search_opened"] = differ(before_srch, srch)
                s.tap("Escape"); settle(s)

                # 4. Library (4 below Search) -> Maintenance (last) -> Update Now (yesno + splash)
                for _ in range(4):
                    s.tap("Down"); time.sleep(0.12)
                s.tap("Return"); settle(s)  # Library
                s.tap("Up"); time.sleep(0.15)
                s.tap("Return"); settle(s)  # Maintenance
                before_yn = settle(s)
                s.tap("Return")
                yn1 = settle(s)
                yn1.save(out / f"{theme}-02-yesno.png")
                res["yesno_opened"] = differ(before_yn, yn1)
                s.tap("Down")
                yn2 = settle(s)
                yn2.save(out / f"{theme}-02b-yesno-no.png")
                res["yesno_wheel_moves"] = differ(yn1, yn2)
                # Cancel -> triggers timed "Cancelled" splash (1s)
                s.tap("Escape")
                time.sleep(0.45)
                spl = s.frame()
                spl.save(out / f"{theme}-03-splash.png")
                res["splash_captured"] = differ(before_yn, spl)
                time.sleep(1.3)
                after_yn = settle(s)
                res["yesno_restores"] = not differ(before_yn, after_yn, (0, STATUS_H, 320, 240))

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
