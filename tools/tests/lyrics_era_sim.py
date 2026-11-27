#!/usr/bin/env python3
"""Render the lyrics page in the SDL simulator and check its frame pacing.

Usage: lyrics_era_sim.py BUILD OUT [--themes classic,fruit-company,fruit-company-ultra,scrim] [--clip]

BUILD is a bundled simulator build directory (build-sim.sh): rockboxui and a
rockbox.zip carrying the native fonts and stock-art.bin. For each theme the
page is opened on three tracks -- timed LRC, plain text and no lyrics -- and
OUT receives screenshots, frame dumps of entry, exit, line changes and
wheel scrolling, and the lyrics-perf.txt and ui-perf.txt traces. The stock
themes must keep the status bar and progress strip in place. In the
paused round trip, both strips must match pixel for pixel and the moving
body must retrace its steps. During playback, only the times and bar fill
may change in the progress strip. The run fails if a line change or wheel scroll takes over 33 ms at p95, if more than one frame
in twenty misses the 30 fps budget (the recorder's "dropped" count), or if
the animation presents fewer than 25 frames a second.

The SDL dummy driver opens no window. Run one simulator at a time for
comparable timings.
"""
import argparse
import json
from pathlib import Path
import re
import shutil
import struct
import tempfile
import time
import wave
from zipfile import ZipFile

from PIL import Image, ImageDraw

from ui_quickscreen_sim import ULTRA_FALLBACK_THEME
from ui_stock_transitions_sim import FPS, H, W, Sim

STOCK_BAR = (0, 0, W, 20)
BODY = (0, STOCK_BAR[3], W, 203)
PROGRESS = (0, BODY[3], W, H)
LIVE_REGIONS = ((15, 208, 54, 225), (54, 207, 266, 233), (256, 208, 310, 225))

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


    # RIFF INFO tags let the test see all three moving WPS labels.
    def chunk(tag, data):
        return tag + struct.pack("<I", len(data)) + data + bytes(len(data) % 2)

    tags = b"INFO" + b"".join(chunk(tag, value + b"\0") for tag, value in (
        (b"INAM", path.stem.encode()), (b"IART", b"Silver Sleeve"),
        (b"IPRD", b"Paper Sky"),
    ))
    data = path.read_bytes() + chunk(b"LIST", tags)
    path.write_bytes(data[:4] + struct.pack("<I", len(data) - 8) + data[8:])


def prepare(build, root, theme, track):
    with ZipFile(build / "rockbox.zip") as archive:
        archive.extractall(root)
    rockbox = root / ".rockbox"
    for name in ("config01.cfg", "nvram.bin", "lyrics-perf.txt", "ui-perf.txt"):
        (rockbox / name).unlink(missing_ok=True)
    if theme == "fruit-company-ultra":
        themes = rockbox / "native-themes"
        themes.mkdir(exist_ok=True)
        (themes / "fruit-company-ultra.podtheme").write_text(
            ULTRA_FALLBACK_THEME + "style.lyrics=classic\n")
    (rockbox / "ui-perf.enable").write_text("")
    (rockbox / "config.cfg").write_text(
        f"native theme: {theme}\nstart in screen: files\nshow files: music\n"
        "backlight timeout: off\nvoice menus: off\nanimations: on\n"
        "lyrics lead: 0\nlyric align: centre\n")
    music = root / "Music"
    music.mkdir()
    write_wav(music / f"{track}.wav", 20)
    sleeve = Image.new("RGB", (240, 240), "#193049")
    draw = ImageDraw.Draw(sleeve)
    draw.rectangle((24, 25, 216, 215), outline="#f3cc81", width=6)
    draw.polygon(((27, 177), (112, 61), (213, 175)), fill="#dc7152")
    draw.ellipse((88, 122, 176, 210), fill="#81c9b1")
    sleeve.save(music / "cover.jpg", quality=95)
    if track == "Synced":
        (music / "Synced.lrc").write_text(SYNCED)
    elif track == "Plain":
        (music / "Plain.txt").write_text(PLAIN)


