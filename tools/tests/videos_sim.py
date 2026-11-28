#!/usr/bin/env python3
"""Videos on the main menu, its list, and films opening in the video player.

Drives a built ipod6g simulator headless (SDL dummy video) with generated
loose media and an indexed movie:

    python3 tools/tests/videos_sim.py build-sim-ipod6g EVIDENCE_DIR

The Videos list must show the .m4v and the .mp4 with a picture track and not
the .mp4 holding only sound. Selecting a film must start the video player,
which in the simulator says it needs the iPod's decoder; selecting the song
must play it. Needs ffmpeg and tesseract. Screens are saved as PNGs in
EVIDENCE_DIR.
"""
import argparse
from contextlib import contextmanager
import mmap
import os
from pathlib import Path
import shutil
import struct
import subprocess
import time

from PIL import Image, ImageOps

from coverflow_back_stack_sim import prepare_ultra

LCD = (320, 240)
FRAME_BYTES = LCD[0] * LCD[1] * 3
STATE_BYTES = 12 + 8 * (4 + FRAME_BYTES)


def ffmpeg(*args):
    subprocess.run(["ffmpeg", "-v", "error", "-y", *map(str, args)], check=True)


def media(disk):
    films, music = disk / "Movies", disk / "Music"
    films.mkdir(parents=True)
    music.mkdir()
    ffmpeg("-f", "lavfi", "-i", "testsrc=s=320x240:r=25:d=3",
           "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=44100:duration=3",
           "-c:v", "libx264", "-profile:v", "baseline", "-level", "3.0",
           "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest",
           films / "film.mp4")
    shutil.copy(films / "film.mp4", films / "clip.m4v")
    ffmpeg("-f", "lavfi", "-i", "sine=frequency=440:sample_rate=44100:duration=20",
           "-c:a", "aac", music / "song.mp4")


def tagged(path, cover=None, **tags):
    """A loose film carrying MP4 tags and, given 'cover', a 'covr' image"""
    args = ["-f", "lavfi", "-i", "testsrc=s=320x240:r=25:d=3",
            "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=44100:duration=3"]
    maps = ["-map", "0:v", "-map", "1:a"]
    if cover:
        args += ["-i", cover]
        maps += ["-map", "2:v", "-c:v:1", "copy",
                 "-disposition:v:1", "attached_pic"]
    path.parent.mkdir(parents=True, exist_ok=True)
    meta = [arg for key, value in tags.items()
            for arg in ("-metadata", f"{key}={value}")]
    ffmpeg(*args, *maps, "-c:v:0", "libx264", "-profile:v:0", "baseline",
           "-pix_fmt:v:0", "yuv420p", "-c:a", "aac", *meta, path)


def loose_media(disk):
    """Loose films the MP4 media kind sorts into the Videos categories"""
    poster = disk / "poster.jpg"
    ffmpeg("-f", "lavfi", "-i", "testsrc2=s=200x300", "-frames:v", "1", poster)
    tagged(disk / "Movies/covered.mp4", poster, title="Covered Film",
           media_type=9)
    tagged(disk / "TV/episode.mp4", poster, title="Pilot", media_type=10,
           show="My Show", season_number=2, episode_sort=3, date="2026-03-08")
    # No show name: the album names it, and it makes My Show's second season
    tagged(disk / "TV/earlier.mp4", title="Before", media_type=10,
           album="My Show", season_number=1, episode_sort=1)
    tagged(disk / "Clips/band.mp4", title="Band Song", media_type=6,
           artist="Some Band")
    poster.unlink()


def rgb565(image, edge):
    data = image.convert("RGB").resize((edge, edge)).tobytes()
    return b"".join(struct.pack("<H", (r >> 3) << 11 | (g >> 2) << 5 | b >> 3)
                    for r, g, b in zip(data[0::3], data[1::3], data[2::3]))


