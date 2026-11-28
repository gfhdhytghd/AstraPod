#!/usr/bin/env python3
"""Walk Fruit Company's menu tree in SDL and check that every row opens.

Arguments: ROCKBOXUI SIMDISK OUTPUT_DIRECTORY
SIMDISK is a simulator root with .rockbox in it, as build-sim.sh leaves.
Xvfb is private to this process. For every row of every menu the walk
selects the row, keeps a screenshot of what opened, and goes back. A row
passes when Select changes the screen and Back returns to the same menu
frame, or, for the rows stock leaves empty, when Select changes nothing.
A row stock changes in place passes when every Select changes the list
and as many Selects as it has values bring the list back.
Writes one PNG per row and result.json to the output directory.
"""

import argparse
import json
import os
import subprocess
import tempfile
import time
from pathlib import Path

from PIL import ImageChops

from ui_menu_sim import FPS, Screen, process

# (name, rows, path of row indices from the main menu). A row is
# (label, kind): "open" must change the screen, "empty" must not, "menu"
# is walked as its own entry, "play" starts playback, after which the main
# menu gains Now Playing and so is not the frame it was: it comes last.
# "cycle:N" steps the row's value in place through N values. The fixture
# contains ordinary songs with no composer tags, audiobooks, or podcasts.
TREE = [
    ("music", [("Cover Flow", "open"), ("Genius Mixes", "open"),
               ("Playlists", "open"),
               ("Artists", "open"), ("Albums", "open"),
               ("Compilations", "empty"), ("Songs", "open"),
               ("Genres", "open"), ("Composers", "empty"),
               ("Audiobooks", "empty"), ("Search", "open")], [0]),
    ("videos", [("Movies", "open"), ("Rentals", "empty"),
                ("TV Shows", "empty"), ("Music Videos", "empty"),
                ("Video Playlists", "empty"), ("Settings", "open")], [1]),
    # The fixture has no pictures, and stock leaves the Photos menu up
    ("photos", [("All Photos", "empty"), ("Settings", "open")], [2]),
    ("extras", [("Clocks", "open"), ("Calendars", "open"),
                ("Contacts", "open"), ("Alarms", "open"), ("Games", "open"),
                ("Notes", "open"), ("Screen Lock", "open"),
                ("Stopwatch", "open"), ("Other Apps", "open"),
                ("Demos", "open"), ("Files", "open"), ("System", "open")],
     [4]),
    ("settings", [("About", "open"), ("Shuffle", "cycle:3"),
                  ("Repeat", "cycle:3"),
                  ("Main Menu", "open"), ("Music Menu", "open"),
                  ("Volume Limit", "open"), ("Backlight", "open"),
                  ("Brightness", "open"), ("Audiobooks", "open"),
                  ("EQ", "open"), ("Sound Check", "cycle:2"),
                  ("Clicker", "cycle:2"), ("Date & Time", "menu"),
                  ("Sort By", "cycle:2"), ("Language", "open"),
                  ("Legal", "open"), ("Reset Settings", "open"),
                  ("Advanced Settings", "open")], [5]),
    ("date_time", [("Date", "open"), ("Time", "open"), ("Time Zone", "open"),
                   ("24 Hour Clock", "cycle:2"), ("Time in Title", "cycle:2")],
     [5, 12]),
    ("main", [("Music", "menu"), ("Videos", "menu"), ("Photos", "menu"),
              ("Podcasts", "empty"), ("Extras", "menu"), ("Settings", "menu"),
              ("Shuffle Songs", "play")], []),
]


def seed(root):
    """Six songs on three albums, so the library rows have a database."""
    for artist, album in (("Alpha Artist", "Alpha Album"),
                          ("Beta Artist", "Beta Album"),
                          ("Gamma Artist", "Gamma Album")):
        folder = root / "Music" / artist
        folder.mkdir(parents=True)
        for number in (1, 2):
            subprocess.run([
                "ffmpeg", "-nostdin", "-v", "error", "-f", "lavfi", "-i",
                "sine=frequency=440:duration=20:sample_rate=44100",
                "-c:a", "flac", "-metadata", f"artist={artist}",
                "-metadata", f"album_artist={artist}",
                "-metadata", f"album={album}",
                "-metadata", f"title={artist.split()[0]} Song {number}",
                "-metadata", f"track={number}", "-metadata", "genre=Test",
                str(folder / f"{number}.flac")], check=True)