def latest_frame(sim):
    count, frames = sim.frames_since(0)
    assert frames, "no frame reached the panel"
    return count, frames[-1]


def burst(sim, seconds, out, prefix, keys=()):
    """Read every presented frame, retaining the distinct consecutive images."""
    seen, last = latest_frame(sim)
    frames = [(0.0, last)]
    edges = sorted((at + delay, f"{edge} {key}") for at, key in keys
                   for delay, edge in ((0, "down"), (2 / FPS, "up")))
    pending = iter(edges)
    edge = next(pending, None)
    start = time.monotonic()
    while (now := time.monotonic() - start) < seconds:
        while edge and edge[0] <= now:
            sim.send(edge[1])
            edge = next(pending, None)
        count, got = sim.frames_since(seen)
        assert len(got) == count - seen, f"missed simulator frames during {prefix}"
        seen = count
        for pixels in got:
            if pixels != last:
                frames.append((round(now, 4), pixels))
                last = pixels
        time.sleep(.002)
    assert sim.process.poll() is None, "simulator exited"
    for i, (_, pixels) in enumerate(frames):
        Image.frombytes("RGB", (W, H), pixels).save(out / f"{prefix}-{i:03d}.png")
    return [stamp for stamp, _ in frames]


def frames_for(directory, prefix):
    frames = []
    for path in sorted(directory.glob(f"{prefix}-*.png")):
        with Image.open(path) as image:
            frames.append((path.name, image.copy()))
    return frames


def strip_without_live_fields(frame):
    strip = frame.crop(PROGRESS)
    for x0, y0, x1, y1 in LIVE_REGIONS:
        strip.paste(0, (x0, y0 - PROGRESS[1], x1, y1 - PROGRESS[1]))
    return strip.tobytes()


def motion_frames(frames):
    body = [frame.crop(BODY).tobytes() for _, frame in frames]
    # A real indicator change after the endpoint belongs to the steady page.
    return frames[:body.index(body[-1]) + 1]


def transition_frames(frames, paused):
    frames = motion_frames(frames)
    first = frames[0][1]
    bar = first.crop(STOCK_BAR).tobytes()
    strip = (lambda frame: frame.crop(PROGRESS).tobytes()) if paused else (
        strip_without_live_fields)
    body = [frame.crop(BODY).tobytes() for _, frame in frames]
    intermediate = [frame for (_, frame), pixels in zip(frames, body)
                    if pixels not in (body[0], body[-1])]
    return {
        "checked_frames": len(frames),
        "status_changed": [name for name, frame in frames
                           if frame.crop(STOCK_BAR).tobytes() != bar],
        "progress_changed": [name for name, frame in frames
                             if strip(frame) != strip(first)],
        "intermediate_body_frames": len({pixels for pixels in body
                                         if pixels not in (body[0], body[-1])}),
        "live_bar_states": len({frame.crop(LIVE_REGIONS[1]).tobytes()
                                for frame in intermediate}),
    }


def unique_body(frames):
    return list(dict.fromkeys(frame.crop(BODY).tobytes() for _, frame in frames))


def reversed_bodies(entry, exit):
    forward, reverse = unique_body(entry), list(reversed(unique_body(exit)))
    # A scheduled host frame can be skipped in either direction. Check every
    # common pose in order, including the two endpoints.
    common = set(forward) & set(reverse)
    return {"matched": len(common), "forward": len(forward), "reverse": len(reverse),
            "in_order": ([body for body in forward if body in common] ==
                         [body for body in reverse if body in common]),
            "endpoints_match": forward[0] == reverse[0] and forward[-1] == reverse[-1]}


