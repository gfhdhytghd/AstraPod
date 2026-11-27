#!/usr/bin/env python3
"""Select changes a short setting in place, in SDL, under each theme.

Arguments: ROCKBOXUI SIMDISK OUTPUT_DIRECTORY [--theme NAME ...]
SIMDISK is a simulator root with .rockbox in it, as build-sim.sh leaves.
Xvfb is private to this process; no window opens on the desktop.

Under each theme the walk opens Settings > Playback, whose first rows are
Shuffle (three values), Repeat (five) and Play Selected (on/off). Under
Fruit Company that is Settings > Advanced Settings > Playback. There:

- Select on Shuffle and on Play Selected must change only that row of
  the list -- no other pixel of the list moves, so no page opened -- show a
  different value at every step, and bring the row back after as many
  Selects as the setting has values;
- Select on Repeat must open its page, which Menu closes again onto the
  unchanged list;
- holding Select on Play Selected must open the row's context menu without
  changing the value, and Menu must close it again;
- Select on Playback Frequency, four rows above the list's end and a number
  of three values, must step in place as Shuffle does;
- in Recording Settings, the menu below Playback, Select on Recording
  Format (four values, a function row rather than a settings row) must
  step in place as Shuffle does.

The walk runs with every setting shown, as Playback Frequency is advanced.
Each row is then left one step on, and the saved settings must come to
hold the new values. Writes the frames and
result.json to the output directory.
"""

import argparse
import json
import os
import re
import tempfile
import time
from pathlib import Path

from PIL import ImageChops

from ui_menu_sim import FPS, Screen, process
from ui_quickscreen_sim import ULTRA_FALLBACK_THEME

FULL = (0, 22, 320, 240)
HALF = (0, 22, 160, 240)

# theme: (list region, start screen, path of (key, presses) to Playback)
THEMES = {
    "scrim": (FULL, "menu", [("Down", 1), ("Return", 1)]),
    "light": (FULL, "menu", [("Down", 1), ("Return", 1)]),
    "classic": (HALF, "menu", [("Down", 1), ("Return", 1)]),
    "fruit-company": (HALF, "root",
                      [("Down", 5), ("Return", 1), ("Down", 17),
                       ("Return", 1), ("Down", 1), ("Return", 1)]),
}
THEMES["fruit-company-ultra"] = THEMES["fruit-company"]
# From Playback back into the settings tree's top level. The tree is a
# screen of its own -- Settings started in, or Fruit Company's Advanced
# Settings -- which Menu leaves, with its row still selected.
BACK = [("Escape", 1), ("Return", 1)]
# Recording Settings' row in the settings tree's top level, below Search,
# Sound and Playback. The tree opens on Sound.
RECORDING_ROW = 3

# (name, row in Playback, cfg name, value after one step, values)
STEPPED = [
    ("shuffle", 0, "shuffle", "on", 3),
    ("play_selected", 2, "play selected", "off", 2),
]
REPEAT_ROW = 1
# (name, rows above Playback's last, cfg name, value after one step, values)
FREQUENCY = ("play_frequency", 4, "playback frequency", "44100", 3)
# The same in Recording Settings, the menu below Playback, whose rows are
# function rows rather than settings rows
RECORDING = [
    ("rec_format", 0, "rec format", "aiff", 4),
]
# Every row left one step on, whose value config.cfg must come to hold
SAVED = STEPPED + [FREQUENCY] + RECORDING
# A row is shorter than this; a change taller than it is a new page
ROW_MAX = 40
# Where press() leaves the screen it gave up on
FAILED_FRAME = None


def changed(a, b, region):
    """The box, in region coordinates, where a and b differ, or None."""
    return ImageChops.difference(a.crop(region), b.crop(region)).getbbox()


def settle(s, region, frames=40):
    last = s.frame()
    for _ in range(frames):
        time.sleep(6 / FPS)
        frame = s.frame()
        if not changed(last, frame, region):
            return frame
        last = frame
    return last