def settle(s, frames=40):
    """The screen once its list half has stopped changing."""
    last = s.frame()
    for _ in range(frames):
        time.sleep(6 / FPS)
        frame = s.frame()
        if same(last, frame):
            return frame
        last = frame
    return last


# The list half of the screen below the status bar. The pane beside it
# animates on its own, and the status bar follows playback.
LIST = (0, 22, 160, 240)


def same(a, b):
    return not ImageChops.difference(a.crop(LIST), b.crop(LIST)).getbbox()


def press(s, key, tries=4):
    """Tap `key` until the list half changes: the simulator drops a wheel
    event now and then, most often the first after Select or Menu."""
    before = settle(s, 20)
    for _ in range(tries):
        s.tap(key)
        for _ in range(45):
            time.sleep(4 / FPS)
            if not same(before, s.frame()):
                return settle(s)
    raise RuntimeError(f"{key} changed nothing")


def back_at(s, menu, seconds=3):
    """Whether the screen comes back to `menu` within `seconds`."""
    for _ in range(int(seconds * FPS / 4)):
        if same(s.frame(), menu):
            return True
        time.sleep(4 / FPS)
    return False


def walk(s, out, result):
    for name, rows, path in TREE:
        for step in path:
            for _ in range(step):
                press(s, "Down")
            press(s, "Return")
        for i, (label, kind) in enumerate(rows):
            menu = press(s, "Down") if i else settle(s)
            key = f"{name}/{label}"
            shot = out / (f"{name}-{i:02d}-" +
                          label.lower().replace(" ", "_").replace("&", "and")
                          + ".png")
            if kind == "menu":
                menu.save(shot)
                result[key] = "menu"
                continue
            if kind.startswith("cycle:"):
                steps = [press(s, "Return")
                         for _ in range(int(kind.split(":")[1]))]
                steps[0].save(shot)
                returned = same(steps[-1], menu)
                ok = returned and not any(same(f, menu) for f in steps[:-1])
                result[key] = {"kind": kind, "returned": returned, "ok": ok}
                (out / "result.json").write_text(json.dumps(result, indent=1))
                continue
            s.tap("Return")
            for _ in range(20):
                time.sleep(6 / FPS)
                opened = s.frame()
                if not same(menu, opened):
                    break
            opened.save(shot)
            changed = not same(menu, opened)
            back = 0
            # A splash goes by itself, and Back during one would leave the
            # menu: after Select and after each Back, give it its time
            while kind != "play" and not back_at(s, menu) and back < 4:
                s.tap("Escape")
                back += 1
            returned = same(settle(s), menu)
            if kind == "play":
                ok = changed
            else:
                ok = returned and (changed if kind == "open" else
                                   not changed)
            result[key] = {"kind": kind, "changed": changed,
                           "returned": returned, "back_presses": back,
                           "ok": ok}
            (out / "result.json").write_text(json.dumps(result, indent=1))
        if not path:
            continue
        for _ in rows[1:]:
            press(s, "Up")
        for step in reversed(path):
            press(s, "Escape")
            for _ in range(step):
                press(s, "Up")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("binary", "simdisk", "out"):
        parser.add_argument(name, type=lambda p: Path(p).resolve())
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    result = {}
    with tempfile.TemporaryDirectory(prefix="fruit-tree-") as temp, \
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
            "tagcache_scan_on_startup: on\n")
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
                settle(s)
                walk(s, args.out, result)
                assert firmware.poll() is None, "simulator exited"
    (args.out / "result.json").write_text(json.dumps(result, indent=1))
    failed = [k for k, v in result.items() if v != "menu" and not v["ok"]]
    for key in failed:
        print("FAIL", key, result[key])
    print(f"{len(result) - len(failed)} of {len(result)} rows pass")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