def transition_strip(directory, entry, exit):
    # Pair matching recorded poses when the two rails and body are fixed.
    # Scrim retains its own entry and exit, so its rows sample independently.
    entry, exit = motion_frames(entry), motion_frames(exit)
    reverse = {frame.crop(BODY).tobytes(): (name, frame) for name, frame in exit}
    pairs = [(name, frame, reverse[body]) for name, frame in entry
             if (body := frame.crop(BODY).tobytes()) in reverse]
    if len(pairs) >= 7:
        entry = [(name, frame) for name, frame, _ in pairs]
        exit = [other for _, _, other in reversed(pairs)]
    out = Image.new("RGB", (W * 7, H * 2))
    for y, frames in enumerate((entry, exit)):
        for i in range(7):
            _, frame = frames[round(i * (len(frames) - 1) / 6)]
            out.paste(frame, (i * W, y * H))
    out.save(directory / "transition.png")


def transition_perf(text):
    result = {}
    for line in text.splitlines():
        if line.startswith(("route=lyrics-up ", "route=lyrics-down ")):
            fields = dict(pair.split("=", 1) for pair in line.split())
            period = int(fields["period_us"])
            result[fields["route"]] = {
                "presented": int(fields["seen"]),
                "work_p99_ms": round(float(fields["work_p99_f"]) * period / 1000, 3),
                "work_max_ms": round(float(fields["work_worst_f"]) * period / 1000, 3),
                "render_max_ms": round(float(fields["render_worst_f"]) * period / 1000, 3),
                "interval_p99_ms": round(float(fields["interval_p99_f"]) * period / 1000, 3),
                "interval_max_ms": round(float(fields["interval_worst_f"]) * period / 1000, 3),
                "work_misses": int(fields["work_misses"]),
                "interval_misses": int(fields["interval_misses"]),
            }
    return result


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


def record_entry(sim, directory, prefix, duration, fruit):
    if not fruit:
        return burst(sim, duration, directory, prefix, [(.05, "select")])

    _, pixels = latest_frame(sim)
    wps_body = Image.frombytes("RGB", (W, H), pixels).crop(BODY).tobytes()
    sim.send("down select")
    sim.wait(54)
    sim.send("up select")
    sim.wait(30)
    sim.tap("down", 15)
    stamps = burst(sim, duration, directory, prefix, [(.05, "select")])

    # The context menu closes before the restored WPS starts the transform.
    frames = frames_for(directory, prefix)
    first = next((i for i, (_, frame) in enumerate(frames)
                  if frame.crop(BODY).tobytes() == wps_body), None)
    assert first is not None, "context menu did not return to WPS before Lyrics"
    for name, _ in frames[:first]:
        (directory / name).unlink()
    return stamps[first:]