def artwork_db(disk, covers):
    """An ArtworkDB and its .ithmb files: for each (dbid, colour) of
    'covers', a 320-pixel and a 56-pixel square of the colour"""
    def record(kind, header, body, fields):
        data = bytearray(header)
        struct.pack_into("<4sII", data, 0, kind, header, header + len(body))
        for offset, value in fields.items():
            struct.pack_into("<I", data, offset, value)
        return data + body

    folder = disk / "iPod_Control/Artwork"
    folder.mkdir(parents=True)
    files = {1060: bytearray(), 1061: bytearray()}
    images = b""
    for number, (dbid, colour) in enumerate(covers, 1):
        thumbs = b""
        for form, edge in ((1060, 320), (1061, 56)):
            offset = len(files[form])
            files[form] += rgb565(Image.new("RGB", (edge, edge), colour), edge)
            name = f":F{form}_1.ithmb".encode("utf-16le")
            location = record(b"mhod", 24, struct.pack("<IBBH", len(name), 2,
                                                       0, 0) + b"\0" * 4 +
                              name, {12: 3})
            thumbs += record(b"mhod", 24,
                             record(b"mhni", 76, location,
                                    {12: 1, 16: form, 20: offset,
                                     24: edge * edge * 2}), {12: 2})
        image = record(b"mhii", 152, thumbs, {12: 2, 16: 100 + number})
        struct.pack_into("<Q", image, 20, dbid)
        images += image
    listing = bytearray(92)
    struct.pack_into("<4sII", listing, 0, b"mhli", 92, len(covers))
    section = record(b"mhsd", 96, bytes(listing) + images, {12: 1})
    (folder / "ArtworkDB").write_bytes(record(b"mhfd", 132, section, {}))
    for form, data in files.items():
        (folder / f"F{form}_1.ithmb").write_bytes(data)


def itunes_db(disk, films, playlists=()):
    """An iTunesDB of 'films', each (title, media kind, length in ms) and
    optionally a dict of artist, show, season, episode and a cover's
    colour, all playing a copy of Movies/clip.m4v; and of 'playlists', each
    (name, [titles])"""
    def record(kind, header, body, fields):
        data = bytearray(header)
        struct.pack_into("<4sII", data, 0, kind, header, header + len(body))
        for offset, value in fields.items():
            struct.pack_into("<I", data, offset, value)
        return data + body

    def text(kind, value):
        encoded = value.encode("utf-16le")
        return record(b"mhod", 24,
                      struct.pack("<IIII", 1, len(encoded), 0, 0) + encoded,
                      {12: kind})

    def listing(kind, items, count):
        data = bytearray(12)
        struct.pack_into("<4sII", data, 0, kind, 12, count)
        return bytes(data) + items

    def playlist(name, ids, hidden):
        items = b"".join(record(b"mhip", 76, b"", {24: i}) for i in ids)
        return record(b"mhyp", 48, text(1, name) + items,
                      {12: 1, 16: len(ids), 20: hidden})

    control = disk / "iPod_Control"
    (control / "Music/F00").mkdir(parents=True)
    tracks, ids, covers = b"", {}, []
    for number, (title, kind, length, *more) in enumerate(films, 1):
        extra = more[0] if more else {}
        name = f"TEST{number}.m4v"
        ids.setdefault(title, number)
        shutil.copyfile(disk / "Movies/clip.m4v", control / "Music/F00" / name)
        body = text(1, title) + text(2, f":iPod_Control:Music:F00:{name}")
        if "artist" in extra:
            body += text(4, extra["artist"])
        if "show" in extra:
            body += text(19, extra["show"])
        tracks += record(b"mhit", 220, body,
                         {16: number, 40: length, 112: 0x1000 + number,
                          208: kind, 212: extra.get("season", 0),
                          216: extra.get("episode", 0)})
        if "cover" in extra:
            covers.append((0x1000 + number, extra["cover"]))
    sections = record(b"mhsd", 16, listing(b"mhlt", tracks, len(films)),
                      {12: 1})
    lists = playlist("Library", range(1, len(films) + 1), 1) + b"".join(
        playlist(name, [ids[t] for t in titles], 0)
        for name, titles in playlists)
    sections += record(b"mhsd", 16,
                       listing(b"mhlp", lists, 1 + len(playlists)), {12: 2})
    database = control / "iTunes/iTunesDB"
    database.parent.mkdir()
    database.write_bytes(record(b"mhbd", 12, sections, {}))
    if covers:
        artwork_db(disk, covers)


def indexed_movie(disk):
    itunes_db(disk, [("Indexed Film", 2, 3000)])


