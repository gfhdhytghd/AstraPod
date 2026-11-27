#!/usr/bin/env python3
"""Check the stock themes' page transitions against the stock firmware, in SDL.

Every distinct frame the simulator presents is recorded with its time, and
each check measures the recording:

  bar-drop     Classic, entering Cover Flow: the status bar stays hidden
               while the rows slide and drops in at least BAR_DELAY_MS
               after the first frame moves.
  np-title     Classic, leaving Now Playing for the main menu: every frame
               that still shows the Now Playing page shows its own title.
  push-back    Classic, Music to Album turned back at 130 ms: the edge of
               the arriving page never jumps more than JUMP px a frame.
  cf-back      Classic, Cover Flow entry turned back at 120 ms: the menu's
               highlight never jumps more than JUMP px a frame.
  still-covers Fruit Company keeps the covers still on entry and exit, so
               no frame shows a cover where Cover Flow at rest has none;
               Classic flies them, so at least two frames do.

Arguments: ROCKBOXUI ZIP OUTPUT_DIRECTORY [--fixture DIRECTORY]
ZIP is the build's rockbox.zip after the bundle steps; the fixture takes
its fonts and themes from it, and is built in OUTPUT_DIRECTORY/fixture
unless --fixture names one built before. Writes result.json, each
session's firmware log and every recording's frames.
"""

import argparse
import json
import mmap
import os
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path
from zipfile import ZipFile

from PIL import Image, ImageChops

sys.dont_write_bytecode = True
from ui_music_fixture import build_tagcache, create_fixture, \
    install_sim_codecs  # noqa: E402

