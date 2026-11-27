#!/usr/bin/env python3
"""Check that every page change in the stock themes slides, in SDL.

Walks the stock and AstraPod menu trees with scripted keys, one fresh simulator per row. PODBOX_PANEL_RECORD
captures every picture the panel was sent. A screen that
calls lcd_update() itself never reaches that record, so where the panel
shows no change the pictures the window showed (PODBOX_UI_WALK_STATE)
are read instead. A step
that changes the page passes when the page moves: at least MOVING pictures
between the page before and the page after match one of them better moved
sideways ("slide") or up or down ("sheet") than where it stands. A cut is
the window going from one page to the next with no such picture between.

Arguments: ROCKBOXUI FIXTURE OUTPUT_DIRECTORY [--theme NAME] [--only PATH]
FIXTURE is a simulator root with a built database, as ui_music_fixture
makes, plus the bundle's .rockbox/themes, sample photos and a movie. No window opens: SDL draws to its
dummy driver. Writes result.json and, per step, a strip of what the panel
showed to the output directory.
"""

import argparse
import json
import mmap
import os
import re
import shutil
import signal
import struct
import subprocess
import tempfile
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

from app_list_test import APP_LIST, phrases

# PODBOX_PANEL_RECORD: per submission, a header and the panel's picture.
RECORD = struct.Struct("<4sIcBhhhI32h")
PANEL = 320 * 240 * 2
RECORD_SIZE = RECORD.size + PANEL
# PODBOX_UI_WALK_STATE: a ring of the last FRAMES pictures the window
# showed, each with its sequence number.
HEADER = struct.Struct("=III")
WORD = struct.Struct("=I")
PIXELS = 320 * 240 * 3
FRAMES = 8
STATE = HEADER.size + FRAMES * (WORD.size + PIXELS)
STATUS = 22
ROUTE = re.compile(r"route=(\S+) seen=(\d+)")
MOVING = 3
# A picture is in motion where this share of its pixels unlike both pages
# is one of them moved. The page underneath is dimmed, so it never matches.
SHIFTED = 0.15
QUIET = 0.32
SETTLE_LIMIT = 1.4

CONFIG = """\
start in screen: root
backlight timeout: off
idle poweroff: off
sleep timer on startup: off
voice menus: off
keyclick: off
animations: on
font: /.rockbox/fonts/18-noto-sans.pbf
font bold: /.rockbox/fonts/18-noto-sans-bold.pbf
"""