def player(build, disk, theme):
    """The built player's .rockbox, linked rather than copied"""
    def link(src, dst):
        try:
            os.link(src, dst)
        except OSError:
            shutil.copy2(src, dst)
    shutil.copytree(build / "simdisk/.rockbox", disk / ".rockbox",
                    copy_function=link,
                    ignore=shutil.ignore_patterns("library", "logs", "config.cfg"))
    config = (disk / ".rockbox/default-config.cfg").read_text()
    if theme == "fruit-company-ultra":
        prepare_ultra(disk)
    (disk / ".rockbox/config.cfg").write_text(
        config + f"\nnative theme: {theme}\nstart in screen: root\n"
        "root menu order: videos,files,settings\n"
        "animations: off\nbacklight timeout: off\nvoice menus: off\n"
        "volume: -24\n")


class Sim:
    def __init__(self, proc, state, case):
        self.proc = proc
        self.state = state
        self.case = case

    def tap(self, key):
        for action in ("down", "up"):
            self.proc.stdin.write(f"{action} {key}\n".encode())
            self.proc.stdin.flush()
            time.sleep(0.16 if action == "down" else 0.30)

    def image(self):
        sequence, = struct.unpack_from("=I", self.state, 4)
        if not sequence:
            return None
        offset = 12 + ((sequence - 1) % 8) * (FRAME_BYTES + 4)
        if struct.unpack_from("=I", self.state, offset)[0] != sequence:
            return None
        return Image.frombytes("RGB", LCD,
                               self.state[offset + 4:offset + 4 + FRAME_BYTES])

    def read(self, image):
        """The screen's text, the middle's and the title's on their own --
        a splash over a list reads poorly as part of the whole -- and the
        screen inverted, for light text on the selection bar"""
        text = ""
        for part, invert, psm, zoom in (
                (image, False, "11", 3),
                (image.crop((32, 92, 288, 146)), False, "11", 3),
                (image, True, "11", 3),
                (image.crop((0, 0, 240, 21)), False, "7", 4)):
            png = self.case / "ocr.png"
            gray = ImageOps.autocontrast(ImageOps.grayscale(part))
            if (gray.resize((1, 1)).getpixel((0, 0)) < 127) != invert:
                gray = ImageOps.invert(gray)
            gray.resize((part.width * zoom, part.height * zoom)).save(png)
            text += subprocess.check_output(
                ["tesseract", png, "stdout", "--psm", psm],
                stderr=subprocess.DEVNULL, text=True).lower() + "\n"
        return text

    def go(self, key, label, words, absent=()):
        """Press 'key' until the screen shows 'words': a key pressed while
        a screen is still opening can be lost"""
        for attempt in range(3):
            self.tap(key)
            try:
                return self.text(label, words, absent, timeout=6)
            except AssertionError:
                if attempt == 2:
                    raise

    def move(self, key, times=1):
        """Press 'key' 'times' times, each once the screen has answered the
        last: a press while a list is still settling can be lost"""
        for _ in range(times):
            for attempt in range(3):
                before = self.image()
                self.tap(key)
                deadline = time.monotonic() + 2
                while time.monotonic() < deadline:
                    after = self.image()
                    if after is not None and before is not None and \
                            after.crop((0, 21, 320, 240)).tobytes() != \
                            before.crop((0, 21, 320, 240)).tobytes():
                        break
                    time.sleep(0.1)
                else:
                    continue
                break
            else:
                raise AssertionError(f"{key} changed nothing")
            time.sleep(0.5)

    def step(self, key, label, words, absent=()):
        """Press 'key' once and wait for 'words': for a screen whose
        selected row reads poorly, where pressing again would go on"""
        self.tap(key)
        return self.text(label, words, absent)

    def text(self, label, words, absent=(), timeout=15):
        """Wait for a screen showing every word and none of 'absent'"""
        deadline = time.monotonic() + timeout
        last, image, value = None, None, ""
        while time.monotonic() < deadline:
            assert self.proc.poll() is None, self.case / "firmware.log"
            image = self.image()
            if image is not None and image.tobytes() != last:
                last = image.tobytes()
                value = self.read(image)
                if all(w in value for w in words) and \
                        not any(w in value for w in absent):
                    image.save(self.case / f"{label}.png")
                    time.sleep(1)   # a key too soon after a screen opens is lost
                    return
            time.sleep(0.1)
        if image is not None:
            image.save(self.case / f"failed-{label}.png")
        raise AssertionError(f"{label}: wanted {words}, not {absent}; "
                             f"read {value!r}")


