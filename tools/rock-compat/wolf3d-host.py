#!/usr/bin/env python3
"""Run the Wolf3D loader and synthetic SDL probe under GDB on private volumes."""
import argparse
import os
from pathlib import Path
import shutil
import signal
import subprocess
import time


DATA_NAMES = ("vswap", "audiohed", "audiot", "gamemaps", "maphead",
              "vgadict", "vgagraph", "vgahead")
LAUNCH = """wait 180
down select
wait 9
up select
wait 45
down down
wait 9
up down
wait 25
down select
wait 9
up select
wait 65
down select
wait 9
up select
"""
NORMAL = """wait 120
down select
wait 2
up select
wait 30
down hold
wait 9
up hold
"""
USB = """wait 120
down usb
wait 9
up usb
wait 60
down select
wait 9
up select
wait 110
down usb
wait 9
up usb
"""
TRACE = r"""
set pagination off
set confirm off
set breakpoint pending on
handle SIGUSR1 nostop noprint pass
break wolf_error
commands
silent
printf "WOLF error %s\n", message
continue
end
break rock_worker_join
commands
silent
printf "WOLF worker join\n"
continue
end
break rock_stream_close
commands
silent
printf "WOLF stream close\n"
continue
end
break rock_usb_takeover
commands
silent
printf "WOLF USB takeover\n"
continue
end
break rock_display_stop
commands
silent
printf "WOLF display restored\n"
continue
end
run
"""


def prepare(case, module, fonts, populate):
    disk = case / "disk"
    base = disk / ".rockbox"
    data = base / "wolf3d"
    data.mkdir(parents=True)
    (base / "fonts").mkdir()
    for font in ("18-noto-sans.pbf", "18-noto-sans-bold.pbf"):
        shutil.copyfile(fonts / font, base / "fonts" / font)
    games = base / "rocks" / "games"
    games.mkdir(parents=True)
    shutil.copyfile(module, games / "wolf3d.rock")
    (base / "config.cfg").write_text(
        "start in screen: root\nroot menu order: plugins,files,settings\n"
        "animations: off\nbacklight timeout: off\nvoice menus: off\n"
        "font: /.rockbox/fonts/18-noto-sans.pbf\n"
        "font bold: /.rockbox/fonts/18-noto-sans-bold.pbf\n"
        "show icons: off\n"
    )
    if populate:
        for name in DATA_NAMES:
            (data / f"{name}.wl1").write_bytes(b"\x00")
    return disk


def exercise(build, fonts, case):
    name = case.name
    module = build / "rock-compat" / (
        "wolf3d-probe.rock" if name in ("normal", "usb") else "modules/games/wolf3d.rock")
    disk = prepare(case, module, fonts, name != "missing")
    keys = case / "keys"
    keys.write_text(LAUNCH + {"normal": NORMAL, "usb": USB}.get(name, ""))
    gdb = case / "gdb"
    gdb.write_text(TRACE)
    log_path = case / "trace.log"
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy",
               SDL_RENDER_DRIVER="software")
    for key in ("LD_PRELOAD", "PODBOX_UI_KEYS", "PODBOX_UI_WALK_STATE"):
        env.pop(key, None)
    command = [
        "gdb", "-batch", "-x", str(gdb), "--args", str(build / "rockboxui"),
        "--root", ".", "--nobackground", "--screenshot", "--keys", str(keys),
    ]
    with log_path.open("wb") as log, subprocess.Popen(
            command, cwd=disk, env=env, stdout=log, stderr=subprocess.STDOUT,
            stdin=subprocess.DEVNULL, start_new_session=True) as sim:
        try:
            deadline = time.monotonic() + 25
            while time.monotonic() < deadline and sim.poll() is None:
                trace = log_path.read_text(errors="replace")
                if "WOLF display restored" in trace:
                    break
                time.sleep(.1)
        finally:
            if sim.poll() is None:
                os.killpg(sim.pid, signal.SIGTERM)
                try:
                    sim.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    os.killpg(sim.pid, signal.SIGKILL)
                    sim.wait()

    trace = log_path.read_text(errors="replace")
    assert "WOLF display restored" in trace, log_path
    assert "Program received signal" not in trace, log_path
    base = disk / ".rockbox"
    if name == "missing":
        assert "WOLF error Missing Wolf3D data: vswap.wl1" in trace, log_path
    elif name == "truncated":
        assert "WOLF error Invalid VSWAP header" in trace, log_path
    else:
        assert "WOLF error" not in trace, log_path
        assert (base / "wolf3d-probe-ready").is_file(), log_path
        join = trace.index("WOLF worker join")
        close = trace.index("WOLF stream close", join)
        restored = trace.index("WOLF display restored", close)
        if name == "normal":
            for marker in ("tap", "release", "return"):
                assert (base / f"wolf3d-probe-{marker}").is_file(), log_path
        else:
            assert close < trace.index("WOLF USB takeover") < restored, log_path
            assert not (base / "wolf3d-probe-return").exists(), log_path
    assert not list((base / "wolf3d").glob("*.cfg")), log_path
    print(f"PASS {name}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path)
    parser.add_argument("fonts", type=Path)
    parser.add_argument("out", type=Path)
    cases = ("missing", "truncated", "normal", "usb")
    parser.add_argument("--case", choices=cases)
    args = parser.parse_args()
    for name in (args.case,) if args.case else cases:
        case = args.out.resolve() / name
        case.mkdir(parents=True)
        exercise(args.build.resolve(), args.fonts.resolve(), case)


if __name__ == "__main__":
    main()
