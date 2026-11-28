#!/usr/bin/env python3
"""Photos with pictures copied to the player as loose files.

Drives a built ipod6g simulator headless (SDL dummy video):

    python3 tools/tests/photos_loose_sim.py build-sim-ipod6g EVIDENCE_DIR \\
        [--theme classic] [--stock-fixture DIR]

Generates about 200 pictures in nested folders -- JPEGs of several sizes
with four of 4000 x 3000, one progressive, PNGs, BMPs, a corrupt file, a
music album's art and hidden files that must stay out -- and checks:

  - Photos lists All Photos, Browse Files and Settings in order;
  - Browse Files lists the current folder and opens its photos in the grid;
  - All Photos opens the thumbnail grid, with placeholders while the
    thumbnails are made and the thumbnails afterwards;
  - a photo opens full screen, LEFT and RIGHT step, PLAY starts and pauses
    a slideshow and MENU leaves it;
  - a second visit, and a restart, draw the thumbnails from the cache.

It measures the time from a wheel step to the frame that shows it, and the
time between frames, while the thumbnails are being made and afterwards.

--stock-fixture DIR copies DIR/01.png..30.png into one folder instead and
captures the grid, full screen and RIGHT states for comparing against the
stock captures of the same pictures. Screens go to EVIDENCE_DIR/<theme>/.
"""
import argparse
from contextlib import contextmanager
import json
import mmap
import os
from pathlib import Path
import random
import shutil
import statistics
import struct
import subprocess
import sys
import time

from PIL import Image, ImageDraw

sys.path.insert(0, str(Path(__file__).resolve().parent))
from videos_sim import Sim, STATE_BYTES, player  # noqa: E402

LCD = (320, 240)
FRAME_BYTES = LCD[0] * LCD[1] * 3
GRID_TOP = 46
# November 27, 2026, the date of the stock captures' photos
STOCK_TIME = 1795737600


