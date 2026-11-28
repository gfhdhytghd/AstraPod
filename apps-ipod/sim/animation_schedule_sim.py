#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run matched headless playback, Cover Flow and list motion sessions."""

import argparse
import csv
import json
from pathlib import Path
import shutil
import math
import re
import subprocess
import sys
import time

from PIL import Image

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/tests"))
from ui_music_fixture import build_tagcache, database_count, install_sim_codecs
from ui_stock_transitions_sim import make_fixture, Sim


def fixture(binary, resource, out):
    make_fixture(binary, resource, out)
    play = out / "Play"
    play.mkdir()
    subprocess.run([
        "ffmpeg", "-nostdin", "-loglevel", "error", "-f", "lavfi",
        "-i", "sine=frequency=440:sample_rate=44100:duration=150",
        "-c:a", "flac", "-threads", "1", str(play / "00 Playback.flac"),
    ], check=True)
    source = next((out / "Music").rglob("*.mp3"))
    image = Image.effect_noise((1280, 1280), 45).convert("RGB")
    for n in range(96):
        album = out / "Music" / "Albums" / f"Fixture {n:02}"
        album.mkdir(parents=True)
        image.save(album / "folder.jpg", quality=88)
        subprocess.run([
            "ffmpeg", "-nostdin", "-loglevel", "error", "-i", str(source),
            "-c:a", "copy", "-metadata", f"album=Fixture {n:02}",
            str(album / "01 Song.mp3"),
        ], check=True)
    # Force the staged rebuild to finish before the fixture is copied to each run.
    database = out / ".rockbox" / "library" / "database"
    shutil.rmtree(database)
    assert build_tagcache(binary, out, out.parent / "tagcache-expanded.log") == 121


def add_scan_work(root):
    # The database walks all the paths. Unsupported files exercise scanning
    # without a merge that would intentionally unload the album tables.
    source = root / "Documents" / "unrecognised.file"
    scan = root / "Music" / "Scan"
    for folder in range(30):
        directory = scan / f"Batch {folder:02}"
        directory.mkdir(parents=True)
        sample = directory / "sample.bin"
        shutil.copyfile(source, sample)
        for n in range(2400):
            (directory / f"File {n:04}.bin").hardlink_to(sample)


def add_art_work(root):
    # The existing library remains usable while cold art is generated.
    art = root / ".rockbox" / "library" / "cache" / "art"
    if art.exists():
        shutil.rmtree(art)


def read(path):
    return path.read_text(errors="replace") if path.exists() else ""


def wait_log(sim, path, marker, previous=0, seconds=8):
    deadline = time.monotonic() + seconds
    while read(path).count(marker) <= previous:
        assert time.monotonic() < deadline, f"Missing {marker!r} in {path}"
        sim.wait(0.25)


def workload(sim, phase):
    marker = f"motion load: {'database' if phase == 'database' else 'art cache'} active"
    log = sim.out / "firmware.log"
    previous = read(log).count(marker)
    suffix = "db" if phase == "database" else "art"
    (sim.root / ".rockbox" / f"ui-motion-{suffix}.request").touch()
    wait_log(sim, log, marker, previous)


def summarize(perf_log, phase):
    counts = {route: {"intervals": [], "loaded_frames": 0,
                      "busy_at_submit": 0, "loaded_pcm_chunks": 0}
              for route in ("list-glide", "cover-flow")}
    worker = "db" if phase == "database" else "art"
    for capture in read(perf_log).split("route=")[1:]:
        summary, *rows = capture.splitlines()
        route = summary.split()[0]
        if route not in counts or not rows:
            continue
        fields = dict(item.split("=", 1) for item in summary.split()[1:])
        stat = counts[route]
        period = int(fields["period_us"]) / 1000
        for row in csv.DictReader(rows):
            if row["pcm_active"] != "1":
                continue
            value = float(row["interval_f"]) * period
            if value:
                stat["intervals"].append(value)
            if row[f"{worker}_busy"] == "1":
                stat["busy_at_submit"] += 1
            if row[f"{worker}_overlap"] == "1":
                stat["loaded_frames"] += 1
                stat["loaded_pcm_chunks"] += int(row["pcm_chunks"])
    for stat in counts.values():
        intervals = sorted(stat.pop("intervals"))
        stat["frames"] = len(intervals)
        if intervals:
            stat["p99_ms"] = round(intervals[math.ceil(len(intervals) * 0.99) - 1], 3)
            stat["max_ms"] = round(intervals[-1], 3)
    return counts


