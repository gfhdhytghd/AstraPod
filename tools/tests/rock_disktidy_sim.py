#!/usr/bin/env python3
"""Exercise Disk Tidy against a private SDL volume with no host devices."""
import argparse
import mmap
import os
from pathlib import Path
import re
import signal
import struct
import subprocess
import time
from zipfile import ZipFile

from PIL import Image, ImageOps


WIDTH, HEIGHT = 320, 240
FRAME_BYTES = WIDTH * HEIGHT * 3
STATE_BYTES = 12 + 8 * (4 + FRAME_BYTES)


def prepare(root, archive, module, theme):
    disk = root / "disk"
    with ZipFile(archive) as package:
        for name in (".rockbox/fonts/18-noto-sans.pbf",
                     ".rockbox/fonts/18-noto-sans-bold.pbf",
                     ".rockbox/fonts/24x24-icons.fnt",
                     ".rockbox/rocks/apps/disktidy.config"):
            package.extract(name, disk)
        rock = ".rockbox/rocks/apps/disktidy.rock"
        if module is None:
            package.extract(rock, disk)
        else:
            (disk / rock).write_bytes(module.read_bytes())

    (disk / ".rockbox/config.cfg").write_text(
        "start in screen: root\nroot menu order: plugins,files,settings\n"
        "animations: off\nbacklight timeout: off\nvoice menus: off\n"
        "font: /.rockbox/fonts/18-noto-sans.pbf\n"
        "font bold: /.rockbox/fonts/18-noto-sans-bold.pbf\n"
        f"native theme: {theme}\nshow icons: off\n"
    )
    (disk / ".rockbox/rocks/apps/disktidy_custom.config").write_text(
        "System Volume Information/: yes\njunk.tmp: yes\ndiscard/: yes\n"
        ".rockbox/: yes\nrockbox.ipod: yes\n"
    )
    kept = [disk / ".rockbox/junk.tmp", disk / "rockbox.ipod", disk / "keep.bin"]
    outside = root / "outside"
    outside.mkdir()
    kept.append(outside / "junk.tmp")
    (disk / "link").symlink_to("../outside", target_is_directory=True)
    (disk / "dangling").symlink_to("missing")

    debris = [disk / "junk.tmp"]
    current = disk
    for level in range(36):
        current /= f"d{level:02d}"
        current.mkdir()
        debris.append(current / "junk.tmp")
    kept.append(current / "keep.bin")

    directories = []
    for parent in (disk, current):
        discard = parent / "discard"
        nested = discard / "a" / "b"
        nested.mkdir(parents=True)
        directories.append(discard)
        debris.append(nested / "inside.dat")
        debris.append(discard / "second.dat")
    for file in kept + debris:
        file.write_bytes(b"isolated Disk Tidy fixture\n")
    return disk, kept, debris, directories


def screen_text(frame, scratch):
    image = Image.new("L", frame.size)
    for top, bottom in ((0, 35), (35, HEIGHT)):
        area = frame.crop((0, top, WIDTH, bottom))
        area = ImageOps.autocontrast(ImageOps.grayscale(area))
        if area.resize((1, 1)).getpixel((0, 0)) < 127:
            area = ImageOps.invert(area)
        image.paste(area, (0, top))
    image.resize((WIDTH * 3, HEIGHT * 3)).save(scratch)
    output = subprocess.check_output(
        ["tesseract", str(scratch), "stdout", "--psm", "11"], text=True,
        stderr=subprocess.DEVNULL, env=dict(os.environ, OMP_THREAD_LIMIT="1"),
    )
    return " ".join(re.findall(r"[a-z0-9]+", output.casefold()))


