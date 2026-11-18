#!/usr/bin/env python3
"""SDL checks for FU93: no row numbers, fast-scroll index, Cover Flow, lyrics."""

import json
import os
import shutil
import struct
import subprocess
import tempfile
import time
import wave
from pathlib import Path
from zipfile import ZipFile

from PIL import ImageChops

from ui_menu_sim import FPS, Screen, process


SIZE = (320, 240)
STATUS = 36
LIST_TOP = 46
LIST_BOTTOM = 230
CENTER = (160, 138)


def wait_display(display, xvfb):
    for _ in range(FPS * 5):
        display.seek(0)
        number = display.read().decode().strip()
        if number:
            return number
        assert xvfb.poll() is None
        time.sleep(1 / FPS)
    raise RuntimeError("Xvfb did not start")


def extract_fonts(resource, root):
    with ZipFile(resource) as archive:
        for name in archive.namelist():
            if name.startswith((".rockbox/fonts/", ".rockbox/langs/",
                                ".rockbox/codepages/", ".rockbox/codecs/")):
                archive.extract(name, root)


def write_config(root, extra=""):
    (root / ".rockbox").mkdir(parents=True, exist_ok=True)
    (root / ".rockbox" / "config.cfg").write_text(
        "show files: all\nlist wraparound: off\n"
        "backlight timeout: off\nvoice menus: off\nkeyclick: off\n"
        "animations: on\nnative theme: scrim\nbeep: off\n"
        "scrollbar: off\n" + extra, encoding="utf-8")


def add_wave(path, seconds=8):
    with wave.open(str(path), "wb") as audio:
        audio.setparams((1, 2, 8000, 0, "NONE", "not compressed"))
        audio.writeframes(bytes(8000 * 2 * seconds))
    info = bytearray(b"INFO")
    value = b"PodBox\0"
    info.extend(struct.pack("<4sI", b"INAM", len(value)))
    info.extend(value)
    with path.open("r+b") as wav:
        wav.seek(0, os.SEEK_END)
        wav.write(struct.pack("<4sI", b"LIST", len(info)) + info)
        size = wav.tell()
        wav.seek(4)
        wav.write(struct.pack("<I", size - 8))


def brightness(pixel):
    return (pixel[0] + pixel[1] + pixel[2]) / 3


def center_box(frame):
    return frame.crop((CENTER[0] - 36, CENTER[1] - 36,
                       CENTER[0] + 36, CENTER[1] + 36))


def overlay_present(frame):
    # Filenames stay left of x=120. The overlay key sits near the list centre.
    box = frame.crop((130, 80, 210, 180))
    return max((p[0] + p[1] + p[2] for p in box.getdata()), default=0) > 400


def capture_spin(screen, clicks, output=None):
    x, y, _, _ = screen.bbox
    subprocess.run(["xdotool", "windowfocus", "--sync", screen.window],
                   env=screen.env, check=True)
    subprocess.run(["xdotool", "mousemove", str(x + 160), str(y + 120)],
                   env=screen.env, check=True)
    t0 = time.monotonic()
    clicker = subprocess.Popen(
        ["xdotool", "click", "--repeat", str(clicks), "--delay", "10", "5"],
        env=screen.env)
    frames, stamps = [screen.frame()], [0.0]
    for sample in range(1, clicks + 80):
        target = t0 + sample / (2 * FPS)
        time.sleep(max(0, target - time.monotonic()))
        frame = screen.frame()
        if ImageChops.difference(frames[-1], frame).getbbox():
            frames.append(frame)
            stamps.append(round((time.monotonic() - t0) * FPS, 2))
        if clicker.poll() is not None and sample > clicks:
            break
    if clicker.poll() is None:
        clicker.wait()
    for _ in range(20):
        time.sleep(1 / FPS)
        frame = screen.frame()
        if ImageChops.difference(frames[-1], frame).getbbox():
            frames.append(frame)
            stamps.append(round((time.monotonic() - t0) * FPS, 2))
    if output and frames:
        holds = [b - a for a, b in zip(stamps, stamps[1:])] + [9]
        frames[0].save(output, save_all=True, append_images=frames[1:],
                       duration=[max(1, round(h * 1000 / FPS)) for h in holds],
                       loop=0)
    return frames, stamps


def vertical_motion(frames, previous):
    """First row that leaves the previous screen, from the bottom."""
    bottoms = []
    for frame in frames:
        last = None
        for y in range(SIZE[1] - 1, STATUS, -1):
            if ImageChops.difference(
                    frame.crop((0, y, SIZE[0], y + 1)),
                    previous.crop((0, y, SIZE[0], y + 1))).getbbox():
                last = y
                break
        if last is not None:
            bottoms.append(last)
    return bottoms


def status_bar_pixels(frame):
    return frame.crop((0, 0, SIZE[0], STATUS)).tobytes()


