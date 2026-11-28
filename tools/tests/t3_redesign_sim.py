#!/usr/bin/env python3
"""Capture the T3 Agents pages in every theme with a headless simulator.

Builds the instrumented ipod6g simulator of t3_remote_buttons_sim.py (or
reuses one), drives it with a synthetic HerdRemote peer and saves PNG frames
of the thread list, thread detail (short, long, with a pending approval) and
Usage. With --check it also asserts the focus rule: moving the wheel over the
thread list sends no fp, and SELECT on a thread sends exactly one.
SDL uses the dummy video and audio drivers throughout.
"""
import argparse
import mmap
from pathlib import Path
import select
import shutil
import time

from PIL import Image

from t3_remote_buttons_sim import (
    HEADER, PIXELS, RING, STATE, THEMES, WORD, Player, State, build_simulator,
    prepare_disk,
)

PROJECTS = ("W\t2\n#\tT3\nt3p1\t-\t2\t1\t0\t0\tpodbox\n"
            "t3p2\t-\t1\t0\t1\t1\tAstraPod-Desktop")
STATUS = "S\t2\nt3p1\tb\t-\nt3p2\tw\t-"
THREADS = {
    "t3t1": ("b", "1", "Redesign the T3 thread list", "t3p1",
             "Waiting for approval to run the simulator"),
    "t3t2": ("w", "0", "Fix the Usage page font", "t3p1",
             "The panel draws in the list viewport's font"),
    "t3t3": ("w", "0", "Flat thread list in the daemon", "t3p2",
             "Adding the tl capability and its tests"),
    "t3t4": ("d", "0", "Release notes for the rolling build", "t3p2",
             "Wrote the upgrade notes"),
    "t3t5": ("i", "0", "Cover Flow stock motion fit", "t3p1",
             "Fitted the curve to the stock frames"),
}
LABELS = {"t3p1": "podbox", "t3p2": "AstraPod-Desktop"}
ACTIVE = ("t3t1", "t3t2", "t3t3", "t3t5")
ALL = ("t3t1", "t3t2", "t3t3", "t3t5", "t3t4")
SHORT = "Done. The Usage page now draws its figures in the theme's own fonts."
LONG = ("I traced the tiny text to the panel viewport: it copies the list's "
        "viewport, whose font is the system font, so every figure came out "
        "six pixels tall. The fix loads the theme's body and title fonts once "
        "when Agents opens and draws the panel with them. 中文也要清楚：标题、"
        "状态和消息分三层，行距宽松，长消息可以用滚轮翻页。Next I will capture "
        "all four themes and compare them with the old frames.")
STEPS = ("You: The detail text is too small, make it readable",
         "Assistant: Measuring the fonts each theme ships",
         "You: Keep voice input exactly as it is")
APPROVAL = ("Q\tt3a1\tt3t1\tt3p1\t-\tpodbox\t1\t2\tRedesign the T3 thread list\n"
            "?\tRun command?\n?\tpython3 tools/tests/t3_redesign_sim.py\n"
            "o\t1\tApprove\t-\no\t2\tDecline\t-\no\t3\tApprove for session\t-")
USAGE = ("U\t30\t1\n"
         "P\tcodex\tok\t42\t7200\t18\t300000\t12345678\t41200\t\n"
         "P\tclaude\tok\t87\t3600\t64\t200000\t98765432\t182500\t\n"
         "P\tgrok\tnone\t-\t-\t-\t-\t-\t-\tNot configured\n"
         "P\tgemini\ttok\t-\t-\t-\t-\t2300000\t-\t\n"
         "N\t113411110\t223700")


def thread_row(tid, flat):
    status, question, name, project, last = THREADS[tid]
    title = LABELS[project] if flat else "gpt-5.5 · main"
    return f"{tid}\tcodex\t{status}\t{question}\t{name}\t{title}\t{last}"


def detail(tid):
    status, _, name, _, _ = THREADS[tid]
    word = {"b": "Waiting", "w": "Running", "d": "Completed", "i": "Idle"}[status]
    say = LONG if tid == "t3t3" else SHORT
    steps = STEPS if tid != "t3t2" else STEPS[:1] + ("", "")
    pending = "Run command?\npython3 tools/tests/t3_redesign_sim.py" if tid == "t3t1" else ""
    return "\t".join(("D", tid, "95", name, f"{word} · gpt-5.5", say, *steps,
                      pending.replace("\n", "\\n"), "0"))


class Peer(Player):
    """A HerdRemote daemon with two projects, five threads and usage."""

    def receive(self, seconds):
        if self.process.poll() is not None:
            raise AssertionError(f"Simulator exited. See {self.directory / 'sim.log'}")
        now = time.monotonic()
        if self.tick and now >= self.tick_at:
            self.send(self.tick)
            self.tick_at = now + 1
        if not select.select([self.peer], [], [], min(seconds, .25))[0]:
            return
        message = self.peer.recv(16384).decode()
        self.trace.write(f"player {message!r}\n")
        fields = message.split("\t")
        self.messages.append(fields)
        key = fields[0]
        if key.startswith(":") and key[1:] in THEMES:
            fields[0] = key[1:]
            self.state = State.decode(fields)
        elif key == "hr":
            self.send("HR\t1\tFixture computer")
            self.send("M\tt3")
            self.send("TL\t1")
        elif key == "tc":
            self.send(PROJECTS)
            self.send(STATUS)
            self.tick = "T\t0\t1\t1"
            self.tick_at = 0
        elif key == "sub" and fields[1] in ("t3p1", "t3p2", "t3"):
            flat = fields[1] == "t3"
            ids = ALL if fields[-1] == "all" else ACTIVE
            if not flat:
                ids = [t for t in ids if THREADS[t][3] == fields[1]]
                self.send(f"T\t{fields[1]}\t2\t0\tactive\tActive\tb\n"
                          f"T\t{fields[1]}\t2\t1\tall\tAll\tb")
            self.send(f"A\t{fields[1]}\t{len(ids)}\n" +
                      "\n".join(thread_row(t, flat) for t in ids))
        elif key == "det" and fields[1] in THREADS:
            self.send(detail(fields[1]))
        elif key == "us":
            self.send(USAGE)
        elif key == "ks":
            self.send(f"K\t{fields[1]}\t2\ninterrupt\tInterrupt turn\t1\n"
                      "reply1\tContinue\t0")
        elif key in ("fp", "vd", "vu", "vc", "act", "a"):
            self.send(f"R\t{fields[1]}\tok\tAccepted")


