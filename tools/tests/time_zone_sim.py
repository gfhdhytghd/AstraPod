"""Exercise the real Date & Time picker and status bar in a headless SDL sim.

Uses a test-only clock interposer, scripted SDL input and the LCD frame ring.
Run against a built Linux ipod6g sim and its bundled simdisk. Requires Pillow,
Tesseract and a C compiler. No host clock writes or physical player access.
"""
import argparse
import datetime as dt
import mmap
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import time

from PIL import Image, ImageOps

HEADER = struct.Struct("=III")
WORD = struct.Struct("=I")
WIDTH, HEIGHT = 320, 240
PIXELS = WIDTH * HEIGHT * 3
STATE_SIZE = HEADER.size + 8 * (WORD.size + PIXELS)
FAKE_CLOCK = r'''
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
time_t time(time_t *out)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    long long value = ts.tv_sec;
    const char *path = getenv("PODBOX_TEST_UTC_FILE");
    FILE *file = path ? fopen(path, "r") : NULL;
    if (file) {
        long long fake;
        if (fscanf(file, "%lld", &fake) == 1)
            value = fake;
        fclose(file);
    }
    if (out)
        *out = value;
    return value;
}
'''


def set_clock(path, value):
    instant = dt.datetime.fromisoformat(value).replace(tzinfo=dt.timezone.utc)
    staged = path.with_suffix(".next")
    staged.write_text(str(int(instant.timestamp())))
    staged.replace(path)


def capture(state):
    for _ in range(100):
        _, seq, _ = HEADER.unpack_from(state)
        if seq:
            at = HEADER.size + ((seq - 1) % 8) * (WORD.size + PIXELS)
            data = state[at + WORD.size:at + WORD.size + PIXELS]
            if WORD.unpack_from(state, at)[0] == seq:
                return Image.frombytes("RGB", (WIDTH, HEIGHT), data)
        time.sleep(0.01)
    raise AssertionError("No complete frame in the LCD ring")


def ocr(frame, mode):
    frame = ImageOps.grayscale(frame)
    frame = frame.resize((frame.width * 4, frame.height * 4))
    with tempfile.NamedTemporaryFile(suffix=".png") as png:
        frame.save(png.name)
        return subprocess.check_output(
            ["tesseract", png.name, "stdout", "--psm", str(mode)],
            text=True, stderr=subprocess.DEVNULL,
        )


def key(proc, name, count=1):
    for _ in range(count):
        proc.stdin.write(f"down {name}\n".encode())
        proc.stdin.flush()
        time.sleep(0.07)
        proc.stdin.write(f"up {name}\n".encode())
        proc.stdin.flush()
        time.sleep(0.13)
    time.sleep(0.25)
    assert proc.poll() is None, f"Simulator exited during {name}"


def expect_clock(proc, state, out, label, hour, minute):
    # Turn the wheel so the status-bar renderer sees a new frame and then
    # allow the title to expire. Keep the selected menu row where it started.
    key(proc, "down")
    key(proc, "up")
    time.sleep(4.3)
    frame = capture(state)
    frame.save(out / f"{label}.png")
    header = frame.crop((2, 0, 48, 18))
    text = ocr(header, 7)
    pattern = rf"(?<!\d)0?{hour}\s*[:.]\s*{minute:02d}(?!\d)"
    assert re.search(pattern, text), (label, text, (hour, minute))


