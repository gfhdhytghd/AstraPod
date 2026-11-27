#!/usr/bin/env python3
"""Compare cold and repeated page, sheet and Cover Flow motion, headlessly.

The simulator uses an isolated library and delays the first read of each
storage file, including fonts, art and database files. The same process
runs both attempts. Use --baseline for builds without readiness telemetry.
"""
import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path

sys.dont_write_bytecode = True
from coverflow_np_sheet_sim import playback_page, sheet_motion, turning_frames
from ui_stock_transitions_sim import CLASSIC, Sim, difference, make_fixture


def prepare_disk(fixture, out):
    root = out / "disk"
    shutil.rmtree(root, ignore_errors=True)
    shutil.copytree(fixture, root)
    for stale in ("config.cfg.new", "config.cfg.old"):
        (root / ".rockbox" / stale).unlink(missing_ok=True)
    config = root / ".rockbox/config.cfg"
    lines = [line for line in config.read_text().splitlines()
             if not line.startswith(("native theme:", "root menu order:"))]
    config.write_text("\n".join(lines) + "\nnative theme: classic\n"
                      "idle poweroff: off\nscrolling enabled: off\n"
                      "album covers sort albums by: year descending\n"
                      + CLASSIC)
    (root / ".rockbox/ui-perf.enable").touch()
    return root


def classify(route, shots):
    if route == "cover-flow-wps-rise":
        window, page = playback_page(shots)
        motion = sheet_motion(window, page, window[0][1], True)
        return {"interior": motion["frames"], "offsets": motion["offsets"],
                "ordered": motion["ordered"], "fit": motion["worst_error"]}
    if route == "cover-flip-open":
        return {"interior": turning_frames(shots)}
    return {"interior": sum(
        difference(image, shots[0][1]) > 0.5 and
        difference(image, shots[-1][1]) > 0.5
        for _, image in shots[1:-1]
    )}


def record_pair(sim):
    results = {}
    sim.wait(660)
    sim.tap("down", 35)
    for _ in range(2):
        results.setdefault("push", []).append(sim.record(100, "select"))
        sim.tap("menu", 55)
    sim.tap("up", 35)
    for _ in range(2):
        results.setdefault("cover-flow-entry", []).append(sim.record(240, "select"))
        results.setdefault("cover-flip-open", []).append(sim.record(300, "select"))
        sim.tap("menu", 100)
        results.setdefault("cover-flow-wps-rise", []).append(sim.record(180, "play"))
        sim.tap("menu", 110)  # Now Playing → Cover Flow
        sim.tap("menu", 90)   # Cover Flow → Home
    for _ in range(2):
        results.setdefault("usb-up", []).append(sim.record(90, "usb"))
        sim.tap("usb", 65)
    return results


def read_perf(path):
    routes = {}
    path = Path(path)
    if path.exists():
        for line in path.read_text().splitlines():
            if line.startswith("route="):
                row = dict(field.split("=", 1) for field in line.split())
                routes.setdefault(row["route"], []).append(row)
    return routes


def check_recordings(recordings, perf, baseline):
    failed = []
    results = {}
    for route, pair in recordings.items():
        metrics = [classify(route, shots) for shots in pair]
        captures = perf.get(route, [])[:2]
        results[route] = {"motion": metrics, "perf": captures}
        cold, warm = [m["interior"] for m in metrics]
        lower = 2 if route == "cover-flow-wps-rise" else 4
        if min(cold, warm) < lower or abs(cold - warm) > max(2, warm // 5):
            failed.append(f"{route}: cold/warm moving frames {cold}/{warm}")
        if route == "cover-flow-wps-rise":
            if any(not m["ordered"] or m["fit"] > 4 for m in metrics):
                failed.append(f"{route}: sheet geometry {metrics}")
        if not baseline:
            if ([c.get("first_run") for c in captures] != ["1", "0"] or
                    any(int(c.get("ready_wait_us", "-1")) < 0 for c in captures)):
                failed.append(f"{route}: missing readiness telemetry")
            elif abs(int(captures[0]["seen"]) - int(captures[1]["seen"])) > 2:
                failed.append(f"{route}: transition submitted different frame counts")
    return results, failed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("binary", "resource", "output"):
        parser.add_argument(name, type=lambda name: Path(name).resolve())
    parser.add_argument("--fixture", type=lambda name: Path(name).resolve())
    parser.add_argument("--baseline", action="store_true")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    fixture = args.fixture or args.output / "fixture"
    if not args.fixture:
        make_fixture(args.binary, args.resource, fixture)
    shim = args.output / "cold_storage.so"
    subprocess.run(["cc", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC",
                    str(Path(__file__).with_name("ui_cold_storage.c")),
                    "-o", str(shim), "-ldl", "-pthread"], check=True)
    root = prepare_disk(fixture, args.output)
    sim = Sim(args.binary, root, args.output,
              {"LD_PRELOAD": str(shim), "PODBOX_COLD_READ_MS": "400",
               "PODBOX_GLYPH_DELAY_MS": "400"})
    try:
        recordings = record_pair(sim)
    finally:
        sim.close()
    log = root / ".rockbox/ui-perf.txt"
    if log.exists():
        shutil.copyfile(log, args.output / log.name)
    results, failures = check_recordings(recordings, read_perf(log), args.baseline)
    (args.output / "result.json").write_text(json.dumps(results, indent=2) + "\n")
    shutil.rmtree(root)
    if failures:
        sys.exit("\n".join(failures))
    print("PASS first and repeated motion:", list(results))


if __name__ == "__main__":
    main()
