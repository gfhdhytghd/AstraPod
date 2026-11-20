#!/usr/bin/env python3
"""Record native iPod 6G SDL frames from a temporary, generated music library.

Build the simulator in an empty build directory with:
  /path/to/podbox/tools/configure --target=ipod6g --type=s --appsdir=apps-ipod
  make -j8 && make zip && /path/to/podbox/bundle-theme.sh
Then run this script with the path to that build directory. Requires Pillow
and ffmpeg. The index and Agents clips use a disposable simulator-only build
with mock wheel contact and an in-process HerdRemote peer.
"""

import argparse
import mmap
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
from zipfile import ZipFile

from PIL import Image, ImageDraw

TOOLS = Path(__file__).resolve().parent
REPO = TOOLS.parent
sys.dont_write_bytecode = True
sys.path.insert(0, str(TOOLS / "tests"))

from ui_music_fixture import DATABASE, database_count
from ui_random_walk import FPS, FRAME_COUNT, HEADER, PIXELS, STATE_SIZE, WORD
from ui_random_walk_remote import build_mock

SIZE = (320, 240)
GIF_FPS = 50
ART = DATABASE.parent / "cache/art"
ALBUMS = [
    ("Aster Vale", "Vesper Loom", "#141e3a", "#50c3bd", "#f7b45c"),
    ("Beryl Signal", "Nacre Circuit", "#2f183d", "#eb776b", "#fcd381"),
    ("Cinder Orchard", "Lumen Array", "#1d2840", "#688ce3", "#c1dbf6"),
    ("Drift Lantern", "Tessera Current", "#12322c", "#4ebe81", "#f4c96a"),
    ("Ember Meridian", "Juniper Coil", "#462129", "#ed8757", "#ffdeb3"),
    ("Fable Transit", "Mica Relay", "#1c2842", "#a494dd", "#f4a2ae"),
    ("Gossamer Pier", "Umber Kite", "#34301b", "#dbb04f", "#aad6aa"),
    ("Hollow Prism", "Sable Thread", "#282847", "#7ca2f0", "#fcb994"),
]
TRACKS = ("Paper Beacons", "Signal Bloom", "Glass Tide", "Copper Ferry", "Small Orbits")

# All times are absolute track times. The 2.26 s rest drives the native gap dots.
LYRICS = """\
<tt xmlns="http://www.w3.org/ns/ttml" xmlns:ttm="http://www.w3.org/ns/ttml#metadata">
  <head><metadata><ttm:agent xml:id="a" type="person"/>
    <ttm:agent xml:id="b" type="person"/></metadata></head>
  <body><div>
    <p begin="0.23s" end="1.72s" ttm:agent="a">
      <span begin="0.23s" end="0.96s">Fold the </span><span begin="0.96s" end="1.72s">blue light</span>
      <span ttm:role="x-bg" ttm:agent="b"><span begin="0.85s" end="1.72s">(blue light)</span></span>
    </p>
    <p begin="1.80s" end="2.77s" ttm:agent="b">
      <span begin="1.80s" end="2.26s">Thread it </span><span begin="2.26s" end="2.77s">through</span>
    </p>
    <p begin="5.03s" end="7.30s" ttm:agent="a">
      <span begin="5.03s" end="6.02s">Paper sails </span><span begin="6.02s" end="7.30s">unfold</span>
      <span ttm:role="x-bg" ttm:agent="b"><span begin="5.50s" end="7.30s">(unfold)</span></span>
    </p>
    <p begin="7.40s" end="10.80s" ttm:agent="b">
      <span begin="7.40s" end="8.32s">Past the </span><span begin="8.32s" end="10.80s">indigo pier</span>
    </p>
  </div></body>
</tt>
"""

CONFIG = """\
font: /.rockbox/fonts/18-noto-sans.pbf
font bold: /.rockbox/fonts/18-noto-sans-bold.pbf
start in screen: root
root menu order: pictureflow,files,database,wps,settings,system_menu
show files: supported
show icons: on
native theme: scrim
backlight timeout: off
voice menus: off
keyclick: off
idle poweroff: off
list wraparound: off
scrollbar: off
animations: on
album art: on
album covers statusbar: off
album covers center margin: 20
album covers sort albums by: name
statusbar: off
wps select action: lyrics
lyrics lead: 0
"""