def run(binary, simdisk, out):
    out.mkdir(parents=True, exist_ok=True)
    code = out / "fake-clock.c"
    library = out / "fake-clock.so"
    code.write_text(FAKE_CLOCK)
    subprocess.run(["cc", "-shared", "-fPIC", str(code), "-o", str(library)],
                   check=True)
    with tempfile.TemporaryDirectory(prefix="astrapod-zone-") as temp:
        root = Path(temp)
        rockbox = root / ".rockbox"
        rockbox.mkdir()
        for item in (simdisk / ".rockbox").iterdir():
            if item.is_dir() and item.name in ("fonts", "themes", "langs", "wps", "icons"):
                (rockbox / item.name).symlink_to(item)
        cfg = rockbox / "config.cfg"
        baseline = (simdisk / ".rockbox/default-config.cfg").read_text()
        # Later lines override the first-boot config.
        cfg.write_text(baseline + """
native theme: fruit-company
start in screen: root
backlight timeout: off
idle poweroff: off
voice menus: off
animations: off
time format: 24hour
time in title: on
time zone: America/New_York
""")
        stamp = root / "stamp"
        set_clock(stamp, "2026-03-08T06:59:00")
        frames = root / "frames"
        with frames.open("w+b") as memory, (out / "sim.log").open("wb") as log:
            memory.truncate(STATE_SIZE)
            with mmap.mmap(memory.fileno(), STATE_SIZE, access=mmap.ACCESS_READ) as state:
                env = dict(os.environ, SDL_VIDEODRIVER="dummy",
                           SDL_AUDIODRIVER="dummy", SDL_RENDER_DRIVER="software",
                           PODBOX_TEST_UTC_FILE=str(stamp), LD_PRELOAD=str(library),
                           PODBOX_UI_WALK_STATE=str(frames))
                def start():
                    memory.seek(0)
                    memory.write(bytes(STATE_SIZE))
                    memory.flush()
                    return subprocess.Popen(
                        [str(binary), "--root", ".", "--nobackground",
                         "--zoom", "1", "--keys", "-"], cwd=root, env=env,
                        stdin=subprocess.PIPE, stdout=log,
                        stderr=subprocess.STDOUT,
                    )

                def stop(proc):
                    if proc.poll() is None:
                        proc.terminate()
                    try:
                        proc.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        proc.kill()
                        proc.wait()

                proc = start()
                try:
                    time.sleep(7)
                    expect_clock(proc, state, out, "spring-before", 1, 59)
                    set_clock(stamp, "2026-03-08T07:00:00")
                    expect_clock(proc, state, out, "spring-after", 3, 0)
                    set_clock(stamp, "2026-11-01T05:59:00")
                    expect_clock(proc, state, out, "fall-before", 1, 59)
                    set_clock(stamp, "2026-11-01T06:00:00")
                    expect_clock(proc, state, out, "fall-after", 1, 0)

                    key(proc, "down", 5)
                    key(proc, "select")
                    key(proc, "down", 12)
                    key(proc, "select")
                    key(proc, "down", 2)
                    key(proc, "select")
                    picker = capture(state)
                    picker.save(out / "time-zone-regions.png")
                    assert "Time Zone" in ocr(picker, 6), "Zone picker did not open"
                    key(proc, "down", 2)  # America -> Asia
                    key(proc, "select")
                    cities = capture(state)
                    cities.save(out / "time-zone-cities.png")
                    assert "Asia" in ocr(cities.crop((0, 0, 60, 18)), 7), "Asia city list did not open"
                    key(proc, "select")
                    expect_clock(proc, state, out, "same-utc-new-city", 11, 0)
                    pending = cfg.with_suffix(".cfg.new")
                    for _ in range(120):
                        paths = (cfg, pending)
                        if any(p.exists() and re.search(r"^time zone: Asia/Almaty$",
                                                       p.read_text(), re.M)
                               for p in paths):
                            break
                        time.sleep(0.1)
                    else:
                        raise AssertionError("The picker did not save Asia/Almaty")
                    stop(proc)
                    proc = start()
                    time.sleep(7)
                    expect_clock(proc, state, out, "restart-new-city", 11, 0)
                    assert re.search(r"^time zone: Asia/Almaty$", cfg.read_text(), re.M)
                finally:
                    stop(proc)
    print("SDL dummy: spring gap, fall fold, region/city selection and unchanged UTC PASS")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("binary", "simdisk", "output"):
        parser.add_argument(name, type=lambda p: Path(p).resolve())
    args = parser.parse_args()
    run(args.binary, args.simdisk, args.output)