def session(binary, source, out, phase, vsync_off):
    out.mkdir(parents=True)
    root = out / "disk"
    shutil.copytree(source, root)
    install_sim_codecs(binary, root)
    base_count = database_count(root)
    if phase == "database":
        add_scan_work(root)
    else:
        add_art_work(root)

    rb = root / ".rockbox"
    config = rb / "config.cfg"
    settings = {
        "start in screen": "root",
        "root menu order": "pictureflow,files,database,wps,settings,plugins",
        "native theme": "scrim",
        "list wraparound": "off",
        "tagcache_scan_on_startup": "off",
        "debug log tagcache": "on",
        "debug log tasks": "on",
        "debug log artcache": "on",
        "debug log playback": "on",
    }
    old = [line for line in config.read_text().splitlines()
           if line.partition(":")[0] not in settings]
    config.write_text("\n".join(old + [f"{k}: {v}" for k, v in settings.items()]) + "\n")
    for path in rb.glob("*.cfg.*"):
        path.unlink()
    (rb / ".resume.cfg").unlink(missing_ok=True)
    (rb / "ui-perf.enable").touch()

    env = {"PODBOX_MOTION_LOAD_PROBE": "1"}
    if vsync_off:
        env["SDL_RENDER_VSYNC"] = "0"
    sim = Sim(binary, root, out, env)
    log, perf = out / "firmware.log", rb / "ui-perf.txt"
    failure = None
    try:
        wait_log(sim, log, "motion probe: home")
        (rb / "ui-motion-play.request").touch()
        wait_log(sim, log, "Codec: entering run state")
        sim.wait(25)

        def home_glides():
            for key in ("down", "down", "down", "up", "up", "up"):
                sim.tap(key, 5)
            # Include repeated detents at the physical list edge.
            for _ in range(10):
                sim.tap("up", 3)
            sim.wait(26)

        workload(sim, phase)
        home_glides()

        sim.tap("select", 36)
        wait_log(sim, perf, "route=cover-flow-entry ")
        workload(sim, phase)
        for _ in range(3):
            for direction in ("down", "up"):
                for _ in range(8):
                    sim.tap(direction, 5)
        sim.wait(32)
        previous = read(log).count("motion probe: home")
        sim.tap("menu", 40)
        wait_log(sim, log, "motion probe: home", previous)
        home_glides()

        (rb / "ui-motion-finish.request").touch()
        wait_log(sim, log, "motion probe: PCM chunks")
    except AssertionError as error:
        failure = str(error)
    finally:
        sim.close()
        sim.process.wait(timeout=5)

    counts = summarize(perf, phase)
    firmware = read(log)
    pcm = re.search(r"motion probe: PCM chunks=(\d+) underruns=(\d+)", firmware)
    result = {
        "routes": counts,
        "codec_starts": firmware.count("Codec: entering run state"),
        "pcm_chunks": int(pcm[1]) if pcm else None,
        "pcm_underruns": int(pcm[2]) if pcm else None,
        "database_before": base_count,
        "database_after": database_count(root),
        "failure": failure,
    }
    (out / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    print(f"{phase} {out.parent.name} {json.dumps(result)}", flush=True)
    return result


def compare(before, after):
    failures = []
    for role, result in (("baseline", before), ("candidate", after)):
        if result["failure"]:
            failures.append(f"{role}: {result['failure']}")
        if result["database_before"] != result["database_after"]:
            failures.append(f"{role}: database changed")
        if not result["pcm_chunks"]:
            failures.append(f"{role}: missing PCM playback")
        if result["pcm_underruns"] != 0:
            failures.append(f"{role}: PCM underruns={result['pcm_underruns']}")

    if after["codec_starts"] != 1:
        failures.append(f"candidate: codec started {after['codec_starts']} times")
    for route, candidate in after["routes"].items():
        baseline = before["routes"][route]
        if any(stat["loaded_frames"] == 0 or not stat["frames"]
               for stat in (baseline, candidate)):
            failures.append(f"{route}: missing playback frames under load")
            continue
        if candidate["p99_ms"] > baseline["p99_ms"]:
            failures.append(f"{route}: P99 {baseline['p99_ms']} -> {candidate['p99_ms']} ms")
        if baseline["p99_ms"] <= 34 and candidate["p99_ms"] > 34:
            failures.append(f"{route}: P99 {candidate['p99_ms']} exceeds 34 ms")
        if candidate["max_ms"] >= 100:
            failures.append(f"{route}: maximum {candidate['max_ms']} exceeds 100 ms")
    return failures


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("baseline", "candidate", "resource", "output"):
        parser.add_argument(name, type=lambda value: Path(value).resolve())
    parser.add_argument("--fixture", type=lambda value: Path(value).resolve())
    parser.add_argument("--vsync-off", action="store_true")
    args = parser.parse_args()
    source = args.fixture or args.output / "fixture"
    args.output.mkdir(parents=True)
    if not args.fixture:
        fixture(args.baseline, args.resource, source)
    failures = {}
    for phase in ("art", "database"):
        before, after = (session(getattr(args, name), source,
                                 args.output / name / phase, phase, args.vsync_off)
                         for name in ("baseline", "candidate"))
        failures[phase] = compare(before, after)
    (args.output / "acceptance.json").write_text(json.dumps(failures, indent=2) + "\n")
    if any(failures.values()):
        raise SystemExit(f"animation scheduling failed: {failures}")


if __name__ == "__main__":
    main()