# The library of the stock firmware's Movies captures: three films of one
# title, none with artwork
STOCK_FILMS = [("Test Pattern", 2, 3725000), ("Test Pattern", 2, 95000),
               ("Test Pattern", 2, 12000)]

# The library of the stock firmware's richer captures, two films with
# artwork
RICH_FILMS = [
    ("Alpha Movie", 2, 5520000, {"cover": (200, 40, 40)}),
    ("Beta Movie Without Artwork", 2, 32000),
    ("Delta Tall Poster", 2, 7380000),
    ("Gamma Movie Watched", 2, 5400000, {"cover": (40, 160, 50)}),
    ("Pilot", 64, 1500000, {"show": "Show One", "season": 1, "episode": 1}),
    ("Second Episode", 64, 1500000,
     {"show": "Show One", "season": 1, "episode": 2}),
    ("Season Two Opener", 64, 1500000,
     {"show": "Show One", "season": 2, "episode": 1}),
    ("Only Episode", 64, 1500000,
     {"show": "Show Two", "season": 1, "episode": 1}),
    ("Video One", 32, 215000, {"artist": "Band A"}),
    ("Video Two", 32, 200000, {"artist": "Band A"}),
    ("Clip", 32, 180000, {"artist": "Band B"}),
]
RICH_PLAYLISTS = [("Road Trip", ["Alpha Movie", "Video One", "Pilot"])]


@contextmanager
def session(build, case, theme="scrim", library=None, playlists=()):
    disk = case / "disk"
    disk.mkdir(parents=True)
    player(build, disk, theme)
    media(disk)
    if library:
        itunes_db(disk, library, playlists)
        shutil.rmtree(disk / "Movies")
    elif theme != "scrim":
        indexed_movie(disk)
        loose_media(disk)
    state_path = case / "state.bin"
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy",
               SDL_RENDER_DRIVER="software", PODBOX_UI_WALK_STATE=str(state_path))
    with state_path.open("w+b") as handle, \
            (case / "firmware.log").open("wb") as log:
        handle.truncate(STATE_BYTES)
        with mmap.mmap(handle.fileno(), 0, access=mmap.ACCESS_READ) as state, \
                subprocess.Popen([
                    build / "rockboxui", "--root", ".", "--nobackground",
                    "--zoom", "1", "--screenshot", "--keys", "-",
                ], stdin=subprocess.PIPE, env=env, cwd=disk, stdout=log,
                    stderr=subprocess.STDOUT) as proc:
            try:
                yield Sim(proc, state, case)
            finally:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait(timeout=5)


def empty_menu_rows(sim):
    for name, steps in (("rentals", 1), ("video playlists", 3)):
        label = name.replace(" ", "-")
        sim.move("down", steps)
        sim.text(label, [name])
        before = Image.open(sim.case / f"{label}.png")
        sim.tap("select")
        after = sim.image()
        # Ignore the status bar, whose battery animation runs independently.
        content = (0, 20, 160, 240)
        assert after and before.crop(content).tobytes() == \
            after.crop(content).tobytes(), f"SELECT opened empty {name}"


def line_text(case, png, box, label):
    with Image.open(case / png) as screen:
        line = ImageOps.autocontrast(ImageOps.grayscale(screen.crop(box)))
    if line.resize((1, 1)).getpixel((0, 0)) < 127:
        line = ImageOps.invert(line)
    path = case / f"{label}.png"
    line.resize((line.width * 4, line.height * 4)).save(path)
    return subprocess.check_output(
        ["tesseract", path, "stdout", "--psm", "7"],
        stderr=subprocess.DEVNULL, text=True).lower().strip()


def first_row(sim, png, art, label):
    """The first row's title and second line, read on their own: white on
    the selection bar reads poorly as part of the screen. 'art' for a
    55-pixel row beside a cover, else a 44-pixel one."""
    x, top = (60, 22) if art else (3, 20)
    if art:
        top += 2
    return (line_text(sim.case, png, (x, top, 296, top + 25), label + "-1"),
            line_text(sim.case, png, (x, top + 23, 296, top + 43),
                      label + "-2"))


def expect_first(sim, png, art, title, value):
    got = first_row(sim, png, art, png[:-4])
    assert got == (title, value), f"{png}: wanted {(title, value)}, " \
        f"read {got}"