def press(s, key, region, tries=4):
    """Tap `key` until the list changes: the simulator drops an event now
    and then, most often the first after Select or Menu."""
    before = settle(s, region, 20)
    for _ in range(tries):
        s.tap(key)
        for _ in range(45):
            time.sleep(4 / FPS)
            if changed(before, s.frame(), region):
                return settle(s, region)
    if FAILED_FRAME:
        s.frame().save(FAILED_FRAME)
    raise RuntimeError(f"{key} changed nothing")


def hold(s, key, region, seconds=1.2):
    before = settle(s, region, 20)
    s.key("keydown", key)
    time.sleep(seconds)
    s.key("keyup", key)
    for _ in range(45):
        time.sleep(4 / FPS)
        if changed(before, s.frame(), region):
            return settle(s, region)
    return settle(s, region)


def end(s, region, key="Up"):
    """`key` until two taps running change nothing: the list's first row, or
    with Down its last, with wraparound off."""
    still = 0
    while still < 2:
        before = settle(s, region, 20)
        s.tap(key)
        time.sleep(30 / FPS)
        still = still + 1 if not changed(before, settle(s, region),
                                         region) else 0


def move(s, region, rows):
    for _ in range(abs(rows)):
        press(s, "Down" if rows > 0 else "Up", region)


def walk(s, region, path):
    for key, times in path:
        for _ in range(times):
            press(s, key, region)


def band(box):
    return box is not None and box[3] - box[1] <= ROW_MAX


def check_stepped(s, region, out, theme, name, values):
    menu = settle(s, region)
    menu.save(out / f"{theme}-{name}-0.png")
    frames = [menu]
    in_place = True
    for i in range(1, values + 1):
        frame = press(s, "Return", region)
        frame.save(out / f"{theme}-{name}-{i}.png")
        in_place &= band(changed(frames[-1], frame, region))
        frames.append(frame)
    distinct = all(changed(frames[i], frames[j], region)
                   for i in range(values) for j in range(i))
    back = not changed(frames[0], frames[-1], region)
    # Left one step on, for config.cfg
    press(s, "Return", region)
    return {"values": values, "in_place": in_place,
            "distinct_values": distinct, "back_to_first": back,
            "ok": in_place and distinct and back}


def check_page(s, region, out, theme):
    menu = settle(s, region)
    page = press(s, "Return", region)
    page.save(out / f"{theme}-repeat-page.png")
    box = changed(menu, page, region)
    opened = box is not None and not band(box)
    press(s, "Escape", region)
    back = not changed(menu, settle(s, region), region)
    return {"page_opened": opened, "menu_closes_it": back,
            "ok": opened and back}


def check_context(s, region, out, theme):
    menu = settle(s, region)
    context = hold(s, "Return", region)
    context.save(out / f"{theme}-play_selected-context.png")
    box = changed(menu, context, region)
    opened = box is not None and not band(box)
    if opened:
        press(s, "Escape", region)
    after = settle(s, region)
    unchanged = not changed(menu, after, region)
    return {"context_opened": opened, "value_unchanged": unchanged,
            "ok": opened and unchanged}


def cfg_value(path, name):
    text = path.read_text(errors="replace") if path.exists() else ""
    m = re.search(r"^%s: (.*)$" % re.escape(name), text, re.M)
    return m.group(1).strip() if m else None


