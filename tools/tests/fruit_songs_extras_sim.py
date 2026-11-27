#!/usr/bin/env python3
"""Fruit Company's Songs row and Extras pane, in a translated language.

Arguments: ROCKBOXUI SIMDISK OUTPUT_DIRECTORY
SIMDISK is a simulator root with .rockbox in it, as build-sim.sh leaves.
Xvfb is private to this process. Runs the simulator in Simplified Chinese,
where the browser names tagnavi rows in Chinese, and checks:

- Music > Songs opens the list of song titles, not the "Tracks by" menu
  (Filename / Title / Title with duration) that lists the ways to show them.
- Music > Artists opens the list of artists, not the whole Music menu.
- The main menu's Extras row shows the Clocks picture, as stock does, and
  not the Games dice.

Writes the screens it compared and result.json to the output directory.
"""

import argparse
import json
import os
import subprocess
import tempfile
import time
from pathlib import Path

from PIL import ImageChops

from fruit_tree_sim import press, seed, settle
from ui_menu_sim import FPS, Screen, process

# The pane beside the list, below the status bar
PANE = (160, 22, 320, 240)
# Rows of the stock list: the first starts under the status bar
ROW_TOP, ROW_H = 22, 25


def rows_with_text(frame):
    """How many list rows carry any dark pixel: text on the white list."""
    count = 0
    for top in range(ROW_TOP, 240 - ROW_H + 1, ROW_H):
        band = frame.crop((8, top + 3, 150, top + ROW_H - 3)).convert("L")
        if band.getextrema()[0] < 128:
            count += 1
    return count


def text_lines(frame):
    """How many separate lines of dark text the list half holds, whatever
    its row height. Some lists put a strip of this colour down the left."""
    strip = (41, 44, 41)
    lines, inside = 0, False
    for y in range(22, 240):
        dark = any(sum(p) < 3 * 128 and p != strip
                   for p in (frame.getpixel((x, y)) for x in range(158)))
        lines += dark and not inside
        inside = dark
    return lines


def differs(a, b, box):
    return bool(ImageChops.difference(a.crop(box), b.crop(box)).getbbox())


def still(s, seconds=10):
    """The screen once the pane, which follows the list, has stopped too."""
    settle(s)
    last, quiet = s.frame(), 0
    for _ in range(int(seconds * FPS / 6)):
        time.sleep(6 / FPS)
        frame = s.frame()
        quiet = 0 if differs(last, frame, (0, 22, 320, 240)) else quiet + 1
        if quiet >= 8:
            return frame
        last = frame
    return last


def check(s, out):
    result = {}
    still(s).save(out / "home.png")

    # Main menu: Music, Videos, Photos, Podcasts, Extras
    for _ in range(4):
        press(s, "Down")
    extras = still(s)
    extras.save(out / "extras-row.png")
    press(s, "Return")
    # Extras: Clocks first, Games fifth
    clocks = still(s)
    clocks.save(out / "extras-clocks-row.png")
    for _ in range(4):
        press(s, "Down")
    games = still(s)
    games.save(out / "extras-games-row.png")
    for _ in range(4):
        press(s, "Up")
    press(s, "Escape")
    # The clock's hands move each minute: one of the two Extras panes is
    # drawn in the same minute as the Clocks one
    extras_again = still(s)
    clocks_like = not differs(extras, clocks, PANE) or \
        not differs(extras_again, clocks, PANE)
    not_dice = differs(extras, games, PANE)
    result["extras"] = {"matches_clocks_pane": clocks_like,
                        "differs_from_games_pane": not_dice,
                        "ok": clocks_like and not_dice}

    for _ in range(4):
        press(s, "Up")
    press(s, "Return")
    # Music: Cover Flow, Playlists, Artists, Albums, Compilations, Songs
    for _ in range(2):
        press(s, "Down")
    menu = still(s)
    press(s, "Return")
    artists = still(s)
    artists.save(out / "artists.png")
    lines = text_lines(artists)
    # Three seeded artists under Shuffle, one of them on the selected bar;
    # the Music menu it falls back to has a row for every way to browse
    result["artists"] = {"text_lines": lines, "ok": 2 <= lines <= 5}
    press(s, "Escape")
    if differs(still(s), menu, (0, 22, 160, 240)):
        raise RuntimeError("Back from Artists did not return to Music")
    for _ in range(3):
        press(s, "Down")
    press(s, "Return")
    songs = still(s)
    songs.save(out / "songs.png")
    rows = rows_with_text(songs)
    # Six seeded songs under Shuffle; "Tracks by" has three rows
    result["songs"] = {"rows": rows, "ok": rows >= 6}
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("binary", "simdisk", "out"):
        parser.add_argument(name, type=lambda p: Path(p).resolve())
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="fruit-songs-") as temp, \
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
            "backlight timeout: off\nvoice menus: off\nanimations: off\n"
            "tagcache_scan_on_startup: on\n"
            "lang: /.rockbox/langs/chinese-simp.lng\n")
        seed(root)
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
                time.sleep(20)        # the database's first scan
                result = check(s, args.out)
                assert firmware.poll() is None, "simulator exited"
    (args.out / "result.json").write_text(json.dumps(result, indent=1))
    failed = [k for k, v in result.items() if not v["ok"]]
    for key in failed:
        print("FAIL", key, result[key])
    print(f"{len(result) - len(failed)} of {len(result)} checks pass")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