def latest_frame(state):
    _, sequence, _ = HEADER.unpack_from(state)
    start = HEADER.size + ((sequence - 1) % RING) * (WORD.size + PIXELS)
    assert sequence and WORD.unpack_from(state, start)[0] == sequence
    raw = bytes(state[start + WORD.size:start + WORD.size + PIXELS])
    return Image.frombytes("RGB", (320, 240), raw)


def find(player, label, direction=None):
    """Select the row ending in label, searching up and then down."""
    for step in ([direction] if direction else ["up", "down"]):
        for _ in range(12):
            if player.state.label.endswith(label):
                return
            player.tap(step)
    if not player.state.label.endswith(label):
        raise AssertionError(("Row not found", label, player.state))


def run_theme(binary, output, resources, theme, check):
    directory = output / theme
    shutil.rmtree(directory / "disk", ignore_errors=True)
    directory.mkdir(parents=True, exist_ok=True)
    disk = prepare_disk(directory, binary.parent, resources, theme)
    path = directory / "frames.bin"
    report = []
    with path.open("w+b") as file:
        file.truncate(STATE)
        with mmap.mmap(file.fileno(), STATE, access=mmap.ACCESS_READ) as state, \
                Peer(binary, directory, disk, path) as player:
            def capture(name, view):
                player.expect_view(view)
                player.wait(.7)
                latest_frame(state).save(directory / f"{name}.png")

            player.expect(lambda: ["tc", "2"] in player.messages, "handshake", timeout=10)
            player.wait(1)
            flat = any(f[:2] == ["sub", "t3"] for f in player.messages)
            player.expect(lambda: player.state and player.state.rows > 1 and
                          not player.state.issue, "first page", timeout=5)
            if flat:
                find(player, THREADS["t3t1"][2], "up")
                capture("list", "projects")
            else:
                capture("projects", "projects")
                find(player, "podbox", "up")
                player.tap("select")
                player.expect(lambda: player.state.view == "threads" and
                              player.state.rows >= 1, "project threads")
                find(player, THREADS["t3t1"][2], "up")
                capture("list", "threads")
            list_view = player.state.view
            project = "t3p1"

            def open_thread(tid):
                nonlocal project
                if not flat and THREADS[tid][3] != project:
                    player.tap("menu")
                    player.expect_view("projects")
                    project = THREADS[tid][3]
                    find(player, LABELS[project])
                    player.tap("select")
                    player.expect(lambda: player.state.view == "threads" and
                                  player.state.rows >= 1, "project threads")
                find(player, THREADS[tid][2])
                player.tap("select")

            if check:
                mark = len(player.messages)
                for key in ("down", "down", "down", "up", "up", "up"):
                    player.tap(key)
                player.wait(.6)
                player.no_wire(mark, "fp", "fw")
                report.append(f"{theme}: 6 wheel steps over the thread list sent no fp or fw")
            find(player, THREADS["t3t2"][2])
            mark = len(player.messages)
            player.tap("select")
            capture("detail-short", "detail")
            sent = [f for f in player.messages[mark:] if f[0] in ("fp", "fw")]
            if check:
                assert [f[2] for f in sent] == ["t3t2"], sent
                report.append(f"{theme}: SELECT sent exactly one fp, for t3t2")
            player.tap("menu")
            player.expect_view(list_view)
            open_thread("t3t3")
            capture("detail-long", "detail")
            player.tap("right", .55)
            player.wait(.3)
            capture("detail-long-end", "detail")
            player.tap("menu")
            player.expect_view(list_view)
            open_thread("t3t1")
            player.expect_view("detail")
            player.wait(.4)
            player.send(APPROVAL)
            capture("approval", "approval")
            player.tap("menu")
            capture("detail-question", "detail")
            player.tap("menu")
            player.expect_view(list_view)
            if not flat:
                player.tap("menu")
                player.expect_view("projects")
            player.tap("back")
            capture("usage", "usage")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--resources", required=True, type=Path,
                        help="built .rockbox resource directory")
    parser.add_argument("--simulator", type=Path,
                        help="reuse an instrumented binary from a previous run")
    parser.add_argument("--theme", choices=THEMES, action="append")
    parser.add_argument("--check", action="store_true",
                        help="assert the wheel sends no fp and SELECT sends one")
    args = parser.parse_args()
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    binary = (args.simulator.resolve() if args.simulator
              else build_simulator(args.output, args.jobs))
    for theme in args.theme or THEMES:
        for line in run_theme(binary, args.output, args.resources.resolve(), theme,
                              args.check):
            print(line, flush=True)
        print(f"{theme}: captured", flush=True)


if __name__ == "__main__":
    main()
