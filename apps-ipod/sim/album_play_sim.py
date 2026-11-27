#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise cold album playback and persistent remote panels using SDL dummy."""

import argparse
import csv
import os
import shutil
import sys
import time
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/tests"))
from ui_stock_transitions_sim import Sim, make_fixture


def reset_config(root, theme):
    rockbox = root / ".rockbox"
    for name in ("config.cfg.new", "config.cfg.old", ".resume.cfg", ".resume.cfg.new"):
        (rockbox / name).unlink(missing_ok=True)
    config = rockbox / "config.cfg"
    keys = ("native theme:", "root menu order:", "album covers sort albums by:")
    lines = [line for line in config.read_text().splitlines()
             if not line.startswith(keys)]
    lines += ["root menu order: pictureflow,wps,database,files",
              f"native theme: {theme}",
              "album covers sort albums by: year descending"]
    config.write_text("\n".join(lines) + "\n")
    (rockbox / "ui-perf.enable").touch()
    (rockbox / "podbox").mkdir(exist_ok=True)
    (rockbox / "podbox/perf.enable").touch()
    return rockbox


def read(path):
    return path.read_text(errors="replace") if path.exists() else ""


def wait_for(sim, predicate, label, seconds=6):
    limit = time.monotonic() + seconds
    while time.monotonic() < limit:
        if predicate():
            return
        sim.wait(3)
    raise AssertionError(label)


def open_panel(sim, perf, wait_for_tracks):
    sim.wait(150)
    sim.tap("select", 150)  # Home → Cover Flow
    for _ in range(3):
        sim.tap("select", 24)
        if "panel select " in read(perf):
            break
    wait_for(sim, lambda: "panel select " in read(perf), "album did not open")
    if wait_for_tracks:
        wait_for(sim, lambda: "panel open " in read(perf), "missing painted rows")
        sim.wait(12)


def route_rows(log, name):
    lines = log.splitlines()
    for i, line in enumerate(lines):
        if line.startswith(f"route={name} "):
            fields = dict(field.split("=", 1) for field in line.split())
            return fields, list(csv.DictReader(lines[i+1:i+2+int(fields["rows"])]))
    raise AssertionError(f"missing route {name}")


def cold_play(binary, fixture, out, theme, baseline):
    out.mkdir()
    root = out / "disk"
    shutil.copytree(fixture, root)
    rockbox = reset_config(root, theme)
    sim = Sim(binary, root, out)
    try:
        open_panel(sim, rockbox / "podbox/perf.log", True)
        sim.tap("select", 1)
        log = rockbox / "ui-perf.txt"
        wait_for(sim, lambda: "cover-wps route=C " in read(log), "missing panel → WPS")
        fields, rows = route_rows(read(log), "cover-panel-wps")
        setup = next(line for line in read(log).splitlines()
                     if line.startswith("cover-wps route=C "))
        excess = max(float(row["interval_f"]) - int(row["divisor"])
                     for row in rows[1:])
        print(theme, fields["seen"], "frames", "max interval",
              fields["interval_worst_f"], "max excess", round(excess, 4), setup,
              flush=True)
        if not baseline:
            assert int(fields["seen"]) > 10, fields
            assert excess < 0.75, fields  # Ignore SDL jitter below one refresh.
            if theme == "scrim":
                assert int(fields["seen"]) >= 22, fields  # Includes controls slide.
    finally:
        sim.close()


def remote(binary, fixture, out):
    out.mkdir()
    root = out / "disk"
    shutil.copytree(fixture, root)
    for phase, delay in (("cold", "5000"), ("reboot", "60000")):
        session = out / phase
        session.mkdir()
        rockbox = reset_config(root, "scrim")
        perf = rockbox / "podbox/perf.log"
        perf.unlink(missing_ok=True)
        os.environ["PODBOX_SIM_CIDER"] = delay
        sim = Sim(binary, root, session)
        try:
            open_panel(sim, perf, phase == "reboot")
            if phase == "cold":
                line = next(line for line in read(perf).splitlines()
                            if line.startswith("panel draw "))
                first = int(line.split()[2])
                selected = next(int(line.split()[2]) for line in read(perf).splitlines()
                                if line.startswith("panel select "))
                assert int(line.split()[4]) == 0 and (first - selected) % 2**32 < 300000
                wait_for(sim, lambda: "panel open " in read(perf), "host rows missing")
                pack = rockbox / "cider-art"
                # Two 32-row shards, each with a sector-aligned 21 KiB payload.
                wait_for(sim, lambda: sum(p.stat().st_size for p in pack.glob("p*.pak"))
                         >= 2 * (21 * 1024 + 512), "tail was not prefetched", seconds=12)
                print("remote cold: empty panel shown in", (first - selected) % 2**32,
                      "µs, both track pages persisted", flush=True)
            else:
                log = read(perf)
                assert "panel disk " in log and "panel remote " not in log, log
                opened = next(line for line in log.splitlines()
                              if line.startswith("panel open ")).split()
                assert int(opened[3]) < 1200000 and int(opened[4]) == 7, opened
                hit = next(line for line in log.splitlines() if line.startswith("panel ram ")).split()
                assert int(hit[3]) < 50000, hit
                sim.tap("select")
                playback = read(session / "firmware.log")
                assert "sim Cider: play play/sim/0 album/sim 0" in playback, playback
                assert "sim Cider: track list requested" not in playback, playback
                print("remote reboot: seven visible tracks in", opened[3],
                      "µs, cached play route sent without a host track fetch", flush=True)
        finally:
            sim.close()
            os.environ.pop("PODBOX_SIM_CIDER", None)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("binary", "resource", "output"):
        parser.add_argument(name, type=lambda value: Path(value).resolve())
    parser.add_argument("--fixture", type=lambda value: Path(value).resolve())
    parser.add_argument("--baseline", action="store_true")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    fixture = args.fixture or args.output / "fixture"
    if not args.fixture:
        make_fixture(args.binary, args.resource, fixture)
    for theme in ("fruit-company", "scrim"):
        cold_play(args.binary, fixture, args.output / theme, theme, args.baseline)
    if not args.baseline:
        remote(args.binary, fixture, args.output / "remote")


if __name__ == "__main__":
    main()
