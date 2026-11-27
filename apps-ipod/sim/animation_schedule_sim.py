#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run matched headless playback, Cover Flow and list motion sessions."""

import argparse
import json
from pathlib import Path
import shutil
import statistics
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


def wait_for_art(sim, cache):
    # A fallback thumbnail is the first output of the real build pass.
    # Starting playback here leaves scratch allocated before audio fills RAM.
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        if any(cache.glob("*/fallback.aat")):
            return
        sim.wait(1)
    raise AssertionError("art cache did not start")


def read(path):
    return path.read_text(errors="replace") if path.exists() else ""


def summarize(records):
    by_route = {}
    for record in records:
        route = record["route"]
        if int(record["pcm_chunks"]) and int(record["seen"]) >= 4:
            by_route.setdefault(route, []).append(record)
    return {
        route: {
            "captures": len(rows),
            "frames": sum(int(row["seen"]) for row in rows),
            "median_capture_p99_ms": round(statistics.median(
                float(row["interval_p99_f"]) * int(row["period_us"]) / 1000
                for row in rows), 3),
            "worst_interval_ms": round(max(
                float(row["interval_worst_f"]) * int(row["period_us"]) / 1000
                for row in rows), 3),
            "interval_misses": sum(int(row["interval_misses"]) for row in rows),
            "database_frames": sum(int(row["database_frames"]) for row in rows),
            "art_frames": sum(int(row["art_frames"]) for row in rows),
            "pcm_chunks": sum(int(row["pcm_chunks"]) for row in rows),
            "pcm_underruns": sum(int(row["pcm_underruns"]) for row in rows),
        }
        for route, rows in by_route.items()
    }


def session(binary, source, out, phase):
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
        "start in screen": "files",
        "start directory": "/Play",
        "root menu order": "pictureflow,files,database,wps,settings,plugins",
        "native theme": "scrim",
        "list wraparound": "off",
        "tagcache_scan_on_startup": "on" if phase == "database" else "off",
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

    sim = Sim(binary, root, out)
    try:
        sim.wait(25)
        sim.tap("select", 18)                # enter /Play
        if phase == "art":
            wait_for_art(sim, rb / "library" / "cache" / "art")
        sim.tap("select", 26)                # start the local FLAC
        sim.tap("menu", 40)                  # Now Playing -> Files
        sim.tap("menu", 45)                  # Files -> Home

        def home_glides():
            for key in ("down", "down", "down", "up", "up", "up"):
                sim.tap(key, 5)
            # A concurrent worker can delay the arrival of the preceding
            # press. Use the list boundary to restore the starting row.
            for _ in range(10):
                sim.tap("up", 3)
            sim.wait(26)

        home_glides()
        if phase == "database":
            sim.wait(120)

        for _ in range(2):
            flow = rb / "ui-perf.txt"
            entries = read(flow).count("route=cover-flow-entry ")
            sim.tap("select", 40)
            deadline = time.monotonic() + 6
            while read(flow).count("route=cover-flow-entry ") == entries:
                assert time.monotonic() < deadline, "Cover Flow entry did not settle"
                sim.wait(3)
            for _ in range(18):
                sim.tap("down", 5)
            sim.wait(45)
            sim.tap("menu", 60)              # Cover Flow -> Home
            home_glides()

        sim.wait(780 if phase == "art" else 300)
    finally:
        sim.close()

    records = [
        dict(field.split("=", 1) for field in line.split())
        for line in read(rb / "ui-perf.txt").splitlines()
        if line.startswith("route=")
    ]
    task_log = read(rb / "logs" / "tasks.log")
    task_phase = phase if phase == "database" else "art cache"
    counts = {
        "worker_log": [line for line in task_log.splitlines()
                       if f"] {task_phase}:" in line],
        "routes": summarize(records),
        "playback_frames_during_db": sum(int(row["database_frames"])
                                         for row in records if int(row["pcm_chunks"])),
        "playback_frames_during_art": sum(int(row["art_frames"])
                                          for row in records if int(row["pcm_chunks"])),
        "thumbnail_count": sum(1 for path in
                               (rb / "library" / "cache" / "art").glob("*/*.aat")
                               if path.name != "fallback.aat"),
        "database_before": base_count,
        "database_after": database_count(root),
        "pcm_underruns_total": int(records[-1]["pcm_underruns_total"]) if records else None,
    }
    (out / "results.json").write_text(json.dumps(counts, indent=2) + "\n")
    print(phase, json.dumps(counts), flush=True)
    assert "cover-flow" in counts["routes"], (phase, "no Cover Flow under playback")
    assert "list-glide" in counts["routes"], (phase, "no list glide under playback")
    assert counts["database_after"] == base_count, counts
    assert counts["worker_log"], (phase, "background pass not observed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("baseline", "candidate", "resource", "output"):
        parser.add_argument(name, type=lambda value: Path(value).resolve())
    parser.add_argument("--fixture", type=lambda value: Path(value).resolve())
    args = parser.parse_args()
    source = args.fixture or args.output / "fixture"
    args.output.mkdir(parents=True)
    if not args.fixture:
        fixture(args.baseline, args.resource, source)
    for phase in ("art", "database"):
        for name in ("baseline", "candidate"):
            session(getattr(args, name), source, args.output / name / phase, phase)


if __name__ == "__main__":
    main()