def cover(path, index, dark, accent, light):
    art = Image.new("RGB", (256, 256), dark)
    draw = ImageDraw.Draw(art)
    x = 60 + 43 * (index % 4)
    y = 61 + 22 * (index % 3)
    draw.ellipse((x - 41, y - 41, x + 41, y + 41), fill=light)
    draw.polygon([(0, 173), (63, 104 + 13 * (index % 3)), (134, 180),
                  (207, 117 + index * 4), (255, 160), (255, 255), (0, 255)],
                 fill=accent)
    draw.polygon([(0, 213), (73, 181), (156, 228), (255, 189),
                  (255, 255), (0, 255)], fill=dark)
    for row in range(207, 256, 12):
        draw.line((164, row, 255, row - 28), fill=light, width=2)
    art.save(path, quality=94, subsampling=0)


def library(root):
    for index, (album, artist, dark, accent, light) in enumerate(ALBUMS):
        folder = root / "Music" / album
        folder.mkdir(parents=True)
        cover(folder / "folder.jpg", index, dark, accent, light)
        for number, title in enumerate(TRACKS, 1):
            path = folder / f"{number:02} {title}.flac"
            subprocess.run([
                "ffmpeg", "-nostdin", "-v", "error", "-f", "lavfi", "-i",
                f"sine=f={165 + index * 19 + number * 11}:r=44100:d=30",
                "-c:a", "flac", "-threads", "1",
                "-metadata", f"title={title}", "-metadata", f"track={number}",
                "-metadata", f"album={album}", "-metadata", f"artist={artist}",
                "-metadata", f"album_artist={artist}", "-metadata", "date=2026",
                str(path),
            ], check=True)
            if number == 1:
                path.with_suffix(".ttml").write_text(LYRICS, encoding="utf-8")


def resources(archive, root, extra=""):
    with ZipFile(archive) as assets:
        if ".rockbox/fonts/18-noto-sans.pbf" not in assets.namelist():
            raise RuntimeError("Run bundle-theme.sh on the simulator ZIP first")
        for name in assets.namelist():
            if name.startswith((".rockbox/fonts/", ".rockbox/langs/",
                                ".rockbox/codepages/")) or name in (
                                    ".rockbox/tagnavi.config", ".rockbox/codecs/flac.codec"):
                assets.extract(name, root)
    return config(root, extra)


def config(root, extra=""):
    path = root / ".rockbox/config.cfg"
    path.write_text(CONFIG + extra, encoding="utf-8")
    return path


def tap(key, frame=0):
    return [(frame, key, True), (frame + 3, key, False)]


class Capture:
    def __init__(self, binary, root, work, name, level=None):
        self.binary = binary
        self.root = root
        self.log = work / (name + ".log")
        self.state = work / (name + ".state")
        self.level = level

    def __enter__(self):
        with self.state.open("wb") as handle:
            handle.truncate(STATE_SIZE)
        with self.state.open("rb") as handle:
            self.frames = mmap.mmap(handle.fileno(), STATE_SIZE, access=mmap.ACCESS_READ)
        env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy",
                   SDL_RENDER_DRIVER="software", PODBOX_UI_WALK_STATE=str(self.state))
        for key in ("LD_PRELOAD", "PODBOX_UI_WALK_RENDER_DELAY", "FU121_AGENTS"):
            env.pop(key, None)
        if self.level == "agents":
            env["FU121_AGENTS"] = "1"
        with self.log.open("wb") as log:
            self.proc = subprocess.Popen(
                [str(self.binary), "--root", ".", "--nobackground", "--zoom", "1",
                 "--screenshot", "--keys", "-"],
                cwd=self.root, env=env, stdin=subprocess.PIPE, stdout=log,
                stderr=subprocess.STDOUT, bufsize=0,
            )
        return self

    def __exit__(self, *_):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        self.proc.stdin.close()
        self.frames.close()

    def check(self):
        if self.proc.poll() is not None:
            raise RuntimeError(f"Simulator exited with {self.proc.returncode}. {self.log.read_text(errors='replace')[-4000:]}")

    def latest(self):
        self.check()
        while True:
            _, seq, _ = HEADER.unpack_from(self.frames)
            if seq == 0:
                raise RuntimeError("SDL has not published any frames")
            offset = HEADER.size + ((seq - 1) % FRAME_COUNT) * (WORD.size + PIXELS)
            pixels = self.frames[offset + WORD.size:offset + WORD.size + PIXELS]
            if WORD.unpack_from(self.frames, offset)[0] == seq:
                return pixels

    def run(self, count, keys=(), dest=None):
        events = iter(sorted(keys))
        edge = next(events, None)
        start = time.monotonic()
        previous = None
        changes = 0
        for frame in range(count):
            deadline = start + frame / FPS
            time.sleep(max(0, deadline - time.monotonic()))
            while edge is not None and edge[0] <= frame:
                _, key, pressed = edge
                self.proc.stdin.write(f"{'down' if pressed else 'up'} {key}\n".encode())
                edge = next(events, None)
            if dest:
                if time.monotonic() - deadline >= 1 / FPS:
                    raise RuntimeError("Capture missed a 60 Hz deadline. Retry on an idle host.")
                pixels = self.latest()
                dest.write(pixels)
                if pixels != previous:
                    changes += 1
                    previous = pixels
        self.check()
        if edge is not None:
            raise ValueError(f"Unsent key edge {edge}")
        if dest and changes < 12:
            raise RuntimeError(f"Only {changes} SDL frame changes for {dest.name}")