W, H, RING = 320, 240, 8
PIXELS = W * H * 3
HEADER = struct.Struct("=III")
WORD = struct.Struct("=I")
STATE = HEADER.size + RING * (WORD.size + PIXELS)
FPS = 60
BAR = (0, 0, W, 17)
BAR_DELAY_MS = 300
JUMP = 96
STRIP = (W // 2, 60, W, 150)
COVER_PIXELS = 50
CLASSIC = ("root menu order: pictureflow,database,tagnavi8,images,podcasts,"
           "tagnavi9,settings,tagnavi10,wps,!tagnavi11,!artistportraits,"
           "!cider,!files,!reading,!random,!search,!docs,!books,!spun,"
           "!playlists,!currentplaylist,!quiz,!shortcuts,!plugins,"
           "!system_menu\n")


class Sim:
    """One simulator with no window, driven by key lines on stdin."""

    def __init__(self, binary, root, out):
        self.out = out
        self.handle = (out / "state.bin").open("w+b")
        self.handle.truncate(STATE)
        self.state = mmap.mmap(self.handle.fileno(), STATE,
                               access=mmap.ACCESS_READ)
        env = dict(os.environ, SDL_VIDEODRIVER="dummy",
                   SDL_AUDIODRIVER="dummy", SDL_RENDER_DRIVER="software",
                   PODBOX_UI_WALK_STATE=str(out / "state.bin"))
        env.pop("LD_PRELOAD", None)
        self.log = (out / "firmware.log").open("wb")
        self.process = subprocess.Popen(
            [str(binary), "--root", ".", "--nobackground", "--zoom", "1",
             "--screenshot", "--keys", "-"],
            cwd=root, env=env, stdin=subprocess.PIPE, stdout=self.log,
            stderr=subprocess.STDOUT, bufsize=0)

    def frames_since(self, since):
        _, count, _ = HEADER.unpack_from(self.state)
        got = []
        for seq in range(max(since + 1, count - RING + 1), count + 1):
            at = HEADER.size + ((seq - 1) % RING) * (WORD.size + PIXELS)
            if WORD.unpack_from(self.state, at)[0] != seq:
                continue
            pixels = bytes(self.state[at + WORD.size:at + WORD.size + PIXELS])
            if WORD.unpack_from(self.state, at)[0] == seq:
                got.append(pixels)
        return count, got

    def send(self, line):
        self.process.stdin.write((line + "\n").encode())

    def wait(self, frames):
        time.sleep(frames / FPS)
        assert self.process.poll() is None, "the simulator stopped"

    def tap(self, key, wait=30):
        self.send("down " + key)
        self.wait(2)
        self.send("up " + key)
        self.wait(wait)

    def record(self, frames, key, second=None, at_ms=0):
        """Press KEY, and SECOND AT_MS later, recording every distinct frame
        for FRAMES 60ths of a second. Returns (ms, image) pairs, the first
        being the screen before KEY."""
        seen, _ = self.frames_since(1 << 30)
        shots = []
        previous = None
        start = time.monotonic()
        self.send("down " + key)
        pending = [(2 / FPS, "up " + key)]
        if second:
            pending += [(at_ms / 1000, "down " + second),
                        (at_ms / 1000 + 2 / FPS, "up " + second)]
        while time.monotonic() - start < frames / FPS:
            now = time.monotonic() - start
            while pending and now >= pending[0][0]:
                self.send(pending.pop(0)[1])
            seen, got = self.frames_since(seen)
            for pixels in got:
                if pixels != previous:
                    shots.append((now * 1000, Image.frombytes(
                        "RGB", (W, H), pixels)))
                    previous = pixels
            time.sleep(0.002)
        for _, line in pending:
            self.send(line)
        assert shots, "no frame reached the panel"
        return shots

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(3)
            except subprocess.TimeoutExpired:
                self.process.kill()
        self.state.close()
        self.handle.close()
        self.log.close()


def make_fixture(binary, resource, root):
    create_fixture(root, resource)
    with ZipFile(resource) as archive:
        for name in archive.namelist():
            if name.startswith((".rockbox/themes/",
                                ".rockbox/native-themes/")):
                archive.extract(name, root)
    install_sim_codecs(binary, root)
    build_tagcache(binary, root, root.parent / "tagcache.log")


def session(binary, fixture, out, theme, extra, script):
    out.mkdir(parents=True, exist_ok=True)
    root = out / "disk"
    shutil.rmtree(root, ignore_errors=True)
    shutil.copytree(fixture, root)
    config = root / ".rockbox/config.cfg"
    for stale in ("config.cfg.new", "config.cfg.old"):
        (root / ".rockbox" / stale).unlink(missing_ok=True)
    lines = [line for line in config.read_text().splitlines()
             if not line.startswith("native theme:")]
    config.write_text("\n".join(lines) + "\nnative theme: " + theme +
                      "\nidle poweroff: off\nsleep timer on startup: off\n" +
                      extra)
    sim = Sim(binary, root, out)
    try:
        sim.wait(150)
        recordings = script(sim)
    finally:
        sim.close()
        shutil.rmtree(root, ignore_errors=True)
    for name, shots in recordings.items():
        folder = out / name
        folder.mkdir(exist_ok=True)
        rows = []
        for i, (ms, image) in enumerate(shots):
            image.save(folder / f"f{i:04d}.png")
            rows.append(f"f{i:04d}.png,{ms:.1f}")
        (folder / "frames.csv").write_text("file,t_ms\n" + "\n".join(rows) +
                                           "\n")
    return recordings


def difference(a, b, box=None):
    """Mean absolute difference per channel, 0-255."""
    if box:
        a, b = a.crop(box), b.crop(box)
    histogram = ImageChops.difference(a, b).convert("L").histogram()
    return sum(i * n for i, n in enumerate(histogram)) / max(1, sum(histogram))


def first_move(shots):
    return next(ms for ms, image in shots[1:]
                if difference(image, shots[0][1]) > 0)


def bar_drop(shots):
    moved = first_move(shots)
    final = shots[-1][1]
    shown = next(ms for ms, image in shots
                 if difference(image, final, BAR) < 1)
    hidden = any(difference(image, final, BAR) >= 1 and
                 difference(image, shots[0][1], BAR) >= 1
                 for ms, image in shots if ms < shown)
    return {"moved_ms": round(moved), "bar_ms": round(shown),
            "hidden_between": hidden}


def np_title(shots):
    """For each frame, the shift that best carries Now Playing's body to
    where it is now, and whether its title travels with it."""
    before = shots[0][1]
    body = before.crop((0, 30, W, 190))
    title = before.crop((0, 0, 100, 17))
    out = []
    for ms, image in shots[1:]:
        best = None
        for dx in range(0, W - 60, 2):
            error = difference(image.crop((dx, 30, W, 190)),
                               body.crop((0, 0, W - dx, 160)))
            if best is None or error < best[1]:
                best = (dx, error)
        dx, error = best
        if error > 3 or dx + 100 > W:
            continue
        out.append({"ms": round(ms), "dx": dx,
                    "title": difference(image.crop((dx, 0, dx + 100, 17)),
                                        title) < 3})
    return out


def edges(shots, box, columns):
    """Per frame, the first column of COLUMNS where at least half of BOX's
    rows differ from the frame before the key, or the column past the end."""
    before = shots[0][1].crop(box)
    out = []
    for ms, image in shots:
        moved = ImageChops.difference(image.crop(box), before).convert("L")
        data = moved.load()
        rows = box[3] - box[1]
        edge = columns[-1] + 1
        for x in columns:
            if sum(1 for y in range(rows) if data[x - box[0], y]) * 2 >= rows:
                edge = x
                break
        out.append((round(ms), edge))
    return out


def highlight(shots, y=32):
    """Per frame, the right end of the menu's highlight in row Y, or -1
    while the rows are off the screen."""
    out = []
    for ms, image in shots:
        row = [image.getpixel((x, y)) for x in range(W // 2 + 8)]
        blue = [x for x, (r, g, b) in enumerate(row) if b > 180 and r < 90]
        out.append((round(ms), blue[-1] if blue else -1))
    return out


def jumps(track):
    worst = 0
    for (_, a), (_, b) in zip(track, track[1:]):
        worst = max(worst, abs(b - a))
    return worst


def dark(image):
    gray = image.crop(STRIP).convert("L")
    return gray.point(lambda v: 255 if v < 40 else 0)


def flying_frames(shots, rest):
    """Frames with covers where REST has none. The menu and its pane only
    hide still covers, so only a cover that moves puts one there."""
    still = dark(rest)
    return sum(1 for _, image in shots
               if ImageChops.subtract(dark(image), still).histogram()[255] >
               COVER_PIXELS)


def classic_flow(sim):
    sim.wait(200)
    enter = sim.record(70, "select")
    sim.wait(60)
    leave = sim.record(40, "menu")
    sim.wait(60)
    back = sim.record(70, "select", "menu", 120)
    return {"cf-enter": enter, "cf-exit": leave, "cf-back": back}


def classic_push(sim):
    sim.wait(200)
    sim.tap("down")
    sim.tap("select", 90)
    return {"push-back": sim.record(60, "select", "menu", 130)}


def classic_now_playing(sim):
    sim.wait(200)
    sim.tap("down", 20)
    for _ in range(4):
        sim.tap("select", 60)
    sim.wait(60)
    for _ in range(4):
        sim.tap("menu", 40)
    sim.tap("up", 20)
    sim.tap("up", 30)
    sim.tap("select", 90)
    return {"np-leave": sim.record(60, "menu")}


def fruit_flow(sim):
    sim.wait(200)
    sim.tap("select", 60)
    enter = sim.record(70, "select")
    sim.wait(60)
    return {"cf-enter": enter, "cf-exit": sim.record(40, "menu")}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("binary", "resource", "output"):
        parser.add_argument(name, type=lambda arg: Path(arg).resolve())
    parser.add_argument("--fixture", type=lambda arg: Path(arg).resolve())
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    fixture = args.fixture or args.output / "fixture"
    if not args.fixture:
        shutil.rmtree(fixture, ignore_errors=True)
        make_fixture(args.binary, args.resource, fixture)

    def run(name, theme, extra, script):
        return session(args.binary, fixture, args.output / name, theme,
                       extra, script)

    classic = run("classic-flow", "classic", CLASSIC, classic_flow)
    push = run("classic-push", "classic", CLASSIC, classic_push)
    playing = run("classic-np", "classic", CLASSIC, classic_now_playing)
    fruit = run("fruit-flow", "fruit-company", "", fruit_flow)

    result = {
        "bar-drop": bar_drop(classic["cf-enter"]),
        "np-title": np_title(playing["np-leave"]),
        "push-back": edges(push["push-back"], (W // 2, 60, W, 200),
                           range(W // 2, W)),
        "cf-back": highlight(classic["cf-back"]),
        "still-covers": {
            "fruit-enter": flying_frames(fruit["cf-enter"],
                                         fruit["cf-enter"][-1][1]),
            "fruit-exit": flying_frames(fruit["cf-exit"],
                                        fruit["cf-exit"][0][1]),
            "classic-enter": flying_frames(classic["cf-enter"],
                                           classic["cf-enter"][-1][1]),
            "classic-exit": flying_frames(classic["cf-exit"],
                                          classic["cf-exit"][0][1]),
        },
    }
    (args.output / "result.json").write_text(json.dumps(result, indent=1) +
                                             "\n")

    failed = []
    drop = result["bar-drop"]
    if drop["bar_ms"] - drop["moved_ms"] < BAR_DELAY_MS or \
            not drop["hidden_between"]:
        failed.append(("bar-drop", drop))
    title = result["np-title"]
    if len(title) < 2 or not all(frame["title"] for frame in title):
        failed.append(("np-title", title))
    for name in ("push-back", "cf-back"):
        track = result[name]
        if jumps(track) > JUMP or len({x for _, x in track}) < 3:
            failed.append((name, jumps(track), track))
    covers = result["still-covers"]
    if covers["fruit-enter"] or covers["fruit-exit"] or \
            covers["classic-enter"] < 2 or covers["classic-exit"] < 2:
        failed.append(("still-covers", covers))
    for failure in failed:
        print("FAIL", *failure)
    if failed:
        sys.exit(1)
    print("PASS stock transitions: bar drops after "
          f"{drop['bar_ms'] - drop['moved_ms']} ms, Now Playing keeps its "
          f"title in {len(title)} frames, largest step "
          f"{jumps(result['push-back'])}/{jumps(result['cf-back'])} px, "
          f"frames with flying covers {covers}")


if __name__ == "__main__":
    main()