def video_settings(sim):
    sim.go("select", "settings", ["tv signal", "tv screen", "picture scaling",
                                 "alternate audio", "captions", "subtitles"])
    tv_out = line_text(sim.case, "settings.png", (2, 22, 122, 44), "tv-out")
    assert tv_out == "tv out", tv_out


def keep(sim, states, name):
    """The settled screen as state 'name', for comparing with stock's"""
    if states:
        time.sleep(1.5)
        states.mkdir(parents=True, exist_ok=True)
        sim.image().save(states / f"{name}.png")


def stock_states(build, case, theme, states):
    """The screens stock was captured on, from stock's own library"""
    with session(build, case, theme, STOCK_FILMS) as sim:
        if theme == "classic":
            sim.text("home", ["videos", "files"])
        else:
            sim.text("home", ["music", "videos", "photos"])
            sim.tap("down")
        sim.go("select", "videos-menu", ["movies", "3 videos"])
        keep(sim, states, "videos-menu")
        sim.step("select", "movies", ["test pattern", "12 seconds"])
        keep(sim, states, "movies-placeholder")
        sim.step("menu", "videos-back", ["music videos", "3 videos"])
        sim.move("down", 5)
        sim.go("select", "settings", ["tv signal", "alternate audio"])
        keep(sim, states, "settings")


def rich_states(build, case, theme, states):
    """The seasons, episodes and playlists stock was captured on, from
    stock's richer library"""
    with session(build, case, theme, RICH_FILMS, RICH_PLAYLISTS) as sim:
        if theme == "classic":
            sim.text("home", ["videos", "files"])
        else:
            sim.text("home", ["music", "videos", "photos"])
            sim.tap("down")
        sim.go("select", "rich-videos-menu", ["movies", "rentals"])
        pane_shows_cover(sim, "rich-pane-movies",
                         [(200, 40, 40), (40, 160, 50)])
        keep(sim, states, "vm-movies")
        sim.move("down", 2)
        keep(sim, states, "vm-tvshows")
        sim.move("down", 2)
        keep(sim, states, "vm-playlists")
        sim.move("up", 2)
        sim.step("select", "tvshows", ["show two", "1 episode"])
        expect_first(sim, "tvshows.png", True, "show one", "2 seasons")
        keep(sim, states, "tvshows")
        sim.step("select", "seasons", ["show one", "season 2", "1 episode"],
                 ["show two"])
        expect_first(sim, "seasons.png", False, "season 1", "2 episodes")
        keep(sim, states, "tv-show-one-seasons")
        sim.step("select", "season-1", ["second episode",
                                       "unknown release date"])
        expect_first(sim, "season-1.png", False, "pilot",
                     "unknown release date")
        keep(sim, states, "tv-season1-episodes")
        sim.step("menu", "seasons-back", ["season 2"], ["second episode"])
        sim.step("menu", "tvshows-back", ["show two"], ["season 2"])
        sim.move("down")
        sim.step("select", "show-two", ["show two"], ["season"])
        expect_first(sim, "show-two.png", False, "only episode",
                     "unknown release date")
        keep(sim, states, "tv-show-two-episodes")
        sim.step("menu", "tvshows-again", ["tv shows"])
        sim.step("menu", "rich-videos-back", ["music videos",
                                             "video playlists"])
        sim.move("down", 2)
        sim.step("select", "video-playlists", ["video playlists"],
                 ["rentals"])
        expect_first(sim, "video-playlists.png", False, "road trip",
                     "3 videos")
        keep(sim, states, "video-playlists")
        sim.step("select", "road-trip", ["video one", "band a", "show one"])
        expect_first(sim, "road-trip.png", False, "alpha movie", "1:32:00")
        keep(sim, states, "playlist-road-trip")


def pane_shows_cover(sim, label, colours=()):
    """The Movies row's pane leaves the film picture and its count for a
    slideshow of the films' covers, given 'colours' one of those. A cover
    waits for the disk walk, and a load it interrupts is retried later."""
    deadline = time.monotonic() + 20
    while True:
        time.sleep(2)
        pane = sim.image().crop((186, 20, 320, 240))
        middle = pane.getpixel((67, 110))
        words = sim.read(pane)
        shown = "videos" not in words and (not colours or any(
            max(abs(a - b) for a, b in zip(middle, c)) < 12 for c in colours))
        if shown or time.monotonic() > deadline:
            break
    pane.save(sim.case / f"{label}.png")
    assert shown, f"{label}: no cover, {words!r} {middle}"