def run_case(build, out, theme, case, track, clip):
    directory = out / theme / case
    directory.mkdir(parents=True, exist_ok=True)
    # Use fresh filenames so a shorter recording cannot pick up old frames.
    for prefix in ("entry", "exit", "paused-entry", "paused-exit",
                   "line", "clip", "wheel"):
        for path in directory.glob(f"{prefix}-*.png"):
            path.unlink()
    with tempfile.TemporaryDirectory(prefix="lyrics-era-") as temp:
        root = Path(temp) / "disk"
        root.mkdir()
        prepare(build, root, theme, track)
        sim = Sim(build / "rockboxui", root, directory)
        try:
            def screenshot(name):
                _, pixels = latest_frame(sim)
                Image.frombytes("RGB", (W, H), pixels).save(directory / f"{name}.png")

            # The screenshot battery voltage is fixed but its filter needs
            # several seconds to reach that voltage after boot.
            sim.wait(360)
            screenshot("boot")
            # Files: Music, then its one track.
            sim.tap("select", 48)
            sim.tap("select", 72)
            screenshot("wps")
            fruit = theme.startswith("fruit-company")
            stamps = {}
            if case == "synced":
                # The paused document has an identical body in both directions.
                sim.tap("play", 20)
                stamps["paused-entry"] = record_entry(
                    sim, directory, "paused-entry", 1.0, fruit)
                stamps["paused-exit"] = burst(sim, .8, directory, "paused-exit",
                                               [(.05, "menu")])
                sim.tap("play", 10)
            stamps["entry"] = record_entry(sim, directory, "entry", 1.0, fruit)
            screenshot("lyrics")
            if case == "synced":
                # Wait for the next two-second line boundary.
                stamps["line"] = burst(sim, 2.6, directory, "line")
                if clip:
                    stamps["clip"] = burst(sim, 6.0, directory, "clip")
                sim.wait(18)
            if case != "none":
                keys = [(0.1, "down"), (0.25, "down"), (0.4, "down"),
                        (1.0, "up")]
                stamps["wheel"] = burst(sim, 1.8, directory, "wheel", keys)
                screenshot("browsed")
            stamps["exit"] = burst(sim, .65, directory, "exit", [(.05, "menu")])
            screenshot("returned")
        finally:
            sim.close()
        rockbox = root / ".rockbox"
        for name in ("lyrics-perf.txt", "ui-perf.txt"):
            if (rockbox / name).exists():
                shutil.copyfile(rockbox / name, directory / name)
    perf = parse_lyrics_perf((directory / "lyrics-perf.txt").read_text())
    result = {
        "perf": perf,
        "captured": {k: len(v) for k, v in stamps.items()},
        "transition_perf": transition_perf((directory / "ui-perf.txt").read_text()),
    }
    transition = {name: frames_for(directory, name) for name in stamps
                  if name.endswith(("entry", "exit"))}
    result["transition_frames"] = {
        name: transition_frames(frames, name.startswith("paused-"))
        for name, frames in transition.items()
    }
    if case == "synced":
        entry, exit = transition["paused-entry"], transition["paused-exit"]
        result["reverse"] = reversed_bodies(entry, exit)
        transition_strip(directory, entry, exit)
    (directory / "stamps.json").write_text(json.dumps(stamps) + "\n")
    if theme != "scrim":
        result["blank_times"] = blank_times(directory)
    (directory / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def blank_times(directory):
    """Frames of the page whose elapsed or remaining time has no ink.
    style.lyrics=classic draws both on every frame."""
    blank = []
    for prefix in ("entry", "exit", "paused-entry", "paused-exit",
                   "line", "clip", "wheel"):
        for path in sorted(directory.glob(f"{prefix}-*.png")):
            with Image.open(path) as image:
                frame = image.convert("L")
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
    if theme != "scrim":
        for route in ("lyrics-up", "lyrics-down"):
            trace = result["transition_perf"].get(route)
            if not trace or trace["presented"] < 4:
                problems.append(f"{route} did not present a multi-frame transition")
            elif trace["work_p99_ms"] > 33:
                problems.append(f"{route} work p99 {trace['work_p99_ms']} ms")
        for name, frames in result["transition_frames"].items():
            for region in ("status", "progress"):
                changed = frames[f"{region}_changed"]
                if changed:
                    problems.append(f"{name} changed the {region} strip in {changed}")
            if frames["intermediate_body_frames"] < 3:
                problems.append(f"{name} has fewer than three intermediate body poses")
            if not name.startswith("paused-") and frames["live_bar_states"] < 2:
                problems.append(f"{name} did not advance progress during the move")
        reverse = result.get("reverse")
        if reverse and (not reverse["endpoints_match"] or not reverse["in_order"]
                        or reverse["matched"] < 5):
            problems.append(f"the paused round trip did not retrace its poses: {reverse}")
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
    parser.add_argument("--themes", default="classic,fruit-company,fruit-company-ultra,scrim")
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
            print(theme, case, json.dumps(result["transition_perf"]), flush=True)
    (args.out / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    for problem in problems:
        print("FAIL", problem)
    print("PASS" if not problems else f"{len(problems)} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    raise SystemExit(main())
