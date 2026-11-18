#!/usr/bin/env python3
"""Run seeded tests of a built iPod 6G SDL simulator on private music libraries.

Examples:
  tools/tests/ui_random_walk.py --quick --simulator build/sim/rockboxui
  tools/tests/ui_random_walk.py --simulator build/sim/rockboxui --seeds 200 --keys 5000
  tools/tests/ui_random_walk.py --mock-remote --quick
  tools/tests/ui_random_walk.py --simulator build/sim/rockboxui --replay runs/seed-001/replay.json

The simulator must be an SDL build that accepts `--keys -`. ffmpeg builds the
fixture. No real input, audio, USB or injected library is opened.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
from contextlib import ExitStack
import hashlib
import json
import mmap
import os
from pathlib import Path
import random
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time

from ui_music_fixture import (
    CONFIG, TRACK_COUNT, build_tagcache, create_fixture, database_count, install_sim_codecs,
)
from ui_random_walk_remote import build_mock, offline_peer

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
DEFAULT_SIMULATOR = REPO / ".build/native-ui/fu90-walk/sim/rockboxui"
RELEASE_SIMULATOR = REPO / ".build/native-ui/fu90/sim/rockboxui"
DEFAULT_RESOURCES = (REPO.parent /
                     "ipod/downloads/podbox-native-ui-astra-review-b7b0e016e8.zip")
DEFAULT_OUTPUT = REPO / ".build/native-ui/fu90-walk"
REGRESSIONS = HERE / "fixtures/ui_random_walk"
FPS = 60
WATCHDOG_FRAMES = 2 * FPS
BOOT_FRAMES = 115
FRAME_WIDTH, FRAME_HEIGHT, FRAME_COUNT = 320, 240, 8
PIXELS = FRAME_WIDTH * FRAME_HEIGHT * 3
WORD = struct.Struct("=I")
HEADER = struct.Struct("=III")
STATE_SIZE = HEADER.size + FRAME_COUNT * (WORD.size + PIXELS)
PANIC = re.compile(rb"\*PANIC\*|Assertion .* failed|AddressSanitizer|"
                   rb"Segmentation fault|runtime error:|"
                   rb"queue_(?:post|send) ovf", re.IGNORECASE)
KEYS = ("up", "down", "select", "menu", "back", "play", "right", "hold")
HOME_ROWS = (
    "pictureflow", "artistportraits", "database", "cider", "files", "wps", "reading",
    "random", "search", "docs", "images", "books", "spun", "podcasts", "playlists",
    "currentplaylist", "quiz", "shortcuts", "settings", "system_menu",
    *(f"tagnavi{i}" for i in range(20)),
)
TAPS = ("up", "down", "select", "menu", "back", "play", "right")
TAP_WEIGHTS = (22, 26, 24, 10, 8, 5, 5)


def action_stream(seed, count):
    rng = random.Random(seed)
    actions = []
    while len(actions) < count:
        kind = rng.randrange(100)
        if kind < 8:
            wheel = rng.choice(("up", "down"))
            actions.extend((wheel, 0, 1) for _ in range(rng.randint(2, 9)))
        elif kind < 11:
            # Exercise the switch while keys are moving, then always release it.
            actions.append(("hold", 1, 2))
            actions.extend((rng.choice(TAPS), 1, 1) for _ in range(rng.randint(1, 4)))
            actions.append(("hold", 1, 2))
        elif kind < 15:
            key = rng.choice(("select", "menu", "back", "right"))
            actions.append((key, rng.randint(21, 42), 2))
        else:
            key = rng.choices(TAPS, TAP_WEIGHTS)[0]
            actions.append((key, 0 if key in {"up", "down"} else 1, 1))
    actions = actions[:count]
    if sum(action[0] == "hold" for action in actions) % 2:
        # A sequence can end inside its last hold group.
        actions[-1] = ("hold", 1, 2) if actions[-1][0] != "hold" else ("menu", 1, 2)
    return actions


def json_write(path, obj):
    path.write_text(json.dumps(obj, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def crc_mpeg2(data):
    crc = 0xffffffff
    for byte in data:
        crc ^= byte << 24
        for _ in range(8):
            polynomial = 0x04c11db7 if crc & (1 << 31) else 0
            crc = ((crc << 1) ^ polynomial) & 0xffffffff
    return crc


def enable_resume_rows(root):
    rockbox = root / ".rockbox"
    control = rockbox / ".playlist_control"
    if not control.exists():
        control.write_text("P:6:/Playlists:Fixture Mix.m3u8\n", encoding="utf-8")
    document = next((root / "Documents").glob("*.txt"))
    relpath = "/" + document.relative_to(root).as_posix()
    # libfile_header and tv_place from the firmware reading data format.
    record = struct.pack("<4i260s", -1, 0, document.stat().st_size, 0, relpath.encode())
    header = struct.pack("<IHHI3iI", 0x44414552, 3, len(record), 1, -1, -1, -1,
                         crc_mpeg2(record))
    reading = rockbox / "library/user/reading.dat"
    reading.parent.mkdir(parents=True, exist_ok=True)
    reading.write_bytes(header + record)
    shortcuts = rockbox / "shortcuts.txt"
    if not shortcuts.exists():
        shortcuts.write_text("[shortcut]\ntype: browse\ndata: /Music\nname: Fixture Music\n",
                             encoding="utf-8")


def prepare_fixture(args):
    if args.fixture is None:
        base = args.output / "fixture"
        create_fixture(base, args.resources)
        build_tagcache(args.simulator, base, args.output / "tagcache.log")
        enable_resume_rows(base)
    else:
        base = args.fixture
    tracks = database_count(base)
    if tracks < TRACK_COUNT:
        raise ValueError(f"Fixture database lacks {TRACK_COUNT} committed tracks: {base}")
    return base, tracks


def write_config(root, seed, rows):
    # Change which Home row the cursor starts on while retaining all entries.
    shift = seed % len(rows)
    rotated = (*rows[shift:], *rows[:shift])
    config = "\n".join(
        line for line in CONFIG.splitlines() if not line.startswith("root menu order:")
    )
    config += "\nroot menu order: " + ",".join(rotated) + "\n"
    config += ("settings mode: everything\nlist wraparound: on\n"
               "idle poweroff: off\nsleep timer on startup: off\n")
    (root / ".rockbox/config.cfg").write_text(config, encoding="utf-8")
    return rotated


class Simulator:
    def __init__(self, args, root, out, render_delay, backend):
        self.logfile = out / "firmware.log"
        self.statefile = out / "state.bin"
        self.history = []
        self.cpu_limit = args.cpu_seconds
        self.memory_limit = args.memory_mib * 1024 * 1024
        self.cpus = ",".join(str(cpu) for cpu in args.cpus)
        self.binary = args.simulator
        self.root = root
        self.render_delay = render_delay
        self.backend = backend
        self.peer_log = out / "mock-remote.log"
        self.timeout = WATCHDOG_FRAMES / FPS
        self.process = None

    def __enter__(self):
        self.log = self.logfile.open("ab", buffering=0)
        self.reader = self.logfile.open("rb", buffering=0)
        self.reader.seek(0, os.SEEK_END)
        self.partial = b""
        self.sent = 0
        self.state_handle = self.statefile.open("w+b")
        self.state_handle.truncate(STATE_SIZE)
        self.state = mmap.mmap(self.state_handle.fileno(), STATE_SIZE, access=mmap.ACCESS_READ)
        command = [
            "taskset", "--cpu-list", self.cpus,
            "prlimit", f"--as={self.memory_limit}", f"--cpu={self.cpu_limit}",
            "--core=0", "--",
            str(self.binary), "--root", ".", "--nobackground",
            "--zoom", "1", "--screenshot", "--keys", "-",
        ]
        env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy",
                   SDL_RENDER_DRIVER="software",
                   PODBOX_UI_WALK_STATE=str(self.statefile))
        env.pop("LD_PRELOAD", None)
        if self.render_delay:
            env["PODBOX_UI_WALK_RENDER_DELAY"] = ":".join(map(str, self.render_delay))
        self.contexts = ExitStack()
        self.peer_ready = None
        if self.backend:
            extra_env, self.peer_ready = self.contexts.enter_context(
                offline_peer(self.backend, self.peer_log))
            env.update(extra_env)
        self.process = subprocess.Popen(command, cwd=self.root, env=env, stdin=subprocess.PIPE,
                                        stdout=self.log, stderr=subprocess.STDOUT, bufsize=0)
        self.present = 0
        self.last_frame = time.monotonic()
        return self

    def __exit__(self, *_):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(2)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        self.process.stdin.close()
        self.state.close()
        self.state_handle.close()
        self.reader.close()
        self.log.close()
        self.contexts.close()

    def observe(self):
        fragment = self.reader.read()
        if fragment:
            self.partial = (self.partial + fragment)[-65536:]
            hit = PANIC.search(self.partial)
            if hit:
                line = self.partial[hit.start():].split(b"\n", 1)[0]
                raise RuntimeError(line.decode("utf-8", errors="replace"))
            self.partial = self.partial.rsplit(b"\n", 1)[-1]
        code = self.process.poll()
        if code == 0:
            raise CleanExit()
        if code is not None:
            raise RuntimeError(f"simulator exited with status {code}")
        presents, _, _ = HEADER.unpack_from(self.state)
        now = time.monotonic()
        if presents != self.present:
            self.present, self.last_frame = presents, now
        if now - self.last_frame > self.timeout:
            raise RuntimeError(f"no new SDL frame for >{WATCHDOG_FRAMES} f")

    def wait(self, frames):
        until = time.monotonic() + frames / FPS
        while time.monotonic() < until:
            self.observe()
            time.sleep(min(max(0, until - time.monotonic()), 6 / FPS))
        self.observe()

    def send(self, key, pressed):
        if key not in KEYS:
            raise ValueError(f"unknown key {key}")
        try:
            self.process.stdin.write(f"{'down' if pressed else 'up'} {key}\n".encode())
            self.process.stdin.flush()
            self.sent += 1
        except BrokenPipeError:
            self.observe()
            raise

    def act(self, action):
        key, hold_frames, gap_frames = action
        if key == "wait":
            self.wait(gap_frames)
            return
        self.send(key, 1)
        if hold_frames:
            self.wait(hold_frames)
        self.send(key, 0)
        self.wait(gap_frames)

    def evidence(self, out):
        _, frames, accepted = HEADER.unpack_from(self.state)
        saved = []
        for seq in range(max(1, frames - FRAME_COUNT + 1), frames + 1):
            frame = HEADER.size + ((seq - 1) % FRAME_COUNT) * (WORD.size + PIXELS)
            if WORD.unpack_from(self.state, frame)[0] != seq:
                continue
            pixels = self.state[frame + WORD.size:frame + WORD.size + PIXELS]
            if WORD.unpack_from(self.state, frame)[0] != seq:
                continue
            filename = f"frame-{seq:06d}.ppm"
            (out / filename).write_bytes(f"P6\n{FRAME_WIDTH} {FRAME_HEIGHT}\n255\n".encode() + pixels)
            saved.append(filename)
        return saved, accepted

    def stats(self):
        return HEADER.unpack_from(self.state)


class CleanExit(Exception):
    """Expected through the shutdown or reboot UI."""


def run_seed(args, base, seed, actions, name, home_rows, render_delay):
    out = args.output / name
    out.mkdir()
    root = out / "disk"
    shutil.copytree(base, root)
    install_sim_codecs(args.simulator, root)
    enable_resume_rows(root)
    rows = write_config(root, seed, home_rows)
    result = {"seed": seed, "keys": 0, "boots": 0, "sdl_events": 0, "presents": 0,
              "home_order": rows, "status": "pass"}
    backend = ("Cider", "Rufin", "Jellyfin")[seed % 3] if args.mock_remote else None
    if backend:
        result["mock_backend"] = backend
    started = time.monotonic()
    with (out / "keys.jsonl").open("w", encoding="utf-8") as log:
        while result["keys"] < len(actions):
            result["boots"] += 1
            sim = Simulator(args, root, out, render_delay, backend)
            with sim:
                exit_expected = False
                try:
                    sim.wait(BOOT_FRAMES)
                    if sim.peer_ready and not sim.peer_ready.is_set():
                        raise RuntimeError("mock PodLink peer did not connect")
                    for action in actions[result["keys"]:]:
                        result["keys"] += 1
                        log.write(json.dumps({
                            "index": result["keys"], "action": action,
                            "boot": result["boots"],
                            "elapsed_f": round((time.monotonic() - started) * FPS),
                        }) + "\n")
                        sim.act(action)
                    sim.wait(12)
                except CleanExit:
                    exit_expected = True
                    if not result["keys"] or result.get("last_exit") == result["keys"]:
                        result.update(status="fail", error="simulator exited before accepting input")
                    result["last_exit"] = result["keys"]
                except RuntimeError as error:
                    result.update(status="fail", error=str(error))
                finally:
                    presents, _, accepted = sim.stats()
                    result["presents"] += presents
                    result["sdl_events"] += accepted
                    if not exit_expected and result["status"] == "pass" and accepted != sim.sent:
                        result.update(status="fail", error="key script did not accept all input")
                    if result["status"] == "fail":
                        result["frames"], _ = sim.evidence(out)
                        json_write(out / "replay.json", {
                            "seed": seed, "keys": result["keys"], "home_rows": home_rows,
                            "render_delay": render_delay, "actions": actions[:result["keys"]],
                        })
            if result["status"] == "fail":
                break
    elapsed = time.monotonic() - started
    result["elapsed_f"] = round(elapsed * FPS)
    result.pop("last_exit", None)
    json_write(out / "result.json", result)
    (out / "state.bin").unlink()
    shutil.rmtree(root)
    return result


def positive(value):
    number = int(value)
    if number < 1:
        raise argparse.ArgumentTypeError("must be greater than zero")
    return number


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--simulator", type=Path, help="prebuilt simulator binary")
    parser.add_argument("--mock-remote", action="store_true",
                        help="build/use the simulator with the offline PodLink fixture")
    parser.add_argument("--resources", type=Path, default=DEFAULT_RESOURCES)
    parser.add_argument("--fixture", type=Path, help="reuse an already-built fixture")
    parser.add_argument("--output", type=Path, help="new evidence directory")
    parser.add_argument("--quick", action="store_true", help="6 seeds with 240 keys apiece")
    parser.add_argument("--seeds", type=positive, help="number of adjacent seeds")
    parser.add_argument("--start-seed", type=int, default=0)
    parser.add_argument("--keys", type=positive, help="key actions per seed")
    parser.add_argument("--jobs", type=positive, default=8)
    parser.add_argument("--cpu-seconds", type=positive, default=150)
    parser.add_argument("--memory-mib", type=positive, default=512)
    parser.add_argument("--replay", type=Path, help="replay a captured seed or explicit actions")
    args = parser.parse_args()
    for attr in ("simulator", "resources", "fixture", "output"):
        path = getattr(args, attr)
        if path is not None:
            setattr(args, attr, path.resolve())
    if args.simulator is None and not args.mock_remote:
        args.simulator = RELEASE_SIMULATOR if RELEASE_SIMULATOR.is_file() else DEFAULT_SIMULATOR
    if args.simulator is not None and not args.simulator.is_file():
        parser.error(f"build the iPod 6G SDL simulator first: {args.simulator}")
    if args.fixture is None and not args.resources.is_file():
        parser.error(f"resource ZIP does not exist: {args.resources}")
    args.seeds = args.seeds or (6 if args.quick else 200)
    args.keys = args.keys or (240 if args.quick else 5000)
    args.cpus = sorted(os.sched_getaffinity(0))[:args.jobs]
    args.jobs = len(args.cpus)
    return args


def replay_spec(path, name):
    spec = json.loads(path.read_text(encoding="utf-8"))
    seed = spec["seed"]
    actions = spec.get("actions") or action_stream(seed, spec["keys"])
    return seed, actions, name, spec.get("home_rows", HOME_ROWS), spec.get("render_delay")


def main():
    args = parse_args()
    if args.output is None:
        DEFAULT_OUTPUT.mkdir(parents=True, exist_ok=True)
        args.output = Path(tempfile.mkdtemp(prefix="run-", dir=DEFAULT_OUTPUT))
    args.output.mkdir(parents=True, exist_ok=True)
    if args.simulator is None:
        args.simulator = build_mock(REPO, args.output, args.jobs)
    base, tracks = prepare_fixture(args)
    if args.replay:
        runs = [replay_spec(args.replay, "replay")]
    else:
        runs = [(seed, action_stream(seed, args.keys), f"seed-{seed:03}", HOME_ROWS, None)
                for seed in range(args.start_seed, args.start_seed + args.seeds)]
        for path in sorted(REGRESSIONS.glob("regression-*.json")):
            runs.append(replay_spec(path, path.stem))
        if not args.quick:
            for path in sorted((REGRESSIONS / "wide").glob("regression-*.json")):
                runs.append(replay_spec(path, path.stem))
    totals = {"results": [], "simulator_sha256": hashlib.file_digest(
        args.simulator.open("rb"), "sha256").hexdigest(), "fixture_tracks": tracks,
        "cpu_per_process_s": args.cpu_seconds, "affinity_cpus": args.cpus,
        "memory_per_process_mib": args.memory_mib, "jobs": args.jobs}
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        pending = {pool.submit(run_seed, args, base, *run): run[2] for run in runs}
        for done in as_completed(pending):
            name = pending[done]
            try:
                result = done.result()
            except Exception as error:  # Report infrastructure failures per seed.
                result = {"status": "error", "error": str(error), "keys": 0}
            result["name"] = name
            totals["results"].append(result)
            print(f"{name}: {result['status']} {result['keys']} keys"
                  + (f" ({result['error']})" if "error" in result else ""), flush=True)
            json_write(args.output / "summary.json", totals)
    return any(result["status"] != "pass" for result in totals["results"])


if __name__ == "__main__":
    sys.exit(main())