class Sim:
    """The simulator, every picture the panel was sent, and every distinct
    picture its window showed."""

    def __init__(self, binary, root, out, stem, row):
        self.row = row
        self.step = "startup"
        path = root / "walk.state"
        with path.open("w+b") as handle:
            handle.truncate(STATE)
            self.state = mmap.mmap(handle.fileno(), STATE,
                                   access=mmap.ACCESS_READ)
        env = dict(os.environ, SDL_VIDEODRIVER="dummy",
                   SDL_AUDIODRIVER="dummy", SDL_RENDER_DRIVER="software",
                   PODBOX_UI_WALK_STATE=str(path),
                   PODBOX_PANEL_RECORD=str(root / "panel.rec"))
        self.record = root / "panel.rec"
        self.perf = root / ".rockbox/ui-perf.txt"
        env.pop("LD_PRELOAD", None)
        self.log = (out / f"{stem}.log").open("wb")
        self.process = subprocess.Popen(
            [str(binary), "--root", ".", "--nobackground", "--zoom", "1",
             "--screenshot", "--keys", "-"],
            cwd=root, env=env, stdin=subprocess.PIPE, stdout=self.log,
            stderr=subprocess.STDOUT, bufsize=0)
        self.seen = []
        self.lost = 0
        self.running = True
        self.poller = threading.Thread(target=self.poll, daemon=True)
        self.poller.start()

    def poll(self):
        last, previous = 0, None
        while self.running:
            _, captured, _ = HEADER.unpack_from(self.state)
            if captured - last > FRAMES:
                self.lost += captured - last - FRAMES
                last = captured - FRAMES
            for seq in range(last + 1, captured + 1):
                at = HEADER.size + (seq - 1) % FRAMES * (WORD.size + PIXELS)
                pixels = self.state[at + WORD.size:at + WORD.size + PIXELS]
                if WORD.unpack_from(self.state, at)[0] != seq:
                    continue
                if pixels != previous:
                    previous = pixels
                    self.seen.append((time.monotonic(), pixels))
            last = captured
            time.sleep(0.002)

    def count(self):
        size = self.record.stat().st_size if self.record.is_file() else 0
        return size // RECORD_SIZE, len(self.seen)

    def frames(self, start, end):
        """The panel's pictures and the window's between two counts."""
        panel = []
        with self.record.open("rb") as handle:
            handle.seek(start[0] * RECORD_SIZE)
            for _ in range(start[0], end[0]):
                head = handle.read(RECORD.size)
                body = handle.read(PANEL)
                if len(body) < PANEL:
                    break
                panel.append((RECORD.unpack(head)[1] / 1e6,
                              np.frombuffer(body, "<u2").reshape(240, 320)))
        window = [(t, np.frombuffer(p, np.uint8).reshape(240, 320, 3))
                  for t, p in self.seen[start[1]:end[1]]]
        return panel, window

    def routes(self, since):
        """The scene routes that ended since the perf log had `since` bytes,
        each with the frames it presented, and the log's length now."""
        if not self.perf.is_file():
            return [], since
        text = self.perf.read_bytes()
        found = ROUTE.findall(text[since:].decode("utf-8", "replace"))
        return [(name, int(seen)) for name, seen in found], len(text)

    def alive(self):
        status = self.process.poll()
        if status is not None:
            reason = signal.Signals(-status).name if status < 0 else f"exit {status}"
            raise RuntimeError(f"{self.row}, {self.step}: the simulator stopped "
                               f"({reason}); see {self.log.name}")

    def key(self, name, hold=2):
        """Press and release. The script's own `wait` counts presents, and
        a still page presents nothing, so the host times the release."""
        self.alive()
        try:
            self.process.stdin.write(f"down {name}\n".encode())
            self.process.stdin.flush()
            time.sleep(hold / 60)
            self.process.stdin.write(f"up {name}\n".encode())
            self.process.stdin.flush()
        except BrokenPipeError as err:
            self.alive()
            raise RuntimeError(f"{self.row}, {self.step}: simulator key pipe "
                               f"closed; see {self.log.name}") from err

    def settle(self, quiet=QUIET, limit=SETTLE_LIMIT):
        """Wait until the window has shown nothing new for `quiet`
        seconds."""
        end = time.monotonic() + limit
        last, since = self.count(), time.monotonic()
        while time.monotonic() < end:
            time.sleep(0.05)
            now = self.count()
            if now[1] != last[1]:
                since = time.monotonic()
            last = now
            if time.monotonic() - since >= quiet:
                break
        self.alive()
        return last

    def close(self):
        self.running = False
        self.poller.join()
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(4)
            except subprocess.TimeoutExpired:
                self.process.kill()
        self.log.close()
        self.state.close()


def packed(frame):
    """Every third row below the status bar, one integer a pixel."""
    if frame.ndim == 3:
        f = frame.astype(np.uint32)
        frame = f[..., 0] << 16 | f[..., 1] << 8 | f[..., 2]
    return frame[STATUS::3]


def explained(frame, old, new):
    """The share of the pixels unlike both pages that one of them, moved
    sideways or up or down by at least 4 pixels, puts there; and whether
    the best move is sideways."""
    unlike = (frame != old) & (frame != new)
    total = int(unlike.sum())
    if total < 100:
        return 0.0, True
    best, across = 0.0, True
    for axis in (1, 0):
        size = frame.shape[axis]
        for d in range(4 if axis else 2, size - 8, 2 if axis else 1):
            for page in (old, new):
                for sign in (1, -1):
                    here = slice(d, None) if sign > 0 else slice(None, -d)
                    there = slice(None, -d) if sign > 0 else slice(d, None)
                    if axis:
                        hits = (frame[:, here] == page[:, there]) & \
                            unlike[:, here]
                    else:
                        hits = (frame[here] == page[there]) & unlike[here]
                    share = int(hits.sum()) / total
                    if share > best:
                        best, across = share, bool(axis)
    return best, across


