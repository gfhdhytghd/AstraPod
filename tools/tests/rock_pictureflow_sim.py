#!/usr/bin/env python3
"""Exercise standalone PictureFlow on a generated native database in SDL."""
import argparse
from array import array
from contextlib import contextmanager
from hashlib import sha256
from pathlib import Path
import mmap
import os
import shutil
import signal
import struct
import subprocess
import time
from zipfile import ZipFile

from PIL import Image, ImageOps

from rock_disktidy_sim import STATE_BYTES
from rock_phase3_files_sim import Simulator


def seed(disk):
    for artist, album, tracks, color in (
        ("Alpha Artist", "Shared Title", [("Alpha Track", 1)], (240, 25, 20)),
        ("Beta Artist", "Shared Title", [("Beta Second", 2), ("Beta First", 1)],
         (25, 240, 20)),
        ("Gamma Artist", "No Cover", [("Gamma Track", 1)], None),
    ):
        folder = disk / "Music" / artist
        folder.mkdir(parents=True)
        if color:
            Image.new("RGB", (480, 480), color).save(folder / "cover.jpg")
        for index, (title, number) in enumerate(tracks):
            subprocess.run([
                "ffmpeg", "-nostdin", "-v", "error", "-f", "lavfi", "-i",
                "sine=frequency=440:duration=20:sample_rate=44100", "-c:a", "flac",
                "-metadata", f"artist={artist}", "-metadata", f"album_artist={artist}",
                "-metadata", f"album={album}", "-metadata", f"title={title}",
                "-metadata", f"track={number}", "-metadata", "date=2024",
                str(folder / f"{index}.flac"),
            ], check=True)


def prepare(args):
    disk = args.evidence / "disk"
    rb = disk / ".rockbox"
    demos = rb / "rocks/demos"
    demos.mkdir(parents=True)
    with ZipFile(args.resources) as archive:
        for path in ("18-noto-sans.pbf", "18-noto-sans-bold.pbf", "24x24-icons.fnt"):
            archive.extract(f".rockbox/fonts/{path}", disk)
    shutil.copyfile(args.build / "rock-compat/modules/demos/pictureflow.rock",
                    demos / "pictureflow.rock")
    (rb / "codecs").mkdir()
    shutil.copyfile(args.build / "lib/rbcodec/codecs/flac.codec", rb / "codecs/flac.codec")
    (rb / "config.cfg").write_text(
        "start in screen: root\nroot menu order: plugins,music,files,settings\n"
        "animations: off\nbacklight timeout: off\nvoice menus: off\nvolume: -6\n"
        "font: /.rockbox/fonts/18-noto-sans.pbf\n"
        "font bold: /.rockbox/fonts/18-noto-sans-bold.pbf\n"
        f"native theme: {args.theme}\n"
        "tagcache_scan_on_startup: on\n"
    )
    seed(disk)
    return disk


@contextmanager
def run_sim(args, disk, label):
    case = args.evidence
    state = case / f"{label}.bin"
    log_path = case / f"{label}.log"
    command = [
        "bwrap", "--die-with-parent", "--ro-bind", "/", "/",
        "--dev", "/dev", "--proc", "/proc", "--tmpfs", "/tmp", "--unshare-net",
        "--bind", str(case), str(case), "--chdir", str(disk), "--",
        "stdbuf", "-oL", str(args.build / "rockboxui"), "--root", ".",
        "--nobackground", "--zoom", "1", "--screenshot", "--keys", "-",
    ]
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="disk",
               SDL_DISKAUDIOFILE=str(case / f"{label}.pcm"),
               SDL_RENDER_DRIVER="software", XDG_RUNTIME_DIR="/tmp",
               PODBOX_UI_WALK_STATE=str(state))
    with state.open("w+b") as handle, log_path.open("wb") as log:
        handle.truncate(STATE_BYTES)
        with mmap.mmap(handle.fileno(), 0, access=mmap.ACCESS_READ) as frames, \
                subprocess.Popen(command, stdin=subprocess.PIPE, env=env,
                                 stdout=log, stderr=subprocess.STDOUT,
                                 start_new_session=True) as proc:
            try:
                yield Simulator(proc, frames, case), log_path
            finally:
                if proc.poll() is None:
                    os.killpg(proc.pid, signal.SIGTERM)
                proc.wait(timeout=8)
    assert "PANIC" not in log_path.read_text()


def await_condition(condition, description, timeout=15):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if condition():
            return
        time.sleep(.10)
    raise AssertionError(description)


def plugin_browser(sim):
    sim.capture("home", "plugins")
    sim.tap("select")
    sim.capture("categories", "applications", "demos")
    sim.tap("down", "down", "select")
    sim.capture("browser", "pictureflow")


