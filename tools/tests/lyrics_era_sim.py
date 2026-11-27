#!/usr/bin/env python3
"""Render the lyrics page in the SDL simulator and check its frame pacing.

Usage: lyrics_era_sim.py BUILD OUT [--themes classic,fruit-company] [--clip]

BUILD is a bundled simulator build directory (build-sim.sh): rockboxui and a
rockbox.zip carrying the native fonts and stock-art.bin. For each theme the
page is opened on three tracks -- timed LRC, plain text and no lyrics -- and
OUT receives a screenshot of each, frames of a line change and of a wheel
scroll, and the page's own lyrics-perf.txt and ui-perf.txt. The run fails if
a line change or wheel scroll takes over 33 ms at p95, if more than one frame
in twenty misses the 30 fps budget (the recorder's "dropped" count), or if
the animation presents fewer than 25 frames a second.

Xvfb is private to this process; nothing opens on the desktop. Run it alone:
two simulators at once skew each other's timing.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time
import wave
from zipfile import ZipFile

from PIL import Image, ImageChops

from ui_menu_sim import Screen, process

# One line every two seconds; the third wraps, the fourth and fifth time
# each word, and the fifth is CJK.
SYNCED = """[ti:Era Test]
[00:00.00]First light over the water
[00:02.00]Every road comes home tonight
[00:04.00]A longer line that has to wrap across the narrow screen of the player
[00:06.00]<00:06.00>Hold <00:06.40>the <00:06.70>signal, <00:07.20>hold <00:07.50>it <00:07.70>close
[00:08.00]<00:08.00>光在<00:08.60>水面上<00:09.20>慢慢<00:09.60>展开
[00:10.00]Turn the wheel and find the line
[00:12.00]Silver sleeve and paper sky
[00:14.00]One more chorus before the end
[00:16.00]Quiet now, the record spins
[00:18.00]Last line of the fixture
"""

PLAIN = """Plain lyrics carry no time
so the page shows them as text
and the wheel moves through them