def picture(path, size, number, fmt="JPEG", **save):
    hue = number * 47 % 360
    image = Image.new("HSV", size, (hue * 255 // 360, 160, 200)).convert("RGB")
    # a camera picture's detail, which is what a decode spends its time on
    noise = Image.effect_noise(size, 48).convert("RGB")
    image = Image.blend(image, noise, 0.3)
    draw = ImageDraw.Draw(image)
    w, h = size
    draw.rectangle((w // 8, h // 8, w * 7 // 8, h * 7 // 8),
                   outline=(255, 255, 255), width=max(1, w // 160))
    draw.text((w // 3, h // 3), f"{number:03d}", fill=(255, 255, 255))
    path.parent.mkdir(parents=True, exist_ok=True)
    image.save(path, fmt, **save)


def library(disk):
    """About 200 pictures in mixed and photo-only folders."""
    rng = random.Random(7)
    sizes = [(640, 480), (480, 640), (2592, 1944), (800, 450), (1600, 1200),
             (3264, 2448), (2048, 1536), (300, 900), (1944, 2592)]
    n = 1
    folders = {
        "DCIM/100APPLE": 30, "Pictures/Trip 2024": 40,
        "Pictures/Trip 2024/Day 2": 20, "Pictures/Family": 25,
        "Misc/Family": 10, "Pictures/Family/Old/Scans": 15,
        "Wallpapers": 20, "Pictures/Pets": 25,
    }
    for folder, count in folders.items():
        for i in range(count):
            size = sizes[rng.randrange(len(sizes))]
            name = f"IMG_{i + 1:04d}.JPG"
            fmt, save = "JPEG", {"quality": 85}
            if folder == "Pictures/Family/Old/Scans" and i % 3 == 1:
                name, fmt, save = f"scan{i + 1}.png", "PNG", {}
            elif folder == "Pictures/Family/Old/Scans" and i % 3 == 2:
                name, fmt, save = f"scan{i + 1}.bmp", "BMP", {}
            elif folder == "Wallpapers" and i % 2:
                name, fmt, save = f"wall{i + 1}.png", "PNG", {}
            picture(disk / folder / name, size, n, fmt, **save)
            n += 1
    for i in range(4):
        picture(disk / "DCIM/100APPLE" / f"BIG_{i + 1}.JPG", (4000, 3000), n,
                quality=90)
        n += 1
    picture(disk / "DCIM/100APPLE/PROG.JPG", (1600, 1200), n,
            progressive=True)
    picture(disk / "root.jpg", (640, 480), n + 1)
    (disk / "Wallpapers/broken.jpg").write_bytes(
        b"\xff\xd8\xff\xe0" + os.urandom(3000))
    # what must not appear: a music album's art, hidden files, cover art
    picture(disk / "Music/Artist/Album/scan.jpg", (640, 640), 900)
    (disk / "Music/Artist/Album/01 Song.mp3").write_bytes(b"\0" * 1024)
    picture(disk / ".hidden/secret.jpg", (640, 480), 901)
    picture(disk / "Pictures/Pets/cover.jpg", (640, 480), 902)
    shutil.copy(disk / "Pictures/Pets/IMG_0001.JPG",
                disk / "Pictures/Pets/._IMG_0001.JPG")
    picture(disk / "DCIM/portrait.jpg", (640, 480), 903)
    picture(disk / "DCIM/folder.jpg", (640, 480), 904)


def stock_library(disk, fixture):
    folder = disk / "Pictures"
    folder.mkdir()
    for i in range(1, 31):
        target = folder / f"{i:02d}.png"
        shutil.copy(Path(fixture) / f"{i:02d}.png", target)
        os.utime(target, (STOCK_TIME, STOCK_TIME))
    return ["pictures"]


class Photos(Sim):
    def frame(self):
        """The newest frame's sequence number and pixels"""
        sequence, = struct.unpack_from("=I", self.state, 4)
        if not sequence:
            return 0, None
        offset = 12 + ((sequence - 1) % 8) * (FRAME_BYTES + 4)
        if struct.unpack_from("=I", self.state, offset)[0] != sequence:
            return sequence, None
        return sequence, bytes(self.state[offset + 4:offset + 4 + FRAME_BYTES])

    def send(self, line):
        self.proc.stdin.write((line + "\n").encode())
        self.proc.stdin.flush()

    def quick(self, key, hold=0.03):
        self.send("down " + key)
        time.sleep(hold)
        self.send("up " + key)

    def save(self, label):
        image = self.image()
        image.save(self.case / f"{label}.png")
        return image

    def settle(self, quiet=0.8, timeout=20):
        """Wait until no new frame has come for `quiet` seconds"""
        deadline = time.monotonic() + timeout
        last, since = self.frame()[0], time.monotonic()
        while time.monotonic() < deadline:
            time.sleep(0.02)
            now = self.frame()[0]
            if now != last:
                last, since = now, time.monotonic()
            elif time.monotonic() - since >= quiet:
                return
        raise AssertionError("the screen never settled")

    def step_latency(self, key, region, on_release=False):
        """Seconds from a key's press (or release) to the first frame that
        changes `region`, or None"""
        _, before = self.frame()
        self.send("down " + key)
        if on_release:
            time.sleep(0.02)
            self.send("up " + key)
        start = time.monotonic()
        threshold = start + 1.5
        try:
            while time.monotonic() < threshold:
                _, pixels = self.frame()
                if pixels is not None and before is not None and \
                        crop(pixels, region) != crop(before, region):
                    return time.monotonic() - start
                time.sleep(0.0005)
            return None
        finally:
            if not on_release:
                self.send("up " + key)

    def frame_times(self, seconds, key=None, every=0.06):
        """Intervals between frames over `seconds`, stepping `key` every
        `every` seconds"""
        stamps, last = [], self.frame()[0]
        end = time.monotonic() + seconds
        next_key = time.monotonic()
        while time.monotonic() < end:
            if key and time.monotonic() >= next_key:
                self.quick(key, 0.01)
                next_key += every
            sequence = self.frame()[0]
            if sequence != last:
                stamps.append(time.monotonic())
                last = sequence
            time.sleep(0.0005)
        return [b - a for a, b in zip(stamps, stamps[1:])]


def crop(pixels, region):
    x0, y0, x1, y1 = region
    stride = LCD[0] * 3
    return b"".join(pixels[y * stride + x0 * 3:y * stride + x1 * 3]
                    for y in range(y0, y1))


def stats(values):
    if not values:
        return {"count": 0}
    ms = sorted(v * 1000 for v in values)
    return {"count": len(ms), "median_ms": round(statistics.median(ms), 1),
            "p95_ms": round(ms[min(len(ms) - 1, int(len(ms) * 0.95))], 1),
            "max_ms": round(ms[-1], 1)}


# LCD_RGBPACK(40, 44, 40); stock's captures and the simulator's round the
# RGB565 value back out differently
LOADING = (41, 44, 41)


def placeholder_cells(image):
    """Cells of the grid drawn as stock's loading cell: flat dark grey"""
    count = 0
    for cell in range(15):
        x = cell % 5 * 64 + 1
        y = GRID_TOP + cell // 5 * 65
        colours = image.crop((x + 4, y + 4, x + 58, y + 59)).getcolors(1 << 16)
        if colours and len(colours) == 1 and \
                all(abs(c - l) <= 2 for c, l in zip(colours[0][1], LOADING)):
            count += 1
    return count


@contextmanager
def session(build, case, disk, theme):
    state_path = case / "state.bin"
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy",
               SDL_RENDER_DRIVER="software",
               PODBOX_UI_WALK_STATE=str(state_path))
    with state_path.open("w+b") as handle, \
            (case / "firmware.log").open("ab") as log:
        handle.truncate(STATE_BYTES)
        with mmap.mmap(handle.fileno(), 0, access=mmap.ACCESS_READ) as state, \
                subprocess.Popen([
                    build / "rockboxui", "--root", ".", "--nobackground",
                    "--zoom", "1", "--screenshot", "--keys", "-",
                ], stdin=subprocess.PIPE, env=env, cwd=disk, stdout=log,
                    stderr=subprocess.STDOUT) as proc:
            try:
                yield Photos(proc, state, case)
            finally:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait(timeout=5)


def assert_menu_order(sim, image, rows):
    menu = image.crop((0, 19, 160, 240)).resize((480, 663))
    path = sim.case / "menu-ocr.png"
    menu.save(path)
    names = subprocess.check_output(
        ["tesseract", path, "stdout", "--psm", "6"],
        stderr=subprocess.DEVNULL, text=True).lower()
    assert all(row in names for row in rows), names
    order = [names.index(row) for row in rows]
    assert order == sorted(order), names
    return names


def open_photos(sim, theme, at_once=False):
    if theme == "classic":
        sim.text("home", ["images"])
    else:
        sim.text("home", ["music", "videos", "photos"])
        sim.tap("down")
        sim.tap("down")
    if not at_once:
        sim.go("select", "photos", ["all photos", "settings"])
        return
    # the list's rows, which change only when the Photos menu opens
    rows = (0, 22, 150, 240)
    home = crop(sim.frame()[1], rows)
    sim.quick("select", 0.05)
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        _, pixels = sim.frame()
        if pixels and crop(pixels, rows) != home:
            break
        time.sleep(0.002)
    sim.save("photos")


def run(build, evidence, theme, fixture):
    case = evidence / theme
    if case.exists():
        shutil.rmtree(case)
    case.mkdir(parents=True)
    disk = case / "disk"
    disk.mkdir()
    player(build, disk, theme)
    with (disk / ".rockbox/config.cfg").open("a") as cfg:
        cfg.write("root menu order: images,files,settings\n"
                  "photo duration: 3\nphoto transition: 0\n")
    stock_library(disk, fixture) if fixture else library(disk)
    report = {"theme": theme}

    with session(build, case, disk, theme) as sim:
        index = disk / ".rockbox/library/cache/photos/thumbs.idx"
        made = lambda: max(0, (index.stat().st_size - 16) // 24) \
            if index.exists() else 0
        open_photos(sim, theme, at_once=True)
        # All Photos at once, first visit: page forward faster than the
        # pages' thumbnails can be made, then step with the wheel, while
        # the thumbnails are made
        report["made_at_entry"] = made()
        assert not index.exists(), "opening Photos started the thumbnail index"
        grid_region = (0, GRID_TOP, 320, 240)
        sim.quick("select", 0.05)
        building, samples = None, []
        deadline = time.monotonic() + 0.6
        while time.monotonic() < deadline:
            _, pixels = sim.frame()
            image = Image.frombytes("RGB", LCD, pixels)
            if building is None or \
                    placeholder_cells(image) > placeholder_cells(building):
                building = image
            time.sleep(0.003)
        for key, release, count in (("right", True, 4), ("up", False, 30)):
            for _ in range(count):
                count_before = made()
                latency = sim.step_latency(key, grid_region, release)
                image = sim.image()
                if building is None or \
                        placeholder_cells(image) > placeholder_cells(building):
                    building = image
                samples.append((key, latency, count_before, made()))
                time.sleep(0.02)
        building.save(case / "grid-building.png")
        report["placeholders_while_building"] = placeholder_cells(building)
        count_before = made()
        report["frames_while_building"] = stats(sim.frame_times(2, "down"))
        report["frames_while_building"]["made"] = [count_before, made()]
        started = time.monotonic()
        while time.monotonic() - started < 180:
            previous = made()
            time.sleep(1.5)
            if made() == previous:
                break
        total = report["made_total"] = made()
        building_steps = [v for k, v, a, b in samples if v and b < total]
        report["latency_while_building"] = stats(building_steps)
        report["latency_samples"] = [
            (k, round(v * 1000, 1) if v else None, a, b)
            for k, v, a, b in samples]
        sim.send("down back")
        time.sleep(1.2)
        sim.send("up back")
        sim.settle(1.0)
        built = sim.save("grid-built")
        report["placeholders_after"] = placeholder_cells(built)
        latencies = [sim.step_latency("down", grid_region) for _ in range(20)]
        report["wheel_latency_built"] = stats([v for v in latencies if v])
        report["frames_built"] = stats(sim.frame_times(3, "down"))
        sim.send("down back")
        time.sleep(1.2)
        sim.send("up back")
        sim.settle(1.0)

        # full screen: the second photo, then RIGHT and LEFT
        sim.tap("down")
        sim.settle(0.6)
        sim.save("grid-02")
        sim.tap("select")
        sim.settle(1.0)
        sim.save("view-02")
        sim.tap("right")
        sim.settle(0.6)
        sim.save("right-03")
        sim.tap("back")
        sim.settle(0.6)
        sim.save("left-02")
        if not fixture:
            # two steps at once land on a large JPEG nothing has readied
            sim.quick("right", 0.02)
            time.sleep(0.08)
            started = time.monotonic()
            sim.quick("right", 0.02)
            sim.settle(0.5, timeout=30)
            report["large_unreadied_s"] = round(time.monotonic() - started
                                                - 0.5, 2)
            sim.save("view-large")
        shown = sim.frame()[1]
        sim.tap("play")
        time.sleep(4.5)
        report["slideshow_advanced"] = sim.save("slideshow").tobytes() != shown
        sim.tap("play")
        time.sleep(0.5)
        paused = sim.frame()[0]
        time.sleep(4)
        report["paused_frames"] = sim.frame()[0] - paused
        sim.save("slideshow-paused")
        sim.tap("menu")
        sim.text("slideshow-menu", ["all photos", "settings"])

        # second visit draws from the cache at once
        sim.tap("select")
        time.sleep(0.4)
        report["placeholders_second_visit"] = placeholder_cells(
            sim.save("grid-second-visit"))
        sim.tap("menu")
        sim.text("photos-again", ["all photos", "settings"])
        menu_image = Image.open(case / "photos-again.png")
        menu = assert_menu_order(
            sim, menu_image, ("all photos", "browse files", "settings"))
        assert not any(name in menu for name in ("wallpapers", "100apple", "scans"))
        if not fixture:
            sim.tap("down")
            sim.step("select", "browse-root", ["dcim", "pictures", "wallpapers"],
                     absent=["100apple", "hidden"])
            sim.tap("down")  # Root Photos row, then DCIM
            sim.step("select", "browse-mixed", ["100apple"], absent=["wallpapers"])
            sim.step("select", "mixed-grid", ["1 photo"])
            sim.step("menu", "mixed-return", ["100apple"])
            sim.tap("down")
            sim.step("select", "leaf-grid", ["35 photos"])
            sim.step("menu", "leaf-return", ["100apple"])
            sim.step("menu", "browse-return", ["dcim", "pictures"])
            sim.step("menu", "photos-return", ["all photos", "browse files"])
            assert_menu_order(sim, sim.image(),
                              ("all photos", "browse files", "settings"))
            report["browse_verified"] = True

    with session(build, case, disk, theme) as sim:
        open_photos(sim, theme)
        sim.tap("select")
        time.sleep(0.4)
        report["placeholders_after_restart"] = placeholder_cells(
            sim.save("grid-after-restart"))

    cache = disk / ".rockbox/library/cache/photos"
    report["cache_files"] = sorted(
        f"{p.relative_to(cache)} {p.stat().st_size}"
        for p in cache.rglob("*") if p.is_file())[:12]
    (case / "report.json").write_text(json.dumps(report, indent=2))
    shutil.rmtree(disk, ignore_errors=True)
    return report


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("build", type=Path)
    parser.add_argument("evidence", type=Path)
    parser.add_argument("--theme", default="classic")
    parser.add_argument("--stock-fixture")
    args = parser.parse_args()
    report = run(args.build.resolve(), args.evidence.resolve(), args.theme,
                 args.stock_fixture)
    print(json.dumps(report, indent=2))
    failures = []
    if not args.stock_fixture and not report["placeholders_while_building"]:
        failures.append("no placeholder seen while building")
    if report["placeholders_after"]:
        failures.append("placeholders left after building")
    if report["placeholders_second_visit"] or \
            report["placeholders_after_restart"]:
        failures.append("thumbnails not drawn from the cache")
    if report["paused_frames"] > 2:
        failures.append("slideshow kept going while paused")
    if not report["slideshow_advanced"]:
        failures.append("slideshow did not advance")
    if failures:
        sys.exit("FAIL: " + "; ".join(failures))
    print("PASS")


if __name__ == "__main__":
    main()