def exercise(simulator, archive, module, case, theme):
    case.mkdir(parents=True)
    disk, kept, debris, directories = prepare(case, archive, module, theme)
    stats = disk / ".rockbox/rocks/apps/disktidy.stats"
    state_path = case / "state.bin"
    command = [
        "bwrap", "--die-with-parent", "--ro-bind", "/", "/",
        "--dev", "/dev", "--proc", "/proc", "--tmpfs", "/tmp",
        "--unshare-net", "--bind", str(case), str(case), "--chdir", str(disk),
        "--", str(simulator), "--root", ".", "--nobackground", "--zoom", "1",
        "--keys", "-",
    ]
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy",
               SDL_RENDER_DRIVER="software", XDG_RUNTIME_DIR="/tmp",
               PODBOX_UI_WALK_STATE=str(state_path))
    env.pop("LD_PRELOAD", None)
    with state_path.open("w+b") as handle, (case / "firmware.log").open("wb") as log:
        handle.truncate(STATE_BYTES)
        with mmap.mmap(handle.fileno(), 0, access=mmap.ACCESS_READ) as state, \
                subprocess.Popen(command, stdin=subprocess.PIPE, env=env, cwd=disk,
                                 stdout=log, stderr=subprocess.STDOUT,
                                 start_new_session=True) as sim:
            def tap(key):
                sim.stdin.write(f"down {key}\n".encode())
                sim.stdin.flush()
                time.sleep(.13)
                sim.stdin.write(f"up {key}\n".encode())
                sim.stdin.flush()
                time.sleep(.4)

            def capture(label, *expected):
                deadline = time.monotonic() + 20
                words = ""
                previous = None
                observations = set()
                while time.monotonic() < deadline:
                    if sim.poll() is not None:
                        raise RuntimeError(f"SDL exited with status {sim.returncode}")
                    _, seq, _ = struct.unpack_from("=III", state)
                    if seq:
                        offset = 12 + ((seq - 1) % 8) * (FRAME_BYTES + 4)
                        pixels = state[offset + 4:offset + 4 + FRAME_BYTES]
                        present, = struct.unpack_from("=I", state, offset)
                        if present == seq and pixels != previous:
                            previous = pixels
                            frame = Image.frombytes("RGB", (WIDTH, HEIGHT), pixels)
                            words = screen_text(frame, case / "ocr.png")
                            if words and words not in observations:
                                observations.add(words)
                                with (case / "ocr.log").open("a") as trace:
                                    trace.write(f"{label}: {words}\n")
                            compact = words.replace(" ", "")
                            if all(term.replace(" ", "") in compact
                                   for term in expected):
                                frame.save(case / f"{label}.png")
                                print(theme, label, words, flush=True)
                                return
                    time.sleep(.12)
                raise AssertionError((theme, label, expected, words))

            try:
                capture("home", "plugins")
                tap("select")
                capture("categories", "applications")
                tap("select")
                capture("browser", "disktidy")
                tap("select")
                capture("menu", "start cleaning", "files to clean")
                tap("down")
                tap("select")
                capture("rules", "files to clean", "none", "clean system volume")
                tap("back")
                capture("menu-again", "start cleaning")
                tap("up")
                tap("select")
                capture("confirm", "selected file types", "matching folders")
                tap("right")
                tap("select")
                capture("declined", "start cleaning")
                assert all(file.exists() for file in debris)
                assert not stats.exists()

                tap("select")
                capture("confirm-again", "selected file types", "matching folders")
                tap("select")
                capture("stats", "last run stats", "files removed", "directories removed")
                assert all(not file.exists() for file in debris)
                assert all(not directory.exists() for directory in directories)
                assert all(file.read_bytes() == b"isolated Disk Tidy fixture\n"
                           for file in kept)
                assert (disk / "link").is_symlink()
                assert (disk / "dangling").is_symlink()
                files, dirs, _, size = struct.unpack_from("@iiid", stats.read_bytes())
                assert files == len(debris), files
                assert dirs == 3 * len(directories), dirs
                assert size == sum(len(b"isolated Disk Tidy fixture\n") for _ in debris)
                assert not stats.with_suffix(".stats.tmp").exists()

                tap("back")
                capture("menu-after-stats", "start cleaning", "last run stats")
                tap("menu")
                capture("returned", "applications", "disktidy")

                # A skipped symlink makes rmdir fail. Only count the file
                # removed before that failure, and preserve the linked data.
                blocked = disk / "discard"
                blocked.mkdir()
                (blocked / "link").symlink_to("../../outside",
                                              target_is_directory=True)
                (blocked / "inside.dat").write_bytes(b"test")
                tap("select")
                capture("reopened", "start cleaning", "last run stats")
                tap("select")
                capture("confirm-rmdir-failure", "selected file types", "matching folders")
                tap("select")
                capture("returned-from-failure", "applications", "disktidy")
                assert blocked.is_dir() and (blocked / "link").is_symlink()
                assert not (blocked / "inside.dat").exists()
                assert kept[3].read_bytes() == b"isolated Disk Tidy fixture\n"
                files, dirs, _, size = struct.unpack_from("@iiid", stats.read_bytes())
                assert (files, dirs, size) == (1, 0, 4)
            finally:
                if sim.poll() is None:
                    os.killpg(sim.pid, signal.SIGTERM)
                sim.wait(timeout=8)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("simulator", type=Path)
    parser.add_argument("archive", type=Path)
    parser.add_argument("evidence", type=Path)
    parser.add_argument("--module", type=Path, help="module override for dev builds")
    args = parser.parse_args()
    for theme in ("scrim",):
        exercise(args.simulator.resolve(), args.archive.resolve(), args.module,
                 args.evidence.resolve() / theme, theme)


if __name__ == "__main__":
    main()