def run_theme(args, env, theme, out):
    global FAILED_FRAME
    region, start, path = THEMES[theme]
    FAILED_FRAME = out / f"{theme}-failed.png"
    result = {}
    with tempfile.TemporaryDirectory(prefix="select-toggle-") as temp:
        root = Path(temp) / "root"
        rockbox = root / ".rockbox"
        rockbox.mkdir(parents=True)
        for entry in (args.simdisk / ".rockbox").iterdir():
            if entry.name not in ("config.cfg", "library", "native-themes"):
                (rockbox / entry.name).symlink_to(entry)
        if theme == "fruit-company-ultra":
            themes = rockbox / "native-themes"
            themes.mkdir()
            filename = "fruit-company-ultra.podtheme"
            original = args.simdisk / ".rockbox/native-themes" / filename
            (themes / filename).write_text(original.read_text() if original.exists()
                                           else ULTRA_FALLBACK_THEME)
        config = rockbox / "config.cfg"
        config.write_text(
            (args.simdisk / ".rockbox/default-config.cfg").read_text() +
            f"\nnative theme: {theme}\nstart in screen: {start}\n"
            "backlight timeout: off\nvoice menus: off\nanimations: off\n"
            "list wraparound: off\nsettings mode: everything\n"
            "shuffle: off\nplay selected: on\nrepeat: off\n"
            "rec format: wave\nplayback frequency: auto\n")
        with (out / f"{theme}.log").open("w") as log, \
                process([str(args.binary), "--root", str(root),
                         "--nobackground", "--zoom", "1", "--screenshot"],
                        log, env=env, cwd=root) as firmware:
            time.sleep(150 / FPS)
            s = Screen(env, firmware)
            time.sleep(5)
            settle(s, region)
            walk(s, region, path)
            settle(s, region).save(out / f"{theme}-playback.png")

            row = 0
            for name, at, _, _, values in STEPPED:
                move(s, region, at - row)
                row = at
                result[name] = check_stepped(s, region, out, theme, name,
                                             values)
                if name == "play_selected":
                    result["context_menu"] = check_context(s, region, out,
                                                           theme)
            move(s, region, REPEAT_ROW - row)
            result["repeat"] = check_page(s, region, out, theme)

            name, above, _, _, values = FREQUENCY
            end(s, region, "Down")
            move(s, region, -above)
            result[name] = check_stepped(s, region, out, theme, name, values)

            walk(s, region, BACK)
            end(s, region)
            move(s, region, RECORDING_ROW)
            press(s, "Return", region)
            settle(s, region).save(out / f"{theme}-recording.png")
            row = 0
            for name, at, _, _, values in RECORDING:
                move(s, region, at - row)
                row = at
                result[name] = check_stepped(s, region, out, theme, name,
                                             values)

            assert firmware.poll() is None, "simulator exited"
            # Settings reach config.cfg.new once storage has been idle a
            # while; the next boot renames it over config.cfg
            saved_cfg = config.with_name("config.cfg.new")
            want = {cfg: value for _, _, cfg, value, _ in SAVED}
            for _ in range(90):
                if saved_cfg.exists() and all(
                        cfg_value(saved_cfg, k) == v for k, v in want.items()):
                    break
                time.sleep(1)
        if not saved_cfg.exists():
            saved_cfg = config
        (out / f"{theme}-config.cfg").write_text(saved_cfg.read_text())
        for name, _, cfg, value, _ in SAVED:
            saved = cfg_value(saved_cfg, cfg)
            result[name]["config"] = saved
            result[name]["ok"] &= saved == value
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("binary", "simdisk", "out"):
        parser.add_argument(name, type=lambda p: Path(p).resolve())
    parser.add_argument("--theme", action="append", choices=sorted(THEMES))
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    results = {}
    with tempfile.TemporaryDirectory(prefix="select-toggle-x-") as temp, \
            tempfile.TemporaryFile() as display, \
            (args.out / "xvfb.log").open("w") as log:
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
            for theme in args.theme or list(THEMES):
                try:
                    results[theme] = run_theme(args, env, theme, args.out)
                except (RuntimeError, AssertionError) as e:
                    results[theme] = {"error": {"ok": False, "why": str(e)}}
                (args.out / "result.json").write_text(
                    json.dumps(results, indent=1))
    failed = [(t, k) for t, r in results.items() for k, v in r.items()
              if not v["ok"]]
    for theme, key in failed:
        print("FAIL", theme, key, results[theme][key])
    total = sum(len(r) for r in results.values())
    print(f"{total - len(failed)} of {total} checks pass")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