def build_library_cache(binary, root, work):
    config(root, "start in screen: db\n")
    try:
        with Capture(binary, root, work, "database") as sim:
            end = time.monotonic() + 45
            while time.monotonic() < end:
                sim.check()
                covers = list((root / ART / "coverflow").glob("[0-9a-f]*.aat"))
                covers = [path for path in covers if path.stem != "fallback"]
                if (database_count(root) == len(ALBUMS) * len(TRACKS)
                        and len(covers) == len(ALBUMS)
                        and (root / ART / "stamps.dat").is_file()):
                    return
                time.sleep(0.1)
        raise RuntimeError("The simulator did not finish indexing the fixture tracks and covers")
    finally:
        (root / ".rockbox/config.cfg.new").unlink(missing_ok=True)
        config(root)


def record(name, binary, root, work):
    raw = work / f"{name}.rgb"
    with Capture(binary, root, work, name, level="agents" if name == "remote" else None) as sim, raw.open("wb") as frames:
        sim.run(155)
        if name == "list-push":
            # Warm the PBF glyph cache for the destination rows so the recorded
            # push captures every row's staggered entrance instead of blank rows
            # waiting on background glyph prefetch.
            sim.run(75, tap("select"))
            sim.run(95, tap("menu"))
            sim.run(246, tap("select", 36) + tap("menu", 147), frames)
        elif name == "index-mode":
            sim.run(46, tap("select"))
            steps = [(14, "touch", True)]
            for frame in range(18, 38, 2):
                steps += [(frame, "down", True), (frame + 1, "down", False)]
            for frame in (61, 104, 147):
                steps += tap("down", frame)
            sim.run(234, steps + [(178, "touch", False)], frames)
        elif name == "lyrics":
            sim.run(60, tap("select"))
            sim.run(38, tap("select"))
            sim.run(462, dest=frames)
        elif name == "coverflow-flip":
            sim.run(118, tap("select"))
            keys = tap("down", 28) + tap("down", 43) + tap("select", 105) + tap("menu", 213)
            sim.run(306, keys, frames)
        elif name == "now-playing":
            sim.run(148, tap("select"))
            sim.run(318, tap("select", 35) + tap("select", 130), frames)
        elif name == "remote":
            sim.run(264, tap("right", 49) + tap("down", 133) + tap("right", 183), frames)
    return raw


def encode(raw, path):
    # Keep the 60 Hz timeline. GIF delays of 10 ms render as 100 ms in Blink,
    # so encode at 50 Hz (20 ms) without blending frames. Use one palette and
    # no dithering to keep stationary glyph pixels stable.
    subprocess.run([
        "ffmpeg", "-nostdin", "-v", "error", "-y", "-filter_complex_threads", "1",
        "-f", "rawvideo", "-pixel_format", "rgb24", "-video_size", "320x240",
        "-framerate", str(FPS), "-i", str(raw), "-filter_complex",
        f"[0:v]fps={GIF_FPS},split[frames][sample];[sample]palettegen=stats_mode=diff[palette];"
        "[frames][palette]paletteuse=dither=none:diff_mode=rectangle",
        "-gifflags", "+offsetting+transdiff", "-final_delay", "2", "-loop", "0",
        str(path),
    ], check=True)
    with Image.open(path) as gif:
        count = gif.n_frames
        intervals = []
        for frame in range(count):
            gif.seek(frame)
            intervals.append(gif.info["duration"])
        duration = sum(intervals) / 1000
        size = path.stat().st_size
        if (gif.size != SIZE or not 3 <= duration <= 8 or size > 2_000_000
                or any(delay < 20 for delay in intervals)
                or abs(duration - raw.stat().st_size / PIXELS / FPS) > 0.011):
            raise RuntimeError(f"{path.name}: bad size/duration ({gif.size}, {size} B, {duration} s)")
        print(f"{path.name}\t{size}\t{duration:.2f}\t{count}", flush=True)


