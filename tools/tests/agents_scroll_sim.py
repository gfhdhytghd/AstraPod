#!/usr/bin/env python3
"""Measure Agents wheel response and UI-perf frames with an offline PodLink peer."""
import argparse
import json
import math
import mmap
from pathlib import Path
import time

from t3_remote_buttons_sim import (
    DETAIL, HEADER, PIXELS, RING, STATE, THEMES, WORD, Player, prepare_disk,
)


def dashboard(t3, revision=0):
    prefix = "t3p" if t3 else "w"
    workspaces = ["W\t32"] + (["#\tT3"] if t3 else [])
    status = ["S\t32"]
    for index in range(1, 33):
        name = f"Project {index:02} · 分析合成测试"
        count = 1 + (revision % 3 if index > 18 else 0)
        workspaces.append(f"{prefix}{index}\t-\t{count}\t0\t0\t1\t{name}")
        state = ("w" if index <= 18 or (revision + index) % 3 else "i")
        status.append(f"{prefix}{index}\t{state}\t-")
    return "\n".join(workspaces), "\n".join(status)


def setup(player, t3):
    player.respond = False
    player.expect(lambda: ["tc", "2"] in player.messages, "initial Agents handshake", timeout=10)
    player.send("HR\t1\tScroll fixture")
    player.send("M\t" + ("t3" if t3 else "herdr"))
    for frame in dashboard(t3):
        player.send(frame)
    player.tick = "T\t0\t1\t1" if t3 else "T\t0\t1"
    player.expect(lambda: player.state and player.state.label.startswith("Project "),
                  "32 project dashboard")


def snapshot(state):
    _, sequence, _ = HEADER.unpack_from(state)
    if sequence:
        start = HEADER.size + ((sequence - 1) % RING) * (WORD.size + PIXELS)
        pixels = bytes(state[start + WORD.size:start + WORD.size + PIXELS])
        if WORD.unpack_from(state, start)[0] == sequence:
            return sequence, pixels
    return 0, b""


def percentile(values, percent):
    return round(sorted(values)[math.ceil(len(values) * percent / 100) - 1], 3) if values else None


def measure(player, frames, name, t3, span):
    player.wait(.65)
    mark = len(player.messages)
    player.send("!p" + name)
    player.expect_wire(mark, ":perf", "started")
    observed = []
    for index in range(48):
        if index % 2 == 0:
            for update in dashboard(t3, index // 2 + 1):
                player.send(update)
        direction = "down" if index // span % 2 == 0 else "up"
        initial = (player.state.selected, player.state.scroll)
        sequence, _ = snapshot(frames)
        start = time.monotonic()
        player.keys(("down", direction), ("up", direction))
        hit = None
        while time.monotonic() - start < .027:
            player.receive(.001)
            if (hit is None and (player.state.selected, player.state.scroll) != initial
                    and HEADER.unpack_from(frames)[1] > sequence):
                hit = (time.monotonic() - start) * 1000
        if hit is not None:
            observed.append(hit)
    player.send("!q")
    player.wait(.2)
    log = player.disk / ".rockbox/ui-perf.txt"
    summary = next(line for line in log.read_text().splitlines()
                   if line.startswith("route=" + name + " "))
    fields = dict(item.split("=") for item in summary.split() if "=" in item)
    period_ms = int(fields["period_us"]) / 1000
    return {
        "scene": name,
        "wheel_p50_ms": percentile(observed, 50),
        "wheel_p99_ms": percentile(observed, 99),
        "wheel_moves": len(observed),
        "wheel_inputs": 48,
        "frames": int(fields["seen"]),
        "frame_work_p99_ms": round(period_ms * float(fields["work_p99_f"]), 3),
        "frame_interval_p99_ms": round(period_ms * float(fields["interval_p99_f"]), 3),
    }


def suite(binary, output, resources, theme, t3):
    directory = output / (theme if t3 else theme + "-herdr")
    disk = prepare_disk(directory, binary.parent, resources, theme)
    config = disk / ".rockbox/config.cfg"
    config.write_text(config.read_text().replace("animations: off", "animations: on"))
    (config.parent / "ui-perf.enable").touch()
    path = directory / "frames.bin"
    with path.open("w+b") as file:
        file.truncate(STATE)
        with mmap.mmap(file.fileno(), STATE, access=mmap.ACCESS_READ) as frames, \
                Player(binary, directory, disk, path) as player:
            setup(player, t3)
            result = [measure(player, frames, "projects" if t3 else "herdr", t3, 6)]
            if not t3:
                return result

            if not player.state.label.startswith("Project "):
                player.tap("up")
            selected = player.state.label
            assert selected.startswith("Project "), player.state
            project = "t3p" + str(int(selected.split()[1]))
            player.tap("select")
            player.expect_view("threads")
            player.send(f"T\t{project}\t2\t0\tactive\tActive\tw\n"
                        f"T\t{project}\t2\t1\tall\tAll\ti")
            rows = [f"t3t{i}\tcodex\ti\t0\tThread {i:02} · 接口审查\tgpt · main\t解析状态和长详情"
                    for i in range(1, 17)]
            player.send(f"A\t{project}\t16\n" + "\n".join(rows))
            player.expect(lambda: player.state.rows >= 16, "16 visible threads")
            player.wait(.35)
            result.append(measure(player, frames, "threads", True, 4))
            if not player.state.label.startswith("Thread"):
                player.tap("down")
            thread = "t3t" + str(int(player.state.label.split()[1]))
            player.tap("select")
            player.expect_view("detail")
            player.send(DETAIL.replace("D\tt3t1\t", f"D\t{thread}\t", 1))
            player.expect(lambda: player.state.scroll_max > 2, "long T3 detail")
            player.wait(.35)
            result.append(measure(player, frames, "detail", True, 3))
            player.tap("menu")
            player.expect_view("threads")
            player.tap("menu")
            player.expect_view("projects")
            if theme.startswith("fruit-company"):
                for _ in range(40):
                    if player.state.label == "Usage":
                        break
                    player.keys(("down", "down"), ("up", "down"))
                    player.wait(.055)
                player.choose("Usage")
            else:
                player.tap("back")
            player.expect_view("usage")
            player.wait(.35)
            result.append(measure(player, frames, "usage", True, 2))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--resources", type=Path, required=True)
    parser.add_argument("--theme", action="append", choices=THEMES)
    args = parser.parse_args()
    for theme in args.theme or THEMES:
        for t3 in (True, False):
            for result in suite(args.binary.resolve(), args.output.resolve(),
                                args.resources.resolve(), theme, t3):
                print(json.dumps({"theme": theme, **result}), flush=True)


if __name__ == "__main__":
    main()
