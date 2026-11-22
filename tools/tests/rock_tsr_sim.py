#!/usr/bin/env python3
"""Exercise TSR replacement, foreground reuse, USB and power in ipod6g SDL."""
from contextlib import contextmanager
import mmap
import os
from pathlib import Path
import re
from shutil import copyfile
import subprocess
import sys
import time

from rock_phase3_files_sim import Simulator, STATE_BYTES

ROOT = Path(__file__).resolve().parents[2]
build, evidence = (Path(arg).resolve() for arg in sys.argv[1:])


@contextmanager
def session(name):
    case = evidence / name
    disk = case / "disk"
    rockbox = disk / ".rockbox"
    apps = rockbox / "rocks/apps"
    apps.mkdir(parents=True, exist_ok=True)
    fonts = rockbox / "fonts"
    fonts.mkdir(exist_ok=True)
    for font in ("18-noto-sans.fnt", "18-noto-sans-bold.fnt"):
        copyfile(ROOT / "apps-ipod/fonts" / font, fonts / font)
    modules = build / "rock-compat/modules/demos"
    copyfile(modules / "tsr_probe.tsr.rock", apps / "01_resident.tsr.rock")
    copyfile(modules / "tsr_foreground_probe.rock", apps / "02_foreground.rock")
    (rockbox / "config.cfg").write_text(
        "start in screen: root\nroot menu order: plugins,files,settings\n"
        "animations: off\nbacklight timeout: off\nvoice menus: off\n"
        "font: /.rockbox/fonts/18-noto-sans.fnt\n"
        "font bold: /.rockbox/fonts/18-noto-sans-bold.fnt\n"
    )
    trace_path = case / "firmware.log"
    command = [
        "bwrap", "--die-with-parent", "--ro-bind", "/", "/", "--dev", "/dev",
        "--proc", "/proc", "--tmpfs", "/tmp", "--unshare-net",
        "--ro-bind", str(build / "rockboxui"), "/tmp/rockboxui",
        "--bind", str(case), "/tmp/case", "--chdir", "/tmp/case/disk",
        "--", "/tmp/rockboxui", "--root", ".", "--nobackground",
        "--screenshot", "--keys", "-",
    ]
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy",
               SDL_RENDER_DRIVER="software", XDG_RUNTIME_DIR="/tmp",
               PODBOX_UI_WALK_STATE="/tmp/case/state.bin")
    with (case / "state.bin").open("w+b") as state, trace_path.open("wb") as log:
        state.truncate(STATE_BYTES)
        with mmap.mmap(state.fileno(), 0, access=mmap.ACCESS_READ) as frames, \
                subprocess.Popen(command, env=env, stdin=subprocess.PIPE,
                                 stdout=log, stderr=subprocess.STDOUT) as proc:
            try:
                sim = Simulator(proc, frames, case)
                sim.capture("home", "plugins", timeout=30)
                sim.tap("select")
                sim.capture("categories", "applications")
                sim.tap("select")
                sim.capture("browser", "resident", "foreground")
                sim.tap("select")
                trace(trace_path, r"TSR entry\n")
                sim.tap("menu")
                yield sim, trace_path
            finally:
                if proc.poll() is None:
                    proc.terminate()
                proc.wait(timeout=8)


def trace(path, pattern, timeout=8):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        log = path.read_text(errors="replace")
        if re.search(pattern, log, re.S):
            return log
        time.sleep(.05)
    raise AssertionError((path, pattern, log[-3000:]))


with session("replacement") as (sim, path):
    sim.tap("down", "select")
    trace(path, r"TSR foreground overwritten\n")
    sim.tap("menu")
    sim.capture("after-overwrite", "resident", "foreground")
    sim.tap("up")
    sim.tap("select")
    trace(path, r"TSR stop 1 ticks [1-9]\d*\n.*TSR image closed\n.*TSR entry")
    sim.tap("select")
    trace(path, r"TSR foreground overwritten\n.*TSR tick \d+\n.*"
                r"TSR foreground exit\n.*TSR stop 1 ticks [1-9]\d*\n"
                r".*TSR image closed\n.*TSR entry\n.*"
                r"TSR stop 4 ticks 0\n.*TSR image closed")

with session("usb") as (sim, path):
    sim.tap("usb")
    sim.capture("charge-consent", "computer connected", "charge only")
    sim.tap("menu", "usb", "usb")
    sim.capture("storage-consent", "computer connected", "charge only")
    assert "TSR stop" not in path.read_text(errors="replace")
    sim.tap("select")
    trace(path, r"USB inserted\. Waiting.*TSR stop 2 ticks [1-9]\d*\n"
                r".*TSR image closed\n.*All threads have acknowledged the connect")
    sim.tap("usb")

with session("power") as (sim, path):
    sim.proc.stdin.write(b"down play\n")
    sim.proc.stdin.close()
    trace(path, r"TSR stop 3 ticks [1-9]\d*\n.*TSR image closed")
    assert sim.proc.wait(timeout=25) == 0

print("PASS SDL TSR foreground overwrite, reentry, abort, USB and power")