Second verse begins here
with a line that is long enough to wrap on the screen
and a short one
"""

CASES = (("synced", "Synced"), ("plain", "Plain"), ("none", "Silent"))


def write_wav(path, seconds):
    with wave.open(str(path), "wb") as audio:
        audio.setparams((1, 2, 22050, 0, "NONE", "not compressed"))
        audio.writeframes(bytes(22050 * 2 * seconds))


def prepare(build, root, theme, track):
    with ZipFile(build / "rockbox.zip") as archive:
        archive.extractall(root)
    rockbox = root / ".rockbox"
    for name in ("config01.cfg", "nvram.bin", "lyrics-perf.txt", "ui-perf.txt"):
        (rockbox / name).unlink(missing_ok=True)
    (rockbox / "ui-perf.enable").write_text("")
    (rockbox / "config.cfg").write_text(
        f"native theme: {theme}\nstart in screen: files\nshow files: music\n"
        "backlight timeout: off\nvoice menus: off\nanimations: on\n"
        "lyrics lead: 0\nlyric align: centre\n")
    music = root / "Music"
    music.mkdir()
    write_wav(music / f"{track}.wav", 40)
    if track == "Synced":
        (music / "Synced.lrc").write_text(SYNCED)
    elif track == "Plain":
        (music / "Plain.txt").write_text(PLAIN)


def changed(a, b):
    return ImageChops.difference(a, b).getbbox() is not None


def burst(screen, seconds, out, prefix, keys=()):
    """Grab frames as fast as the X server allows, pressing `keys` as
    (time, key) pairs on the way; keep the ones that changed."""
    start = time.monotonic()
    pending = list(keys)
    frames, last = [], None
    while time.monotonic() - start < seconds:
        now = time.monotonic() - start
        while pending and pending[0][0] <= now:
            screen.tap(pending.pop(0)[1])
        frame = screen.frame()
        if last is None or changed(last, frame):
            frames.append((round(now, 3), frame))
            last = frame
    for i, (stamp, frame) in enumerate(frames):
        frame.save(out / f"{prefix}-{i:03d}.png")
    return [stamp for stamp, _ in frames]


def perf_block(text, name):
    match = re.search(re.escape(name) + r": (\d+) (\d+)/(\d+)/(\d+)", text)
    return dict(zip(("count", "avg_us", "p95_us", "max_us"), map(int, match.groups())))


def parse_lyrics_perf(text):
    session = text.rsplit("Session ", 1)[-1]
    result = {"timing": re.search(r"^Track: (Timed|Untimed)$", session, re.M).group(1)}
    for key, pattern in (("frames", r"frames: (\d+)"),
                         ("dropped", r"dropped \(30 fps\): (\d+)"),
                         ("animation_fps", r"animation fps: (\d+)")):
        result[key] = int(re.search(pattern, session).group(1))
    for name in ("restore", "text/glyph", "LCD", "steady", "entry",
                 "line change", "manual scroll"):
        result[name] = perf_block(session, name)
    return result


def run_case(build, out, theme, case, track, clip):
    directory = out / theme / case
    directory.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="lyrics-era-") as temp, \
            tempfile.TemporaryFile() as display, \
            (directory / "xvfb.log").open("w") as xlog, \
            (directory / "sim.log").open("w") as log:
        root = Path(temp) / "disk"
        root.mkdir()
        prepare(build, root, theme, track)
        runtime = Path(temp) / "runtime"
        runtime.mkdir(mode=0o700)
        with process(["Xvfb", "-displayfd", str(display.fileno()), "-screen", "0",
                      "800x600x24", "-nolisten", "tcp"], xlog,
                     pass_fds=(display.fileno(),)) as xvfb:
            for _ in range(200):
                display.seek(0)
                number = display.read().decode().strip()
                if number:
                    break
                assert xvfb.poll() is None, "Xvfb exited"
                time.sleep(.025)
            else:
                raise AssertionError("Xvfb did not start")
            env = dict(os.environ, DISPLAY=":" + number, SDL_VIDEODRIVER="x11",
                       SDL_AUDIODRIVER="dummy", SDL_RENDER_DRIVER="software",
                       XDG_RUNTIME_DIR=str(runtime))
            with process([str(build / "rockboxui"), "--root", str(root),
                          "--nobackground", "--zoom", "1", "--screenshot"],
                         log, env=env, cwd=root) as player:
                time.sleep(2.5)
                screen = Screen(env, player)
                screen.frame().save(directory / "boot.png")
                # Files: Music, then its one track.
                screen.tap("Return")
                time.sleep(.8)
                screen.tap("Return")
                time.sleep(1.2)
                screen.frame().save(directory / "wps.png")
                stamps = {"entry": burst(screen, 1.0, directory, "entry",
                                         [(0.05, "Return")])}
                screen.frame().save(directory / "lyrics.png")
                if case == "synced":
                    # Wait for the next two-second line boundary.
                    stamps["line"] = burst(screen, 2.6, directory, "line")
                    if clip:
                        stamps["clip"] = burst(screen, 6.0, directory, "clip")
                    time.sleep(.3)
                if case != "none":
                    keys = [(0.1, "Down"), (0.25, "Down"), (0.4, "Down"),
                            (1.0, "Up")]
                    stamps["wheel"] = burst(screen, 1.8, directory, "wheel", keys)
                    screen.frame().save(directory / "browsed.png")
                stamps["exit"] = burst(screen, 1.0, directory, "exit",
                                       [(0.05, "Escape")])
                screen.frame().save(directory / "returned.png")
                assert player.poll() is None, "simulator exited"
        rockbox = root / ".rockbox"
        for name in ("lyrics-perf.txt", "ui-perf.txt"):
            if (rockbox / name).exists():
                shutil.copyfile(rockbox / name, directory / name)
    perf = parse_lyrics_perf((directory / "lyrics-perf.txt").read_text())
    result = {"perf": perf, "captured": {k: len(v) for k, v in stamps.items()}}
    (directory / "stamps.json").write_text(json.dumps(stamps) + "\n")
    if theme != "scrim":
        result["blank_times"] = blank_times(directory)
    (directory / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def blank_times(directory):
    """Frames of the page whose elapsed or remaining time has no ink.
    style.lyrics=classic draws both on every frame."""
    blank = []
    for path in sorted(directory.glob("[lcw]*-*.png")):
        frame = Image.open(path).convert("L")
        for left, right in ((12, 48), (270, 312)):
            ink = sum(frame.getpixel((x, y)) < 100
                      for x in range(left, right) for y in range(206, 224))
            if ink < 5:
                blank.append(path.name)
                break
    return blank


def check(theme, case, result):
    perf = result["perf"]
    problems = []
    if result.get("blank_times"):
        problems.append(f"time missing in {', '.join(result['blank_times'])}")
    expected = "Timed" if case == "synced" else "Untimed"
    if perf["timing"] != expected:
        problems.append(f"timing {perf['timing']}, expected {expected}")
    animated = perf["line change"]["count"] + perf["manual scroll"]["count"]
    # The host schedules the simulator; allow it the odd late wake-up.
    if perf["dropped"] > max(1, animated // 20):
        problems.append(f"{perf['dropped']} of {animated} frames over the 30 fps budget")
    for name in ("line change", "manual scroll"):
        if perf[name]["p95_us"] > 33000:
            problems.append(f"{name} p95 {perf[name]['p95_us']} us")
    if case != "none" and animated and perf["animation_fps"] < 25:
        problems.append(f"animation at {perf['animation_fps']} fps")
    return [f"{theme}/{case}: {p}" for p in problems]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=lambda p: Path(p).resolve())
    parser.add_argument("out", type=lambda p: Path(p).resolve())
    parser.add_argument("--themes", default="classic,fruit-company")
    parser.add_argument("--cases", default="synced,plain,none")
    parser.add_argument("--clip", action="store_true")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    results, problems = {}, []
    wanted = args.cases.split(",")
    for theme in args.themes.split(","):
        for case, track in CASES:
            if case not in wanted:
                continue
            result = run_case(args.build, args.out, theme, case, track, args.clip)
            results[f"{theme}/{case}"] = result
            problems += check(theme, case, result)
            print(theme, case, json.dumps(result["perf"]))
    (args.out / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    for problem in problems:
        print("FAIL", problem)
    print("PASS" if not problems else f"{len(problems)} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    raise SystemExit(main())