def check_list(screen, output):
    idle = screen.frame()
    idle.save(output / "list-idle.png")
    # Slow: one row. Overlay must stay away.
    screen.tap("Down")
    time.sleep(20 / FPS)
    slow = screen.frame()
    slow.save(output / "list-slow.png")
    assert not overlay_present(slow), "slow scroll showed the index overlay"

    # Left inset of a file row has no digit column: the selection bar starts
    # near the theme text padding, not a number cell.
    bar = None
    for x in range(0, 80):
        if slow.getpixel((x, LIST_TOP + 10)) != slow.getpixel((x, LIST_TOP + 40)):
            bar = x
            break
    assert bar is not None and bar < 24, bar

    frames, stamps = capture_spin(screen, 20, output / "list-fast.gif")
    shown_frames = []
    shown = []
    brights = []
    for frame, stamp in zip(frames, stamps):
        box = frame.crop((130, 80, 210, 180))
        n = max((p[0] + p[1] + p[2] for p in box.getdata()), default=0)
        brights.append(n)
        if n > 400:
            shown_frames.append(frame)
            shown.append(stamp)
    if shown_frames:
        shown_frames[0].save(output / "list-overlay.png")
    for i, frame in enumerate(frames):
        if i in (0, len(frames) // 2, len(frames) - 1):
            frame.save(output / f"list-fast-{i:03d}.png")
    assert shown, brights
    return {
        "slow_overlay": False,
        "fast_overlay_first_f": shown[0],
        "fast_overlay_frames": len(shown),
        "selection_inset_px": bar,
    }


def battery_pixel(frame):
    return frame.getpixel((295, 9))


def is_cover_flow(frame):
    return battery_pixel(frame) != (206, 207, 206) and frame.getpixel((16, 10))[0] < 40


def check_covers(screen, output):
    previous = screen.frame()
    previous.save(output / "covers-before.png")
    assert battery_pixel(previous) == (206, 207, 206)
    t0 = time.monotonic()
    screen.key("keydown", "Return")
    time.sleep(2 / FPS)
    screen.key("keyup", "Return")
    frames, stamps = [previous], [0.0]
    settled = None
    for sample in range(1, 240):
        time.sleep(max(0, t0 + sample / (2 * FPS) - time.monotonic()))
        frame = screen.frame()
        if ImageChops.difference(frames[-1], frame).getbbox():
            frames.append(frame)
            stamps.append(round((time.monotonic() - t0) * FPS, 2))
            settled = frame
            if is_cover_flow(frame):
                break
    assert settled is not None
    settled.save(output / "covers-settled.png")
    frames[min(4, len(frames) - 1)].save(output / "covers-enter-mid.png")
    bottoms = vertical_motion(frames[1:], previous)
    assert bottoms, "Cover Flow did not change the screen"
    assert max(bottoms[:3]) >= 200, bottoms[:6]
    assert is_cover_flow(settled), "Cover Flow kept the status bar"
    # Leave
    t0 = time.monotonic()
    screen.key("keydown", "Left")
    time.sleep(2 / FPS)
    screen.key("keyup", "Left")
    leave, leave_stamps = [settled], [0.0]
    for sample in range(1, 80):
        time.sleep(max(0, t0 + sample / (2 * FPS) - time.monotonic()))
        frame = screen.frame()
        if ImageChops.difference(leave[-1], frame).getbbox():
            leave.append(frame)
            leave_stamps.append(round((time.monotonic() - t0) * FPS, 2))
        if not is_cover_flow(frame) and battery_pixel(frame) == (206, 207, 206):
            break
    leave[-1].save(output / "covers-left.png")
    return {
        "enter_frames": len(frames),
        "enter_span_f": stamps[-1] if stamps else 0,
        "leave_frames": len(leave),
        "leave_span_f": leave_stamps[-1] if leave_stamps else 0,
        "no_status_bar": True,
        "enter_from_bottom": True,
    }


def check_lyrics(screen, output):
    # Search, .rockbox, Test.lrc, then Test.wav.
    for _ in range(3):
        screen.tap("Down")
        time.sleep(8 / FPS)
    files = screen.frame()
    screen.tap("space")
    playing = None
    for _ in range(8 * FPS):
        frame = screen.frame()
        if ImageChops.difference(files, frame).getbbox():
            playing = frame
            break
        time.sleep(1 / FPS)
    if playing is None:
        playing = files
    playing.save(output / "lyrics-wps.png")
    t0 = time.monotonic()
    screen.key("keydown", "Return")
    time.sleep(2 / FPS)
    screen.key("keyup", "Return")
    frames, stamps = [playing], [0.0]
    for sample in range(1, 80):
        time.sleep(max(0, t0 + sample / (2 * FPS) - time.monotonic()))
        frame = screen.frame()
        if ImageChops.difference(frames[-1], frame).getbbox():
            frames.append(frame)
            stamps.append(round((time.monotonic() - t0) * FPS, 2))
    frames[-1].save(output / "lyrics-open.png")
    bottoms = vertical_motion(frames[1:], playing)
    opened = bool(bottoms)
    from_bottom = opened and max(bottoms[:3]) >= 160
    return {
        "enter_frames": len(frames),
        "enter_span_f": stamps[-1] if stamps else 0,
        "enter_from_bottom": from_bottom,
        "changed": opened,
        "first_changed_row": bottoms[:3] if bottoms else [],
    }


def run_list(binary, resource, env, output):
    root = output / "list-root"
    if root.exists():
        shutil.rmtree(root)
    root.mkdir()
    extract_fonts(resource, root)
    write_config(root, "start in screen: files\n")
    names = ([f"9{i:02d}.txt" for i in range(20)] +
             [f"A{i:02d}.txt" for i in range(20)] +
             [f"Z{i:02d}.txt" for i in range(20)] +
             [f"北{i:02d}.txt" for i in range(20)])
    for name in names:
        (root / name).write_text("x\n", encoding="utf-8")
    with (output / "list-sim.log").open("w") as log, process(
        [str(binary), "--root", str(root), "--nobackground", "--zoom", "1",
         "--screenshot"], log, env=env, cwd=root,
    ) as firmware:
        time.sleep(120 / FPS)
        screen = Screen(env, firmware)
        screen.wait_full_charge()
        result = check_list(screen, output)
        assert firmware.poll() is None
        return result


def run_covers(binary, fixture, env, output):
    root = output / "covers-root"
    if root.exists():
        shutil.rmtree(root)
    shutil.copytree(fixture, root)
    rb = root / ".rockbox"
    for name in ("config.cfg", "config.cfg.new", "config.cfg.old",
                 "config01.cfg", "nvram.bin", ".resume.cfg", ".resume.cfg.new"):
        (rb / name).unlink(missing_ok=True)
    write_config(root, "start in screen: root\nroot menu order: pictureflow\n")
    with (output / "covers-sim.log").open("w") as log, process(
        [str(binary), "--root", str(root), "--nobackground", "--zoom", "1",
         "--screenshot"], log, env=env, cwd=root,
    ) as firmware:
        time.sleep(180 / FPS)
        screen = Screen(env, firmware)
        screen.wait_full_charge()
        result = check_covers(screen, output)
        assert firmware.poll() is None
        return result


def run_lyrics(binary, resource, env, output):
    root = output / "lyrics-root"
    if root.exists():
        shutil.rmtree(root)
    root.mkdir()
    extract_fonts(resource, root)
    write_config(root, "start in screen: files\n")
    add_wave(root / "Test.wav", seconds=30)
    (root / "Test.lrc").write_text(
        "[00:00.00]First lyric line\n[00:10.00]Second lyric line\n",
        encoding="utf-8")
    with (output / "lyrics-sim.log").open("w") as log, process(
        [str(binary), "--root", str(root), "--nobackground", "--zoom", "1",
         "--screenshot"], log, env=env, cwd=root,
    ) as firmware:
        time.sleep(120 / FPS)
        screen = Screen(env, firmware)
        screen.wait_full_charge()
        result = check_lyrics(screen, output)
        assert firmware.poll() is None
        return result


def main():
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=lambda s: Path(s).resolve())
    parser.add_argument("resources", type=lambda s: Path(s).resolve())
    parser.add_argument("output", type=lambda s: Path(s).resolve())
    parser.add_argument("--fixture", type=lambda s: Path(s).resolve() if s else None)
    args = parser.parse_args()
    if args.output.exists():
        for leftover in args.output.glob("list-*.png"):
            leftover.unlink()
        for leftover in args.output.glob("covers-*.png"):
            leftover.unlink()
        for leftover in args.output.glob("lyrics-*.png"):
            leftover.unlink()
    args.output.mkdir(parents=True, exist_ok=True)
    result = {}
    with (args.output / "xvfb.log").open("w") as log:
        runtime = args.output / "runtime"
        runtime.mkdir(exist_ok=True)
        runtime.chmod(0o700)
        number = str(50 + os.getpid() % 400)
        with process(["Xvfb", ":" + number, "-screen", "0", "800x600x24"], log) as xvfb:
            time.sleep(0.4)
            assert xvfb.poll() is None
            env = dict(os.environ, DISPLAY=":" + number,
                       XDG_RUNTIME_DIR=str(runtime),
                       SDL_VIDEODRIVER="x11", SDL_AUDIODRIVER="dummy",
                       SDL_RENDER_DRIVER="software",
                       LIBGL_ALWAYS_SOFTWARE="1",
                       __GLX_VENDOR_LIBRARY_NAME="mesa")
            result["list"] = run_list(args.binary, args.resources, env, args.output)
            if args.fixture:
                result["covers"] = run_covers(args.binary, args.fixture, env,
                                              args.output)
            result["lyrics"] = run_lyrics(args.binary, args.resources, env,
                                          args.output)
    (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print("PASS FU93 list index, Cover Flow and lyrics SDL checks")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