def check_color(frame, channel):
    r, g, b = frame.getpixel((160, 100))
    rgb = (r, g, b)
    assert rgb[channel] > 170 and sum(rgb) - rgb[channel] < 100, rgb


def assert_new_cache(cache):
    files = list(cache.glob("*.pfraw"))
    assert len(files) == 2, files
    for path in files:
        data = path.read_bytes()
        width, height = struct.unpack_from("=hh", data)
        assert width == height == 128 and len(data) == 4 + width * height * 2, path


def check_track_list(sim, label):
    frame = sim.capture(label, "beta second", "beta artist")
    for y, track in ((72, "beta first"), (92, "beta second")):
        row = frame.crop((60, y, 260, y + 19)).convert("L")
        row = ImageOps.invert(ImageOps.autocontrast(row))
        path = sim.evidence / f"{label}-{y}.png"
        row.resize((800, 76)).save(path)
        text = subprocess.check_output(
            ["tesseract", str(path), "stdout", "--psm", "7"],
            text=True, stderr=subprocess.DEVNULL).lower()
        assert track in text, text


def run(args):
    disk = prepare(args)
    rb = disk / ".rockbox"
    cache = rb / "pictureflow"
    native_art = rb / "library/cache/art"
    with run_sim(args, disk, "cold") as (sim, log):
        # Let the native first-boot scan index the generated FLAC tags.
        await_condition(lambda: (rb / "library/database/database_idx.tcd").is_file(),
                        "native tagcache did not build", timeout=30)
        await_condition(lambda: (rb / "library/cache/art/stamps.dat").is_file(),
                        "native art cache did not settle", timeout=30)
        native_before = {p: sha256(p.read_bytes()).digest()
                         for p in native_art.rglob("*.aat")}
        assert native_before
        plugin_browser(sim)
        sim.tap("select")
        await_condition(lambda: len(list(cache.glob("*.pfraw"))) == 2,
                        "PictureFlow did not write two separate cached covers")
        assert_new_cache(cache)
        check_color(sim.capture("alpha-cover", "shared title", "alpha artist"), 0)
        sim.tap("down")
        check_color(sim.capture("beta-cover", "shared title", "beta artist"), 1)
        sim.tap("select")
        check_track_list(sim, "beta-tracks")
        sim.tap("menu")
        sim.capture("normal-return", "pictureflow")
        for path, digest in native_before.items():
            assert sha256(path.read_bytes()).digest() == digest, path
        assert (cache / "pictureflow.cfg").is_file()
        sim.tap("select")
        check_color(sim.capture("reopened", "beta artist"), 1)
        sim.tap("down")
        sim.capture("placeholder", "gamma artist", "no cover")
        sim.tap("usb")
        sim.capture("usb-consent", "computer connected")
        sim.tap("select")
        await_condition(lambda: "All threads have acknowledged the connect." in log.read_text(),
                        "USB did not take storage")
        sim.tap("usb")
        sim.capture("usb-return", "plugins")

    # A short or oversized cache entry must be replaced by a decoded cover.
    for path in cache.glob("*.pfraw"):
        path.write_bytes(struct.pack("=hh", 32767, 32767) + b"x")
    with run_sim(args, disk, "warm") as (sim, _):
        plugin_browser(sim)
        sim.tap("select")
        check_color(sim.capture("corrupt-cache-fallback", "beta artist"), 1)
        await_condition(lambda: all(path.stat().st_size > 9 for path in
                                    cache.glob("*.pfraw")), "covers were not rebuilt")
        assert_new_cache(cache)
        sim.tap("select")
        check_track_list(sim, "warm-tracks")
        sim.tap("select")
        sim.tap("menu", "select")
        check_color(sim.capture("during-playback", "beta artist"), 1)
        pcm = args.evidence / "warm.pcm"
        offset = pcm.stat().st_size
        time.sleep(0.5)
        data = pcm.read_bytes()[offset:]
        assert len(data) > 4096 and any(
            abs(sample) > 8 for sample in array("h", data[:len(data) & ~1])
        ), "native PCM did not continue during PictureFlow"
        assert not list(cache.glob("*.tmp"))
    print("PASS PictureFlow cold/warm, track filters, placeholder, exit, USB and cache recovery")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path, help="configured SDL build directory")
    parser.add_argument("resources", type=Path, help="archive with AstraPod fonts")
    parser.add_argument("evidence", type=Path, help="new directory for the test disk and captures")
    parser.add_argument("--theme", default="scrim")
    args = parser.parse_args()
    for field in ("build", "resources", "evidence"):
        setattr(args, field, getattr(args, field).resolve())
    run(args)
