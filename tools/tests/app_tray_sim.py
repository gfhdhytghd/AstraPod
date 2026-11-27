#!/usr/bin/env python3
"""Capture the status-bar app tray in each built-in theme, headless.

The tray store is written before boot: MochiPod's icon in the first slot and
the 8 x 8 demo icon in the second, or only as many as --trays names. A WAV
is played, and the status bar of the file list is cropped and zoomed 8x
while it plays and again once it is paused.

    app_tray_sim.py SIMULATOR RESOURCES EVIDENCE [--store 1|2]
                    [--mochi-frame N] [--themes scrim,light,classic,...]
                    [--trays 0,1,2] [--check] [--demo]

RESOURCES is the simulator build's rockbox.zip after the bundle steps.
--store 1 writes the old store layout with MochiPod's 8 x 8 icons, for a
build from before the 12 x 12 tray; --store 2 writes the current one.
--check asserts, in Classic and Fruit Company, that each tray icon's ink
lies within the playback icon's rows, that a 12-row icon spans the
battery's, and that 7 clear columns separate the tray's drawn pixels. In
Scrim and Light it asserts that the tray sits against the battery with the
playback icon left of it, 9 clear columns between the drawn pixels of each
(8 from the play icon's faint tip), and that a 12-row icon is centred.
--demo leaves the second slot empty and runs app_tray_demo from the file
list instead, then checks that it saved its 8 x 8 frames centred.
"""
import argparse
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
import time
import zipfile

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ui_menu_sim import FPS, Screen, process  # noqa: E402
from ui_native_wps_sim import add_wave_fixture  # noqa: E402

THEMES = ("scrim", "light", "classic", "fruit-company")
ART_THEMES = ("classic", "fruit-company")
FLAT_THEMES = ("scrim", "light")
FLAT_GAP = 9
STATUS_HEIGHT = {"scrim": 35, "light": 35, "classic": 20, "fruit-company": 20}
ZOOM = 8

# MochiPod's frames: content, asleep, needs care, sick or dead.
MOCHI8 = (
    (0x38, 0x7f, 0xef, 0xfc, 0xfc, 0xef, 0x7f, 0x38),
    (0x90, 0xd0, 0xb0, 0x90, 0x09, 0x0d, 0x0b, 0x09),
    (0x38, 0x6e, 0x78, 0x6e, 0x38, 0x00, 0x5f, 0x00),
    (0x1c, 0x72, 0x33, 0x7f, 0x7f, 0x33, 0x72, 0x1c),
)
MOCHI12 = (
    (0x1f0, 0x3ff, 0x7ff, 0xf9c, 0xef8, 0xdf8,
     0xdf8, 0xef8, 0xf9c, 0x7ff, 0x3ff, 0x1f0),
    (0x880, 0xc80, 0xa80, 0x980, 0x003, 0x0c3,
     0x0e3, 0x0f3, 0x0db, 0x0cf, 0x0c7, 0x0c3),
    (0x1f8, 0x3ff, 0x7cf, 0x77c, 0x6fc, 0x77c,
     0x7cf, 0x3ff, 0x1f8, 0x000, 0xcff, 0xcff),
    (0x0fc, 0x1fe, 0x7cf, 0xfc7, 0xdc7, 0x77f,
     0x77f, 0xdc7, 0xfc7, 0x7cf, 0x1fe, 0x0fc),
)
# app_tray_demo's frames: circle, circle with a dot, disc.
DEMO8 = (
    (0x00, 0x3c, 0x42, 0x42, 0x42, 0x42, 0x3c, 0x00),
    (0x00, 0x3c, 0x42, 0x5a, 0x5a, 0x42, 0x3c, 0x00),
    (0x00, 0x3c, 0x7e, 0x7e, 0x7e, 0x7e, 0x3c, 0x00),
)


def widen(frame):
    """An 8 x 8 frame centred in 12 x 12, as app_tray_register8() does."""
    out = [0] * 12
    for c, column in enumerate(frame):
        out[c + 2] = column << 2
    return out


def store(version, slots):
    """The tray file: slots are (owner, frames, selected frame)."""
    frame_format = "<8B" if version == 1 else "<12H"
    blank = (0,) * (8 if version == 1 else 12)
    body = bytearray(b"ASTRY01\0" if version == 1 else b"ASTRY02\0")
    for i in range(4):
        owner, frames, selected = slots[i] if i < len(slots) else ("", (), 0)
        body += owner.encode().ljust(32, b"\0")
        body += struct.pack("<5I", 0, 0, 0, 0, 0)
        for f in range(4):
            body += struct.pack(frame_format,
                                *(frames[f] if f < len(frames) else blank))
        body += bytes(4) + bytes((len(frames), selected, 0, 0))
    return bytes(body) + struct.pack("<I", crc_32(bytes(body)))