def stock_navigation(sim, theme, states=None):
    if theme == "classic":
        sim.text("home", ["videos", "files"])
    else:
        sim.text("home", ["music", "videos", "photos"])
        sim.tap("down")
    sim.go("select", "videos-menu", ["movies", "rentals", "tv shows",
                                   "music videos", "playlists", "settings"])
    # The walk that indexes the loose films starts at boot.
    time.sleep(12)
    sim.step("select", "movies", ["covered film", "indexed film"],
             ["rentals"])
    keep(sim, states, "loose-plain")
    sim.move("down")
    keep(sim, states, "loose-covr")
    sim.move("down", 2)
    sim.step("select", "movies-open", ["ipod video"])
    sim.text("movies-back", ["covered film", "film"], ["ipod video"])
    sim.step("menu", "videos-back", ["music videos", "video playlists"],
             ["indexed film"])
    pane_shows_cover(sim, "pane-movies")
    keep(sim, states, "pane-movies")
    sim.move("down", 2)
    sim.step("select", "tv-shows", ["tv shows"], ["rentals", "pilot"])
    expect_first(sim, "tv-shows.png", True, "my show", "2 seasons")
    sim.step("select", "tv-seasons", ["my show", "season 2"], ["pilot"])
    expect_first(sim, "tv-seasons.png", False, "season 1", "1 episode")
    sim.move("down")
    sim.step("select", "tv-episodes", ["season 2"], ["season 1"])
    expect_first(sim, "tv-episodes.png", False, "pilot", "march 8, 2026")
    sim.step("menu", "tv-seasons-back", ["season 2"], ["pilot"])
    sim.step("menu", "tv-back", ["tv shows"], ["rentals", "season 2"])
    sim.step("menu", "tv-shows-back", ["music videos", "video playlists"],
             ["pilot"])
    sim.move("down")
    sim.step("select", "music-videos", ["some band"], ["rentals"])
    sim.step("menu", "music-videos-back", ["music videos", "video playlists"],
             ["some band"])
    sim.move("up", 3)
    empty_menu_rows(sim)
    sim.tap("down")
    video_settings(sim)
    sim.go("menu", "settings-back", ["movies", "video playlists", "rentals"])


def loose_navigation(sim):
    sim.text("home", ["videos", "files"])
    # The list is the file index's, from a walk that starts at boot.
    # Until it has run, the row says there is nothing yet.
    time.sleep(4)
    sim.go("select", "videos-list", ["clip", "film"], ["song"])
    sim.go("select", "videos-open-m4v", ["ipod video"])
    sim.text("videos-back", ["clip", "film"])
    sim.go("menu", "home-again", ["videos", "files"], ["clip"])

    sim.tap("down")
    sim.go("select", "files", ["movies", "music"])
    sim.go("select", "movies", ["clip", "film"])
    sim.tap("down")
    sim.go("select", "files-open-mp4", ["ipod video"])
    sim.text("movies-back", ["movies", "clip"])
    sim.go("menu", "files-back", ["movies", "music"], ["clip"])
    sim.tap("down")
    sim.go("select", "music", ["song"], ["movies"])
    sim.go("select", "song-playing", ["song"], ["ipod video", "music"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=lambda p: Path(p).resolve())
    parser.add_argument("evidence", type=lambda p: Path(p).resolve())
    parser.add_argument("--theme", action="append",
                        choices=("scrim", "classic", "fruit-company",
                                 "fruit-company-ultra"))
    parser.add_argument("--states", type=lambda p: Path(p).resolve(),
                        help="also save the screens stock was captured on "
                        "under STATES/<theme>/")
    args = parser.parse_args()
    for theme in args.theme or ["scrim"]:
        case = args.evidence / theme
        case.mkdir(parents=True)
        states = args.states / theme if args.states and \
            theme != "scrim" else None
        with session(args.build, case, theme) as sim:
            if theme != "scrim":
                stock_navigation(sim, theme, states)
            else:
                loose_navigation(sim)
        print(f"PASS {theme} video navigation, list and player dispatch",
              flush=True)
        if states:
            stock_states(args.build, case / "stock", theme, states)
            rich_states(args.build, case / "rich", theme, states)
            print(f"PASS {theme} stock states in {states}", flush=True)


if __name__ == "__main__":
    main()