def patch_once(path, old, new):
    content = path.read_text(encoding="utf-8")
    if content.count(old) != 1:
        raise RuntimeError(f"Unexpected simulator source in {path}: {old!r}")
    path.write_text(content.replace(old, new), encoding="utf-8")


def capture_build(work, jobs):
    mock = work / "mock"
    mock.mkdir()
    binary = build_mock(REPO, mock, jobs)
    source = mock / "remote-src"
    shutil.copyfile(TOOLS / "readme-gif-mock.c", source / "apps-ipod/sim/fu79-peer.c")
    hook = source / "firmware/target/hosted/sdl/button-script-sdl.c"
    patch_once(hook, "#include <stdatomic.h>", "#include <stdbool.h>\n#include <stdatomic.h>")
    patch_once(hook, "static FILE *script;", """\
static FILE *script;
static atomic_bool wheel_contact;
bool fu121_wheel_touched(void) { return atomic_load(&wheel_contact); }""")
    patch_once(hook, "            SDL_Keycode key = lookup_key(name);", """\
            if (!strcmp(name, "touch"))
            {
                atomic_store(&wheel_contact, cmd[0] == 'd');
                continue;
            }
            SDL_Keycode key = lookup_key(name);""")
    patch_once(source / "apps-ipod/widgets/list_render.c", """\
    return wheel_status() >= 0;
#else
    return true;""", """\
    return wheel_status() >= 0;
#elif defined(SIMULATOR)
    extern bool fu121_wheel_touched(void);
    return fu121_wheel_touched();
#else
    return true;""")
    result = subprocess.run(["make", f"-j{jobs}", "bin"], cwd=binary.parent,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if result.returncode:
        raise RuntimeError(result.stdout)
    return binary


def scene_root(name, assets, base, work):
    root = work / name
    if name in ("coverflow-flip", "now-playing"):
        shutil.copytree(base, root)
    else:
        extra = "root menu order: files,pictureflow,wps,settings\n"
        resources(assets, root, extra)
        if name == "index-mode":
            for initial in "ABCDEFGHIJKLM":
                for item in ("Beacons", "Circuits", "Drift", "Embers", "Ferries", "Islands", "Signals"):
                    (root / f"{initial} · {item}.txt").touch()
        elif name == "list-push":
            for title in ("Copper Fields", "Glass Harbor", "Indigo Pier", "Paper Sails",
                          "Quiet Tides", "Vesper Loom"):
                (root / title).mkdir()
        elif name == "lyrics":
            folder = base / "Music" / ALBUMS[1][0]
            for suffix in ("flac", "ttml"):
                shutil.copyfile(folder / f"01 {TRACKS[0]}.{suffix}",
                                root / f"{TRACKS[0]}.{suffix}")
            shutil.copyfile(folder / "folder.jpg", root / "folder.jpg")
            config(root, "start in screen: files\nshow files: music\n")
    return root


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path, help="iPod 6G SDL build including bundled rockbox.zip")
    parser.add_argument("--output", type=Path, default=REPO / "docs/media")
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 1, 8))
    names = ("coverflow-flip", "lyrics", "list-push", "index-mode", "now-playing", "remote")
    parser.add_argument("--clip", choices=names, action="append", help="record selected clips")
    args = parser.parse_args()
    binary = args.build.resolve() / "rockboxui"
    assets = binary.with_name("rockbox.zip")
    args.output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="fu121-") as temp:
        work = Path(temp)
        base = work / "library"
        resources(assets, base)
        library(base)
        clips = args.clip or names
        if {"coverflow-flip", "now-playing"}.intersection(clips):
            build_library_cache(binary, base, work)
        mock = capture_build(work, args.jobs) if {"index-mode", "remote"}.intersection(clips) else None
        for name in clips:
            root = scene_root(name, assets, base, work)
            sim = mock if name in ("index-mode", "remote") else binary
            raw = record(name, sim, root, work)
            encode(raw, work / f"{name}.gif")
        for name in clips:
            shutil.copyfile(work / f"{name}.gif", args.output / f"{name}.gif")


if __name__ == "__main__":
    main()