def apart(a, b):
    return float(np.mean(a != b))


def image(frame):
    if frame.ndim == 3:
        return Image.fromarray(frame, "RGB")
    return Image.frombytes("RGB", (320, 240), frame.tobytes(), "raw",
                           "BGR;16")


def classify(before, frames):
    """'same', 'slide' (sideways), 'sheet' (up or down) or 'cut', and the
    pictures in motion between the pages."""
    if not frames:
        return "same", 0, 0
    old, new = packed(before), packed(frames[-1][1])
    if apart(old, new) < 0.005:
        return "same", 0, 0
    sideways = upward = 0
    times = []
    for t, frame in frames[:-1]:
        f = packed(frame)
        if apart(f, old) < 0.005 or apart(f, new) < 0.005:
            continue
        share, across = explained(f, old, new)
        if share < SHIFTED:
            continue
        times.append(t)
        if across:
            sideways += 1
        else:
            upward += 1
    moving = sideways + upward
    span = int((times[-1] - times[0]) * 1000) if times else 0
    if moving < MOVING:
        return "cut", moving, span
    return ("slide" if sideways >= upward else "sheet"), moving, span


def strip(frames, path):
    shots = frames[:: max(1, len(frames) // 24)][:24]
    sheet = Image.new("RGB", (160 * min(8, len(shots)),
                              120 * ((len(shots) + 7) // 8)))
    for i, shot in enumerate(shots):
        sheet.paste(image(shot).resize((160, 120)),
                    (160 * (i % 8), 120 * (i // 8)))
    sheet.save(path)


# A step is (kind, key, name). "slide" must slide; "any" is recorded and
# not checked; "nav" only gets to where the checks are.
def into(name, depth, leave="back"):
    """Select `depth` levels down from a list page, and back up by
    `leave`."""
    steps = [("slide", "select", f"{name}{'/item' * (d + 1)} >")
             for d in range(depth)]
    steps += [("slide", leave, f"{name}{'/item' * (d + 1)} <")
              for d in reversed(range(depth))]
    return steps


def down(n):
    return [("nav", "down", "")] * n


def row(label, kind="page", inner=(), back="menu", pre=None):
    """A menu row: kind "menu" has `inner` as its rows, "page" runs the
    steps `inner` on the page it opens, "any" opens and leaves without a
    check, "none" is a row Select does nothing on. `pre` reaches the row in
    place of the wheel turns its position needs."""
    return (label, kind, list(inner), back, pre)


def segments(rows, path="", reach=()):
    """One walk per row, each from a fresh start: (name, steps)."""
    out = []
    for i, (label, kind, inner, back, pre) in enumerate(rows):
        name = f"{path}/{label}" if path else label
        nav = list(reach) + (pre if pre is not None else down(i))
        if kind == "none":
            out.append((name, nav + [("any", "select", name)]))
            continue
        if kind == "raw":
            out.append((name, nav + [("slide", "select", name + " >")] +
                        inner))
            continue
        check = "any" if kind == "any" else "slide"
        body = [] if kind == "menu" else inner
        out.append((name, nav + [(check, "select", name + " >")] + body +
                    [(check, back, name + " <")]))
        if kind == "menu":
            out += segments(inner, name, nav + [("nav", "select", "")])
    return out


def levels(name, labels, leave="back"):
    """Select down through list levels `labels` (first row each time) and
    back up by `leave`, one level per press."""
    path = name
    fwd, back = [], []
    for label in labels:
        path = f"{path}/{label}"
        fwd.append(("slide", "select", f"{path} >"))
        back.insert(0, ("slide", leave, f"{path} <"))
    return fwd + back


def nav_to(labels):
    """Unchecked selects down through list levels."""
    return [("nav", "select", "")] * len(labels)


def play_from(name, labels):
    """Reach the song list unchecked, Select its first song (into Now
    Playing), then Menu out of Now Playing."""
    return nav_to(labels) + [
        ("slide", "select", f"{name}/{'/'.join(labels)}/song > NowPlaying"),
        ("slide", "menu", f"{name}/{'/'.join(labels)}/NowPlaying <"),
        ("any", "menu", f"{name}/{'/'.join(labels)}/NowPlaying << again")]


def menu_from(name, labels):
    """Reach the deepest list unchecked and press Menu once."""
    return nav_to(labels[:-1]) + [
        ("slide", "select", f"{name}/{'/'.join(labels)} >"),
        ("slide", "menu", f"{name}/{'/'.join(labels)} <menu")]


# Rows of the Music menu: label, position, list levels below the row's own
# page down to the song list (the row's page is level 0).
MUSIC_LEVELS = [
    ("Artists", 2, ["artist", "album"]),
    ("Albums", 3, ["album"]),
    ("Songs", 5, []),
    ("Genres", 6, ["genre", "albumartist", "album"]),
    ("Composers", 7, ["composer", "album"]),
    ("Audiobooks", 8, ["book"]),
]

MUSIC = [
    # Playlists opens on its first playlist, below its Search row
    row("Playlists", inner=levels("Music/Playlists", ["playlist"]),
        pre=down(1)),
    row("Playlists, play", "raw", play_from("Music/Playlists", ["playlist"]),
        pre=down(1)),
    row("Playlists, Menu from deepest", "raw",
        menu_from("Music/Playlists", ["playlist"]), pre=down(1)),
    row("Cover Flow", pre=down(0)),
    row("Cover Flow, select album", "raw",
        [("any", "select", "Music/Cover Flow/album >"),
         ("any", "menu", "Music/Cover Flow/album <"),
         ("slide", "menu", "Music/Cover Flow, select album <")],
        pre=down(0)),
    row("Compilations", "none", pre=down(4)),
    row("Search", pre=down(9)),
]
for label, at, below in MUSIC_LEVELS:
    # An audiobook's first level is the Book Shelf, a screen of the main
    # menu's: Back from it is back to Home, past the Audiobooks list.
    shelf = label == "Audiobooks"
    MUSIC.append(row(label, "raw" if shelf else "page",
                     levels(f"Music/{label}", below), pre=down(at)))
    MUSIC.append(row(f"{label}, play", "raw",
                     play_from(f"Music/{label}", below), pre=down(at)))
    if below:
        MUSIC.append(row(f"{label}, Menu from deepest", "raw",
                         menu_from(f"Music/{label}", below), pre=down(at)))

FRUIT_TREE = [
    row("Music", "menu", MUSIC),
    row("Videos", "menu", [
        row("Movies"), row("Rentals", "none", pre=down(1)),
        row("TV Shows", "none", pre=down(2)),
        row("Music Videos", "none", pre=down(3)),
        row("Video Playlists", "none", pre=down(4)),
        row("Settings", "any", pre=down(5))]),
    row("Photos", "menu", [
        # The Images list opens on its first picture; Menu leaves the viewer
        row("All Photos", inner=levels("Photos/All Photos", ["photo"],
                                       leave="menu")),
        row("Settings", "any", pre=down(1))]),
    row("Podcasts", inner=levels("Podcasts", ["show"]), pre=down(3)),
    row("Podcasts, play", "raw", play_from("Podcasts", ["show"]),
        pre=down(3)),
    row("Podcasts, Menu from deepest", "raw",
        menu_from("Podcasts", ["show"]), pre=down(3)),
    row("Shuffle Songs", pre=down(6)),
    # Playing, the main menu gains Now Playing below Shuffle Songs
    row("Now Playing", pre=down(6) + [("nav", "select", ""),
                                      ("nav", "menu", "")] + down(1)),
]

EXTRAS = [
    row("Clocks"),
    row("Calendars", "menu", [
        row("All", inner=levels("Extras/Calendars/All", ["Day"], "menu")),
        row("To Do's"), row("Alarms", "none"),
    ]),
    row("Contacts", "none"),
    row("Alarms", "menu", [
        row("New Alarm", "menu", [
            row("Alarm", "none"), row("Date"), row("Time"), row("Repeat"),
            row("Sound"), row("Label"), row("Delete", "any"),
        ]),
        row("Sleep Timer"),
    ]),
    row("Games", "menu", [row("2048", inner=down(4), back="select")]),
    row("Notes", inner=levels("Extras/Notes", ["Instructions"], "menu")),
    row("Screen Lock"), row("Stopwatch"), row("Other Apps"), row("Demos"),
    row("Files"), row("System", "menu", [
        row("Recording", "any"), row("About"), row("Rockbox Info"),
        row("Credits"), row("Background Tasks"), row("License"),
    ]),
]

SETTINGS = [
    row("About"), row("Shuffle", "none"), row("Repeat", "none"),
    row("Main Menu"), row("Music Menu"), row("Volume Limit"),
    row("Backlight"), row("Brightness"), row("Audiobooks"), row("EQ"),
    row("Sound Check", "none"), row("Clicker", "none"),
    row("Date & Time", "menu", [row("Date"), row("Time"),
         row("Time Zone", "any"), row("24 Hour Clock", "none"),
         row("Time in Title", "any")]),
    row("Sort By", "any"), row("Language"), row("Legal"),
    row("Reset Settings"), row("Advanced Settings"),
]

FRUIT_TREE += [row("Extras", "menu", EXTRAS, pre=down(4)),
               row("Settings", "menu", SETTINGS, pre=down(5))]

SYSTEM_SETTINGS = [
    row("USB"), row("Accessories"), row("Shortcuts Replaces Quickscreen"),
    row("Keyclick"), row("Limits"), row("Volume Adjustment Mode"),
    row("Volume Adjustment Steps"), row("Time & Date"),
    row("Startup & Shutdown"), row("Language and Text"), row("Settings Files"),
]

NATIVE_SETTINGS = [
    row("Search", pre=[("nav", "up", "")]), row("Sound", back="back", pre=[]),
    row("Sound, Menu to Home", pre=[]),
    row("Playback", back="back", pre=down(1)),
    row("Recording", back="back", pre=down(2)),
    row("Library", "menu", [
        row("Files", back="back"), row("Music", back="back"),
        row("Search", back="back"), row("Album Covers", back="back"),
        row("Database", back="back"), row("Artwork Cache", back="back"), row("Playlist Engine", back="back"),
        row("Viewers", back="back"), row("Maintenance", back="back"),
    ], back="back", pre=down(3)),
    row("Theme", pre=down(4)), row("Appearance", pre=down(5)),
    row("Battery & Power", pre=down(6)),
    row("System", "menu", SYSTEM_SETTINGS, back="back", pre=down(7)),
    row("Settings Mode", pre=down(8)), row("Changed Settings", pre=down(9)),
]

# The configured root order below fixes row positions. Start playing from
# Music on a separate walk so conditional Now Playing rows do not shift them.
NATIVE_TREE = [
    row("Music", back="back"),
    row("Audiobooks", back="back"),
    row("Podcasts", inner=levels("Podcasts", ["show"]), back="back"),
    row("Cover Flow"),
    row("Playlists", inner=levels("Playlists", ["playlist"]), back="back"),
    row("Files"), row("Photos", back="back"),
    row("Videos", "any", back="back"),
    row("Plugins", "menu", [row("Applications", back="back"),
        row("Games", back="back"), row("Demos", back="back")], back="back"),
    row("Settings", "menu", NATIVE_SETTINGS, back="back"),
    row("System", "menu", [
        row("Recording", "any"), row("About"), row("Rockbox Info", back="back"),
        row("Credits"), row("Background Tasks", back="back"), row("License"),
    ], back="back"),
]
NATIVE_ORDER = ("root menu order: database,pictureflow,playlists,files,images,"
                "videos,plugins,settings,system_menu,!wps\n"
                "settings mode: everything\n")
TREES = {"fruit-company": FRUIT_TREE, "classic": NATIVE_TREE,
         "scrim": NATIVE_TREE}



def games(fixture):
    """Map each installed game's display name to the module the row runs."""
    english = phrases("english.lang")
    table = re.findall(r'\{ "([^"]+)",\s*(LANG_\w+), APP_LIST_GAMES \}',
                       APP_LIST.read_text())
    modules = {english[lang]: module for module, lang in table
               if (fixture / ".rockbox/rocks" / (module + ".rock")).is_file()}
    quiz = english["LANG_MUSIC_QUIZ"]
    modules[quiz] = None
    rows = []
    quit_at_end = {"Amaze", "Blackjack", "Brickmania", "Bubbles",
                   "Chopper", "Clix", "Jewels"}
    for name, module in sorted(modules.items(),
                               key=lambda item: item[0].casefold()):
        if name == "2048":
            rows.append(row(name, inner=down(4), back="select", pre=[]))
        elif (name in quit_at_end or
              (module and module.startswith("games/sgt_puzzles/"))):
            rows.append(row(name, inner=[("nav", "up", "")],
                            back="select", pre=[]))
        else:
            rows.append(row(name, pre=[]))
    return rows, modules


def run_walk(sim, out, walk, stem):
    results = []
    perf = 0
    for index, (kind, key, name) in enumerate(walk, start=1):
        sim.step = f"step {index} ({name or kind}, key={key})"
        if kind == "nav":
            sim.key(key)
            if key in ("up", "down"):
                sim.settle(quiet=0.08, limit=0.22)
            else:
                sim.settle(quiet=0.45)
            continue
        start = sim.settle()
        _, perf = sim.routes(perf)
        sim.key(key)
        time.sleep(0.1)
        end = sim.settle()
        routes, perf = sim.routes(perf)
        last = (start[0] - 1, start[1] - 1)
        panel_before, window_before = sim.frames(last, start)
        panel, window = sim.frames(start, end)
        if not panel_before or not window_before:
            raise RuntimeError(f"{name}: missing initial LCD frame")
        before, frames = panel_before[0][1], panel
        verdict, between, span = classify(before, frames)
        # A screen that updates the LCD itself is only in the window
        if verdict in ("same", "cut") and window:
            alt_before = window_before[0][1]
            alt = classify(alt_before, window)
            if alt[1] >= between:
                before, frames = alt_before, window
                verdict, between, span = alt
        shot = f"{stem}-{len(results)}"
        strip([before] + [f for _, f in frames], out / f"{shot}.png")
        image(frames[-1][1] if frames else before).save(
            out / f"{shot}-end.png")
        # A panel submission is stronger evidence than pixel matching when
        # a page has few distinct pixels or its overlay exactly restores it.
        scene_frames = max((seen for route, seen in routes
                            if route in ("push", "pop") or
                            route.endswith(("-push", "-pop")) or
                            route.startswith(("picker-", "keyboard-",
                                              "dialog-"))), default=0)
        if verdict in ("same", "cut") and scene_frames >= MOVING:
            verdict = "scene"
        ok = kind != "slide" or verdict in ("slide", "sheet", "scene")
        if name.startswith("Extras/Games/") and "Music Quiz" not in name:
            game_route = "app-push" if name.endswith(" >") else "app-pop"
            ok = any(route == game_route and seen >= MOVING
                     for route, seen in routes)
        results.append({"step": name, "key": key, "expect": kind,
                        "verdict": verdict, "between": between,
                        "ms": span, "frames": len(frames), "ok": ok,
                        "routes": routes,
                        "strip": shot})
    return results


def contact(out, theme, results):
    """Where each checked step ended, labelled, to read the walk by."""
    cells = [(r["step"], r["verdict"],
              Image.open(out / f"{r['strip']}-end.png").resize((160, 120)))
             for r in results]
    sheet = Image.new("RGB", (160 * 8, 134 * ((len(cells) + 7) // 8)),
                      "white")
    draw = ImageDraw.Draw(sheet)
    for i, (name, verdict, shot) in enumerate(cells):
        x, y = 160 * (i % 8), 134 * (i // 8)
        sheet.paste(shot, (x, y))
        draw.text((x + 2, y + 121), f"{verdict} {name}"[-30:],
                  fill="red" if verdict == "cut" else "black")
    sheet.save(out / f"{theme}-walk.png")


def walk_segment(binary, fixture, out, theme, index, name, steps, game=None):
    with tempfile.TemporaryDirectory(prefix="stock-slide-") as temp:
        root = Path(temp) / "root"
        shutil.copytree(fixture, root, symlinks=True,
                        ignore=shutil.ignore_patterns(
                            "panel.rec", "walk.state", "ui-perf.txt",
                            ".resume.cfg*", "config.cfg.old", "config.cfg.new"))
        if game is not None:
            # Leave assets and overlays available. Omit other foreground
            # modules from this private copy so wheel counts cannot skip a row.
            rocks = root / ".rockbox/rocks"
            target = rocks / (game + ".rock")
            if game != "@quiz":
                # The built-in quiz cannot be omitted. Sort it after every
                # installed game so the target always starts selected.
                langs = root / ".rockbox/langs"
                langs.mkdir(exist_ok=True)
                (langs / "overrides.cfg").write_text(
                    "Music Quiz: ~ Music Quiz\n", encoding="utf-8")
            for other in (rocks / "games").rglob("*.rock"):
                if other != target:
                    other.unlink()
        extra = "" if theme == "fruit-company" else NATIVE_ORDER
        (root / ".rockbox/config.cfg").write_text(
            CONFIG + f"native theme: {theme}\n" + extra)
        (root / ".rockbox/ui-perf.enable").touch()
        stem = f"{theme}-{index:02d}"
        sim = Sim(binary, root, out, stem, name)
        try:
            time.sleep(2)
            sim.settle(quiet=1.5, limit=20)
            return run_walk(sim, out, steps, stem)
        finally:
            sim.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=lambda p: Path(p).resolve())
    parser.add_argument("fixture", type=lambda p: Path(p).resolve())
    parser.add_argument("out", type=lambda p: Path(p).resolve())
    parser.add_argument("--theme", action="append", choices=TREES)
    parser.add_argument("--only", help="walk only rows whose name has this")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--all-games", action="store_true",
                        help="walk every installed game in an isolated fixture")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    game_modules = {}
    if args.all_games:
        game_rows, game_modules = games(args.fixture)
        EXTRAS[:] = [row("Games", "menu", game_rows) if item[0] == "Games"
                     else item for item in EXTRAS]
        FRUIT_TREE[-2] = row("Extras", "menu", EXTRAS, pre=down(4))
    def module_for(name):
        parent, _, label = name.rpartition("/")
        if parent == "Extras/Games" and label in game_modules:
            return game_modules[label] or "@quiz"
        return None

    result = {}
    for theme in args.theme or list(TREES):
        walks = [(name, steps) for name, steps in segments(TREES[theme])
                 if not args.only or args.only in name]
        result[theme] = []
        with ThreadPoolExecutor(args.jobs) as pool:
            runs = pool.map(
                lambda job: walk_segment(args.binary, args.fixture, args.out,
                                         theme, *job),
                [(i, name, steps, module_for(name))
                 for i, (name, steps) in enumerate(walks)])
            for rows in runs:
                result[theme].extend(rows)
                for r in rows:
                    print(f"{theme:14s} {r['step']:44s} {r['verdict']:5s} "
                          f"between={r['between']:3d} {r['ms']:4d}ms "
                          f"{'ok' if r['ok'] else 'FAIL'}", flush=True)
        if result[theme]:
            contact(args.out, theme, result[theme])
    (args.out / "result.json").write_text(json.dumps(result, indent=1))
    failed = [(t, r["step"]) for t, rows in result.items() for r in rows
              if not r["ok"]]
    for theme, step in failed:
        print("FAIL", theme, step)
    total = sum(len(rows) for rows in result.values())
    print(f"{total - len(failed)} of {total} steps pass")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