def crc_32(data, crc=0xffffffff):
    """firmware/common/crc32.c: polynomial 0x04c11db7, MSB first."""
    for byte in data:
        crc ^= byte << 24
        for _ in range(8):
            crc = (crc << 1 ^ 0x04c11db7 if crc & 0x80000000 else crc << 1)
            crc &= 0xffffffff
    return crc


def segments(bar):
    """Runs of inked columns in the bar's right half, 4 clear columns apart
    at least, each as (left, right, top, bottom) of its ink."""
    width, height = bar.size
    reference = [bar.getpixel((width - 2, y)) for y in range(height - 1)]
    ink = []
    for x in range(width // 2, width - 2):
        rows = [y for y in range(height - 1)
                if bar.getpixel((x, y)) != reference[y]]
        ink.append((x, rows))
    runs = []
    for x, rows in ink:
        if not rows:
            continue
        if runs and x - runs[-1][1] <= 4:
            run = runs[-1]
            runs[-1] = (run[0], x, min(run[2], rows[0]), max(run[3], rows[-1]))
        else:
            runs.append((x, x, rows[0], rows[-1]))
    return runs


def clear_gaps(runs):
    """Clear columns between neighbouring runs, measured from drawn ink."""
    return [b[0] - a[1] - 1 for a, b in zip(runs, runs[1:])]


def capture(simulator, resources, root, env, theme, tray, evidence, demo,
            label):
    with zipfile.ZipFile(resources) as archive:
        for name in archive.namelist():
            if name.startswith((".rockbox/fonts/", ".rockbox/themes/",
                                ".rockbox/codecs/wav", ".rockbox/icons/")):
                archive.extract(name, root)
        if demo:
            (root / "app_tray_demo.rock").write_bytes(
                archive.read(".rockbox/rocks/demos/app_tray_demo.rock"))
    rockbox = root / ".rockbox"
    shown = "all" if demo else "music"
    (rockbox / "config.cfg").write_text(
        f"start in screen: files\nshow files: {shown}\n"
        "backlight timeout: off\n"
        "voice menus: off\nanimations: off\nalbum art: off\n"
        "show background tasks: off\nshow disk activity: off\n"
        f"native theme: {theme}\n")
    (rockbox / "app-tray.dat").write_bytes(tray)
    add_wave_fixture(root / "Test.wav")
    log = evidence / f"{label}.log"
    with log.open("w") as out, process(
        [str(simulator), "--root", str(root), "--nobackground", "--zoom", "1",
         "--screenshot"], out, env=env, cwd=root,
    ) as firmware:
        time.sleep(120 / FPS)
        screen = Screen(env, firmware)
        time.sleep(60 / FPS)
        # Below .rockbox, the demo registers and returns; then the WAV is
        # played and the list shown, then it is paused in Now Playing.
        keys = ("Down", "Return", "Down") if demo else ()
        keys += ("Return", "Escape", "Return", "space", "Escape")
        frames = {}
        for step, key in enumerate(keys):
            screen.tap(key)
            time.sleep(150 / FPS)  # past a one-second splash
            screen.frame().save(evidence / f"{label}-step{step}-{key}.png")
            if key == "Escape":
                frames["pause" if "play" in frames else "play"] = \
                    screen.frame()
        assert firmware.poll() is None
        return frames


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawTextHelpFormatter)
    for key in ("simulator", "resources", "evidence"):
        parser.add_argument(key, type=lambda v: Path(v).resolve())
    parser.add_argument("--store", type=int, choices=(1, 2), default=2)
    parser.add_argument("--mochi-frame", type=int, choices=range(4), default=0)
    parser.add_argument("--themes", default=",".join(THEMES))
    parser.add_argument("--trays", default="2")
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--demo", action="store_true")
    args = parser.parse_args()
    args.evidence.mkdir(parents=True, exist_ok=True)

    if args.store == 1:
        slots = [("mochipod", MOCHI8, args.mochi_frame),
                 ("app_tray_demo", DEMO8, 1)]
    else:
        slots = [("mochipod", MOCHI12, args.mochi_frame),
                 ("app_tray_demo", [widen(f) for f in DEMO8], 1)]
    # The demo registers itself in the second slot.
    stored = slots[:1] if args.demo else slots
    counts = [2] if args.demo else [int(n) for n in args.trays.split(",")]

    result = {}
    with tempfile.TemporaryDirectory(prefix="app-tray-") as folder, \
            tempfile.TemporaryFile() as display, \
            (args.evidence / "xvfb.log").open("w") as log:
        base = Path(folder)
        runtime = base / "runtime"
        runtime.mkdir(mode=0o700)
        with process(["Xvfb", "-displayfd", str(display.fileno()), "-screen",
                      "0", "800x600x24", "-nolisten", "tcp"], log,
                     pass_fds=(display.fileno(),)):
            for _ in range(300):
                display.seek(0)
                number = display.read().decode().strip()
                if number:
                    break
                time.sleep(1 / FPS)
            else:
                raise RuntimeError("Xvfb did not start")
            env = dict(os.environ, DISPLAY=":" + number,
                       XDG_RUNTIME_DIR=str(runtime), SDL_VIDEODRIVER="x11",
                       SDL_AUDIODRIVER="dummy", SDL_RENDER_DRIVER="software",
                       LIBGL_ALWAYS_SOFTWARE="1",
                       __GLX_VENDOR_LIBRARY_NAME="mesa")
            for theme in args.themes.split(","):
                for count in counts:
                    name = f"{theme}-tray{count}"
                    root = base / name
                    root.mkdir()
                    frames = capture(args.simulator, args.resources, root,
                                     env, theme, store(args.store,
                                                       stored[:count]),
                                     args.evidence, args.demo, name)
                    if args.demo:
                        # The demo's 8 x 8 frames, centred, in the second
                        # slot.
                        saved = (root / ".rockbox/app-tray.dat").read_bytes()
                        slot = saved[8 + 156:8 + 2 * 156]
                        assert slot[:14] == b"app_tray_demo\0", slot[:14]
                        frames_saved = struct.unpack_from("<36H", slot, 52)
                        assert list(frames_saved) == sum(
                            (widen(f) for f in DEMO8), []), frames_saved
                    for state, frame in frames.items():
                        shot = f"{name}-{state}"
                        frame.save(args.evidence / f"{shot}.png")
                        bar = frame.crop((0, 0, 320, STATUS_HEIGHT[theme]))
                        bar.crop((160, 0, 320, bar.height)).resize(
                            (160 * ZOOM, bar.height * ZOOM),
                            Image.NEAREST).save(
                            args.evidence / f"{shot}-bar-8x.png")
                        runs = segments(bar)
                        result[shot] = {"ink": runs, "gaps": clear_gaps(runs)}

    (args.evidence / "result.json").write_text(json.dumps(result, indent=2))
    print(json.dumps(result))
    if args.check:
        for theme in ART_THEMES:
            shot = f"{theme}-tray2-pause"
            if shot not in result:
                continue
            runs, gaps = result[shot]["ink"], result[shot]["gaps"]
            # Tray, tray, playback, battery.
            assert len(runs) == 4, (shot, runs)
            # The playback slot is laid out by the play art, whose ink is a
            # column wider than the pause art's, so only the tray's gaps.
            assert gaps[:2] == [7, 7], (shot, gaps)
            pause = runs[2]
            for icon in runs[:2]:
                assert icon[2] >= pause[2] and icon[3] <= pause[3], \
                    (shot, icon, pause)
            # The battery's ink and highlight rows; the pause art adds a
            # faint second highlight row below them.
            mochi, battery = runs[0], runs[3]
            assert (mochi[2], mochi[3]) == (battery[2], battery[3]), \
                (shot, mochi, battery)
        for theme in FLAT_THEMES:
            for count in counts:
                for state in ("play", "pause"):
                    shot = f"{theme}-tray{count}-{state}"
                    if shot not in result:
                        continue
                    runs, gaps = result[shot]["ink"], result[shot]["gaps"]
                    # ..., playback, tray icons, battery.
                    assert len(runs) >= count + 2, (shot, runs)
                    # The play icon's tip is a column of faint ink.
                    tip = 8 if state == "play" else FLAT_GAP
                    assert gaps[-count - 1:] == \
                        [tip] + [FLAT_GAP] * count, (shot, gaps)
                    playback = runs[-count - 2]
                    assert playback[3] - playback[2] + 1 == 14, \
                        (shot, playback)
                    if count:
                        mochi = runs[-count - 1]
                        height = STATUS_HEIGHT[theme]
                        assert (mochi[2], mochi[3]) == \
                            ((height - 12) // 2, (height - 12) // 2 + 11), \
                            (shot, mochi)
        themes = args.themes.split(",")
        if set(themes) & set(ART_THEMES):
            print("PASS tray icons on the playback icon's rows, "
                  "7 clear columns")
        if set(themes) & set(FLAT_THEMES):
            print("PASS tray against the battery, playback left of it, "
                  f"{FLAT_GAP} clear columns")


if __name__ == "__main__":
    main()
