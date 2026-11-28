#!/usr/bin/env python3
"""Test T3 pocket remote buttons with an ipod6g SDL simulator and a synthetic peer.

The build uses a disposable source copy, with the real input, UI and playback
code. Only the USB transport, startup target and state observation are supplied
by the fixture. SDL uses the dummy video and audio drivers throughout.
"""
import argparse
from dataclasses import dataclass
import mmap
import os
from pathlib import Path
import select
import shutil
import socket
import subprocess
import time
import wave

from PIL import Image
from ui_stock_transitions_sim import HEADER, RING, STATE, WORD, PIXELS

REPO = Path(__file__).resolve().parents[2]
FIXTURES = Path(__file__).resolve().parent / "fixtures/t3-remote"


def replace(source, name, old, new):
    path = source / name
    data = path.read_text()
    assert data.count(old) == 1, (name, old)
    path.write_text(data.replace(old, new))


def copy_source(source):
    source.mkdir()
    names = subprocess.check_output([
        "git", "ls-files", "-z", "--cached", "--others", "--exclude-standard",
    ], cwd=REPO).decode().split("\0")
    for name in filter(None, names):
        original = REPO / name
        if original.is_file() or original.is_symlink():
            copy = source / name
            copy.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(original, copy, follow_symlinks=False)


def instrument(source):
    replace(source, "firmware/export/config/sim.h", "#undef HAVE_USB_PODLINK",
            "#define HAVE_USB_PODLINK")
    replace(source, "firmware/SOURCES", "usbstack/podlink_rust_platform.c\n", "")
    replace(source, "firmware/export/usb.h",
            "void usb_storage_ejected(void);\n#endif /* HAVE_USBSTACK */",
            "void usb_storage_ejected(void);\n#else\n"
            "static inline bool usb_exclusive_storage(void) { return false; }\n"
            "#endif /* HAVE_USBSTACK */")
    replace(source, "apps-ipod/SOURCES", "system/podlink_shell_lcd.c\n", "")
    replace(source, "apps-ipod/SOURCES", "sim/shim-cider.c\n", "")
    replace(source, "apps-ipod/SOURCES", "sim/shim-usb.c\n",
            "sim/shim-usb.c\nsim/t3-test-podlink.c\n")
    replace(source, "apps-ipod/root_menu.c", "    next_screen = root_menu_setup_screens();",
            "    next_screen = root_menu_setup_screens();\n"
            "    extern int t3_test_start(void);\n    next_screen = t3_test_start();")
    for function, label in (("wpsscrn(void* param)", "wps"),
                            ("agents_scrn(void *param)", "agents")):
        mark = f"static int {function}\n{{\n"
        replace(source, "apps-ipod/root_menu.c", mark, mark +
                f'    extern void t3_test_observe(const char *);\n'
                f'    t3_test_observe("entry\\t{label}");\n')
    actions = source / "apps-ipod/input/action.c"
    if '#include "system/activity.h"' not in actions.read_text():
        replace(source, "apps-ipod/input/action.c", '#include "action.h"',
                '#include "action.h"\n#include "system/activity.h"')
    replace(source, "apps-ipod/screens/playback/cider.c", "< 1152 * 1024,", "< 2048 * 1024,")
    replace(source, "apps-ipod/screens/system/agents_screen.c",
            "        bool changed = poll_link();",
            "#include \"t3-test-observe.inc\"\n        bool changed = poll_link();")
    replace(source, "apps-ipod/system/shutdown.c", "    switch(event)\n",
            "    extern void t3_test_media(long);\n    t3_test_media(event);\n    switch(event)\n")
    transport = "void wps_wheel_transport(int action)\n{\n"
    replace(source, "apps-ipod/screens/playback/wps.c", transport, transport +
            "    extern void t3_test_transport(int);\n    t3_test_transport(action);\n")
    seek = "void audio_ff_rewind(long time)\n{\n"
    replace(source, "apps-ipod/audio/playback.c", seek, seek +
            "    extern void t3_test_seek(long);\n    t3_test_seek(time);\n")
    shutil.copyfile(FIXTURES / "podlink.c", source / "apps-ipod/sim/t3-test-podlink.c")
    shutil.copyfile(FIXTURES / "observe.inc",
                    source / "apps-ipod/screens/system/t3-test-observe.inc")

def build_simulator(output, jobs):
    source = output / "source"
    copy_source(source)
    instrument(source)
    build = output / "build"
    build.mkdir()
    log_path = output / "build.log"
    with log_path.open("wb") as log:
        for command in (
            [str(source / "tools/configure"), "--target=ipod6g", "--type=s", "--ram=64",
             "--appsdir=apps-ipod", "--rbdir=/.rockbox"],
            ["make", f"-j{jobs}", "bin", "codecs"],
        ):
            if subprocess.run(command, cwd=build, stdout=log, stderr=subprocess.STDOUT).returncode:
                raise RuntimeError(f"Simulator build failed. See {log_path}")
    return build / "rockboxui"


THEMES = ("classic", "scrim", "fruit-company", "fruit-company-ultra")
PROJECTS = "W\t1\n#\tT3\nt3p1\tf\t1\t0\t0\t1\tFixture project"
TABS = "T\tt3p1\t2\t0\tactive\tActive\tw\nT\tt3p1\t2\t1\tall\tAll\tw"
THREAD = "t3t1\tcodex\tw\t0\tFixture thread\tgpt · main\tWorking on tests"
ACTIONS = ("K\tt3t1\t3\ninterrupt\tInterrupt turn\t1\nreply1\tContinue\t0"
           "\narchive\tArchive thread\t1")
SUMMARY = " ".join(f"Summary section {i} stays readable." for i in range(9))
DETAIL = "\t".join(("D", "t3t1", "132", "Task", "Run tests", SUMMARY,
                    "First tool step", "Second tool step", "Third tool step", "", "0"))


def approval(qid):
    return (f"Q\t{qid}\tt3t1\tt3p1\t-\tFixture project\t1\t2\tFixture thread\n"
            "?\tRun command?\n?\tpython3 run-tests.py\n"
            "o\t1\tApprove\t-\no\t2\tDecline\t-\no\t3\tApprove for session\t-")


def prepare_disk(directory, build, resources, theme):
    rockbox = directory / "disk/.rockbox"
    fonts = rockbox / "fonts"
    fonts.mkdir(parents=True)
    for asset in (resources / "fonts").iterdir():
        if asset.suffix in (".fnt", ".pbf"):
            (fonts / asset.name).symlink_to(asset.resolve())
    codecs = rockbox / "codecs"
    codecs.mkdir()
    (codecs / "wav.codec").symlink_to(build / "lib/rbcodec/codecs/wav.codec")
    themes = rockbox / "native-themes"
    themes.mkdir()
    art = resources / "themes/stock-art.bin"
    if art.exists():
        (rockbox / "themes").mkdir()
        (rockbox / "themes/stock-art.bin").symlink_to(art.resolve())
    (rockbox / "config.cfg").write_text(
        f"native theme: {theme}\nanimations: off\nbacklight timeout: off\n"
        "idle poweroff: off\nrepeat: all\n")
    for name in ("one", "two"):
        with wave.open(str(rockbox.parent / f"{name}.wav"), "wb") as track:
            track.setparams((1, 2, 44100, 0, "NONE", "not compressed"))
            track.writeframes(bytes(2 * 44100 * 30))
    return rockbox.parent


@dataclass(frozen=True)
class State:
    theme: str
    view: str
    selected: int
    rows: int
    scroll: int
    scroll_max: int
    talking: int
    questions: int
    tab: str
    label: str
    issue: str

    @classmethod
    def decode(cls, fields):
        return cls(*fields[:2], *map(int, fields[2:8]), *fields[8:])


class Player:
    def __init__(self, binary, directory, disk, frame_state=None, start="agents"):
        self.binary = binary
        self.directory = directory
        self.disk = disk
        self.messages = []
        self.state = None
        self.tick = ""
        self.tick_at = 0
        self.respond = True
        self.frame_state = frame_state
        self.start = start

    def __enter__(self):
        self.peer, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_DGRAM)
        self.log = (self.directory / "sim.log").open("wb")
        self.trace = (self.directory / "wire.log").open("w", buffering=1)
        env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy",
                   SDL_RENDER_DRIVER="software", T3_TEST_FD=str(child.fileno()),
                   T3_TEST_START=self.start)
        if self.frame_state is not None:
            env["PODBOX_UI_WALK_STATE"] = str(self.frame_state)
        try:
            self.process = subprocess.Popen([
                str(self.binary), "--root", str(self.disk), "--nobackground",
                "--zoom", "1", "--screenshot", "--keys", "-",
            ], cwd=self.binary.parent, env=env, pass_fds=(child.fileno(),),
                stdin=subprocess.PIPE, stdout=self.log, stderr=self.log)
        finally:
            child.close()
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
        self.log.close()
        self.trace.close()
        self.peer.close()

    def send(self, message):
        self.trace.write(f"host {message!r}\n")
        self.peer.send(message.encode())

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
        elif not self.respond:
            return
        elif key == "hr":
            self.send("HR\t1\tFixture computer")
            self.send("M\tt3")
        elif key == "tc":
            self.send(PROJECTS)
            self.send("S\t1\nt3p1\tw\t-")
            self.tick = "T\t0\t1\t1" if fields[1] == "2" else "T\t0\t1"
            self.tick_at = 0
        elif key == "sub" and fields[1] == "t3p1":
            self.send(TABS)
            extra = "\nt3t2\tcodex\ti\t0\tOlder thread\tgpt · main\tFinished"
            all_threads = fields[-1] == "all"
            self.send(f"A\tt3p1\t{2 if all_threads else 1}\n" + THREAD +
                      (extra if all_threads else ""))
        elif key == "det" and fields[1] == "t3t1":
            self.send(DETAIL)
        elif key == "ks":
            assert fields[1] == "t3t1", fields
            self.send(ACTIONS)
        elif key in ("fp", "vd", "vu", "vc", "act", "a"):
            self.send(f"R\t{fields[1]}\tok\tAccepted")
            if key == "a":
                self.send(f"X\t{fields[2]}")

    def wait(self, seconds):
        end = time.monotonic() + seconds
        while (left := end - time.monotonic()) > 0:
            self.receive(left)

    def expect(self, predicate, reason, timeout=3):
        end = time.monotonic() + timeout
        while not predicate() and (left := end - time.monotonic()) > 0:
            self.receive(left)
        assert predicate(), (reason, self.state, self.messages[-12:], self.directory)

    def expect_view(self, name):
        self.expect(lambda: self.state and self.state.view == name, name)

    def expect_wire(self, since, key, *arguments):
        def received():
            for fields in self.messages[since:]:
                suffix = fields[2:] if key in ("vd", "vu", "vc", "a", "act", "fp") else fields[1:]
                if fields[0] == key and tuple(suffix) == arguments:
                    return True
            return False
        self.expect(received, (key, arguments))

    def no_wire(self, since, *keys):
        records = [fields for fields in self.messages[since:] if fields[0] in keys]
        assert not records, ("Unwanted wire messages", records, self.state)

    def keys(self, *events):
        for kind, key in events:
            self.trace.write(f"key {kind} {key}\n")
        self.process.stdin.write("".join(f"{kind} {key}\n" for kind, key in events).encode())
        self.process.stdin.flush()

    def tap(self, key, duration=None):
        if duration is None:
            duration = .14 if key in ("back", "right", "select") else .075
        self.keys(("down", key))
        self.wait(duration)
        self.keys(("up", key))
        self.wait(.18)

    def choose(self, label, direction="down"):
        for _ in range(10):
            if self.state.label.endswith(label):
                self.tap("select")
                return
            self.tap(direction)
        raise AssertionError(("Row not selectable", label, self.state))

    def voice_hold(self, key, target, duration):
        state = self.state
        mark = len(self.messages)
        self.keys(("down", key))
        self.wait(duration)
        self.expect_wire(mark, "vd", target)
        self.keys(("up", key))
        self.expect_wire(mark, "vu")
        self.expect(lambda: self.state == state, "release preserves the view")
        self.no_wire(mark, ":playback", "a", "act", "k", "ks", "sub")

    def chord(self, destination):
        mark = len(self.messages)
        self.keys(("down", "select"), ("down", "play"))
        self.wait(.09)
        self.keys(("up", "select"), ("up", "play"))
        self.expect_wire(mark, ":entry", destination)
        self.wait(.15)
        self.no_wire(mark, ":playback", "vd", "vu", "a", "act")


def run_theme(binary, output, resources, theme):
    directory = output / theme
    directory.mkdir()
    disk = prepare_disk(directory, binary.parent, resources, theme)
    fruit = theme.startswith("fruit-company")
    with Player(binary, directory, disk) as player:
        player.expect(lambda: ["tc", "2"] in player.messages, "Agents handshake", timeout=10)
        player.wait(.2)
        if fruit and player.state.label == "Usage":
            player.tap("up")
        player.expect(lambda: player.state and player.state.label.endswith("Fixture project"),
                      "project appears after hello")
        assert player.state.theme == theme, player.state
        theme_log = disk / ".rockbox/native-theme.log"
        assert not theme_log.exists(), theme_log.read_text() if theme_log.exists() else ""
        assert player.state.view == "projects" and player.state.rows == (2 if fruit else 1)
        assert player.messages.index(["tc", "1"]) == player.messages.index(["hr", "1"]) + 1
        mark = len(player.messages)
        player.tap("right")
        player.expect_view("projects")
        player.tap("back")
        player.expect_view("usage")
        player.no_wire(mark, "fw", "k")
        mark = len(player.messages)
        player.tap("select")
        player.expect_wire(mark, "us")
        player.tap("down")
        player.no_wire(mark, "k")
        player.voice_hold("play", "-", .23)
        if fruit:
            player.voice_hold("select", "-", .5)
        player.tap("right")
        player.expect_view("projects")
        player.choose("Fixture project", direction="up")
        player.expect(lambda: player.state.view == "threads" and player.state.label.endswith("Fixture thread"),
                      "initial Active thread")
        player.wait(.15)
        mark = len(player.messages)
        player.tap("right")
        player.expect_wire(mark, "sub", "t3p1", "all")
        player.expect(lambda: player.state.rows == (3 if fruit else 2), "All snapshot loaded")
        mark = len(player.messages)
        player.tap("back")
        player.expect_wire(mark, "sub", "t3p1", "active")
        player.expect(lambda: player.state.rows == (2 if fruit else 1) and
                      player.state.label.endswith("Fixture thread"), "Active snapshot loaded")
        player.choose("Fixture thread")
        player.expect(lambda: player.state.view == "detail" and player.state.scroll_max > 1,
                      "scrollable thread detail")
        player.tap("right", .55)
        player.expect(lambda: player.state.scroll == player.state.scroll_max,
                      "RIGHT hold jumps to the bottom")
        player.tap("back")
        player.expect(lambda: player.state.scroll < player.state.scroll_max, "LEFT pages up")
        player.tap("back", .55)
        player.expect(lambda: player.state.scroll == 0, "LEFT hold jumps to the top")
        player.voice_hold("play", "t3t1", .22)
        if fruit:
            player.voice_hold("select", "t3t1", .5)
        player.tap("select")
        player.expect_view("actions")
        assert player.state.rows == (4 if fruit else 3), player.state
        if fruit:
            mark = len(player.messages)
            player.choose("Voice reply")
            player.expect_wire(mark, "vd", "t3t1")
            player.expect_view("detail")
            player.tap("select")
            player.expect_wire(mark, "vu")
            player.tap("select")
            player.expect_view("actions")
            mark = len(player.messages)
            player.choose("Voice reply")
            player.expect_wire(mark, "vd", "t3t1")
            player.tap("menu")
            player.expect_wire(mark, "vc")
            player.expect_view("detail")
            player.tap("select")
            player.expect_view("actions")
        player.choose("Interrupt turn")
        player.expect_view("confirm")
        mark = len(player.messages)
        player.tap("select")
        player.expect_view("actions")
        player.no_wire(mark, "act")
        player.tap("select")
        player.expect_view("confirm")
        player.tap("right")
        player.tap("select")
        player.expect_wire(mark, "act", "t3t1", "interrupt")
        player.expect_view("detail")
        player.send(approval("t3a1"))
        player.expect_view("approval")
        mark = len(player.messages)
        player.tap("select" if fruit else "back", .05)
        player.expect_view("approval")
        player.no_wire(mark, "a")
        player.wait(.25)
        mark = len(player.messages)
        player.tap("back", .45)
        player.expect_view("approval")
        player.no_wire(mark, "a")
        mark = len(player.messages)
        player.tap("select")
        player.expect_view("question")
        player.no_wire(mark, "a")
        player.tap("right")
        player.tap("back")
        player.tap("menu")
        player.expect_view("detail")
        assert player.state.questions == 1, player.state
        player.no_wire(mark, "a", "qi")
        player.send("X\tt3a1")
        player.send(approval("t3a2"))
        player.expect_view("approval")
        mark = len(player.messages)
        player.wait(.28)
        player.tap("right")
        player.expect_wire(mark, "a", "t3a2", "2")
        player.expect_view("detail")
        player.send("Q\tt3q1\tt3t1\tt3p1\t-\tFixture project\t0\t0\tFixture thread\n"
                    "?\tPick a preset\no\t1\tRun checks\t-\no\t2\tWrite a response\tc")
        player.expect_view("question")
        mark = len(player.messages)
        player.tap("down")
        assert player.state.label.endswith("Dismiss"), player.state
        player.tap("select")
        player.expect_wire(mark, "a", "t3q1", "-2")
        player.expect_view("detail")
        player.chord("wps")
        player.chord("agents")
        player.expect_view("detail")
    return f"{theme}: Projects, Threads, Detail, Approval, All Options, Actions, Confirm, Usage, WPS chord PASS"


def run_navigation(binary, output, resources, theme):
    directory = output / f"{theme}-navigation"
    disk = prepare_disk(directory, binary.parent, resources, theme)
    with Player(binary, directory, disk, start="menu") as player:
        player.wait(2)
        for key, name in (("back", "prev"), ("right", "next"), ("play", "play")):
            mark = len(player.messages)
            player.tap(key)
            player.expect_wire(mark, ":transport", name)
            player.no_wire(mark, ":entry", "hr")
    return f"{theme}: Home LEFT/RIGHT/PLAY transport PASS"


def run_mode_handshake(binary, output, resources):
    directory = output / "mode"
    disk = prepare_disk(directory, binary.parent, resources, "fruit-company")
    with Player(binary, directory, disk) as player:
        player.respond = False
        player.expect(lambda: ["tc", "2"] in player.messages, "initial handshake", timeout=10)
        player.send("HR\t1\tFixture")
        player.send("M\therdr")
        player.send("W\t1\nw1\t-\t0\t0\t0\t1\tHerd workspace")
        player.expect(lambda: player.state and player.state.label == "Herd workspace",
                      "explicit herdr snapshot")
        player.chord("wps")
        mark = len(player.messages)
        player.chord("agents")
        player.expect_wire(mark, "tc", "1")
        handshake = player.messages.index(["hr", "1"], mark)
        assert player.messages[handshake + 1] == ["tc", "1"], player.messages[mark:]
        assert not any("Herd workspace" in fields for fields in player.messages[mark:])
        player.send("HR\t1\tFixture")
        player.send("M\tt3")
        player.expect(lambda: player.state.issue == "Connecting to T3…" and
                      player.state.label == "Usage", "T3 connecting without stale herdr rows")
        player.send("W\t0")
        player.expect(lambda: not player.state.issue and player.state.label == "Usage",
                      "empty explicit T3 snapshot")
        player.send(PROJECTS)
        player.expect(lambda: player.state.label == "Fixture project", "late T3 snapshot")
        player.send("M\therdr")
        player.send("W\t1\nw1\t-\t0\t0\t0\t1\tHerd workspace")
        player.expect(lambda: player.state.label == "Herd workspace",
                      "live mode change back to herdr")
    return "Explicit mode, empty T3 snapshot, resident reattach and live source change PASS"


def agents_frames(binary, output, resources, theme):
    directory = output / theme
    disk = prepare_disk(directory, binary.parent, resources, theme)
    config = disk / ".rockbox/config.cfg"
    config.write_text(config.read_text().replace("animations: off", "animations: on"))
    path = directory / "frames.bin"
    frames = {}
    with path.open("w+b") as file:
        file.truncate(STATE)
        with mmap.mmap(file.fileno(), STATE, access=mmap.ACCESS_READ) as state, \
                Player(binary, directory, disk, path) as player:
            def capture(view):
                player.expect_view(view)
                player.wait(.6)
                _, sequence, _ = HEADER.unpack_from(state)
                start = HEADER.size + ((sequence - 1) % RING) * (WORD.size + PIXELS)
                assert sequence and WORD.unpack_from(state, start)[0] == sequence
                raw = bytes(state[start + WORD.size:start + WORD.size + PIXELS])
                assert WORD.unpack_from(state, start)[0] == sequence
                # The clock belongs to the simulator's host. Compare the app
                # viewport including the Agents status-bar title.
                frame = Image.frombytes("RGB", (320, 240), raw)
                frame.paste((0, 0, 0), (255, 0, 320, 20))
                frame.save(directory / f"{view}.png")
                frames[view] = frame.tobytes()

            player.expect(lambda: player.state and player.state.label == "Fixture project",
                          "opening projects", timeout=10)
            player.tick = "T\t0\t1\t1"
            player.send("S\t1\nt3p1\ti\t-")
            capture("projects")
            player.tap("back")
            capture("usage")
            player.tap("right")
            player.choose("Fixture project", direction="up")
            player.expect_view("threads")
            player.send("A\tt3p1\t1\n" + THREAD.replace("\tw\t0\t", "\ti\t0\t"))
            capture("threads")
            player.send("A\tt3p1\t1\n" + THREAD)
            player.wait(.18)
            player.choose("Fixture thread")
            capture("detail")
            player.tap("select")
            capture("actions")
            player.choose("Interrupt turn")
            capture("confirm")
    return frames


def compare_agents_motion(binary, output, resources):
    fruit = agents_frames(binary, output, resources, "fruit-company")
    ultra = agents_frames(binary, output, resources, "fruit-company-ultra")
    for view in fruit:
        assert fruit[view] == ultra[view], (view, output)
    return "Fruit Company and Ultra: six Agents view frames match pixel for pixel"


def run_outage(binary, output, resources):
    directory = output / "outage"
    disk = prepare_disk(directory, binary.parent, resources, "fruit-company")
    with Player(binary, directory, disk) as player:
        player.expect(lambda: player.state and not player.state.issue and
                      player.state.label == "Fixture project", "connected project list")
        player.choose("Fixture project")
        player.expect_view("threads")
        player.choose("[Active]   All", direction="up")
        player.expect(lambda: player.state.rows == 3, "two All thread rows")
        player.respond = False
        player.tick = ""
        player.send("!b")
        player.tap("down")
        player.expect(lambda: player.state.label == "Older thread",
                      "wheel while sends return BUSY")
        player.tap("up")
        player.wait(6.4)
        player.expect(lambda: player.state.issue.startswith("No update for"),
                      "stalled peer status")
        player.tap("down")
        player.expect(lambda: player.state.label == "Older thread",
                      "wheel under sustained backpressure")
        player.send("!d")
        player.expect(lambda: player.state.issue == "Computer disconnected" and
                      player.state.view == "threads", "disconnected T3 list")
        player.tap("up")
        player.expect(lambda: player.state.label == "Fixture thread",
                      "wheel while disconnected")
        mark = len(player.messages)
        player.respond = True
        player.send("!r")
        player.expect_wire(mark, "sub", "t3p1", "all")
        player.expect(lambda: not player.state.issue and
                      player.state.label == "Fixture thread", "same view and row after reconnect")
        player.tap("down")
        player.expect(lambda: player.state.label == "Older thread",
                      "wheel after reconnect")
    return "T3 wheel, BUSY, heartbeat stall, session loss and resubscribe PASS"


def working_badges(image):
    """Find the uniform green 7x7 working dot in the header and pill area."""
    pixels = image.load()
    dots = []
    widths = (3, 5, 7, 7, 7, 5, 3)
    for y in range(29):
        for x in range(314):
            color = pixels[x + 2, y]
            red, green, blue = color
            if green < max(red, blue) + 24 or pixels[x, y] == color:
                continue
            if all(pixels[x + dx, y + dy] == color
                   for dy, width in enumerate(widths)
                   for dx in range((7 - width) // 2, (7 + width) // 2)):
                dots.append((x, y))
    return dots


def run_wps_badge(binary, output, resources, theme):
    directory = output / theme
    directory.mkdir()
    disk = prepare_disk(directory, binary.parent, resources, theme)
    state_path = directory / "frames.bin"
    with state_path.open("w+b") as file:
        file.truncate(STATE)
        with mmap.mmap(file.fileno(), STATE, access=mmap.ACCESS_READ) as state, \
                Player(binary, directory, disk, state_path) as player:
            player.expect_wire(0, "tc", "2")
            player.expect(lambda: player.state and player.state.theme == theme and
                          player.state.rows > 0 and not player.state.issue,
                          "working project is visible")
            player.chord("wps")
            player.wait(2.0)
            seen = 0
            captures = 0
            max_dots = []
            image = None
            for _ in range(20):
                player.wait(.05)
                _, seq, _ = HEADER.unpack_from(state)
                if not seq or seq == seen:
                    continue
                start = HEADER.size + ((seq - 1) % RING) * (WORD.size + PIXELS)
                raw = bytes(state[start + WORD.size:start + WORD.size + PIXELS])
                if WORD.unpack_from(state, start)[0] != seq:
                    continue
                seen = seq
                captures += 1
                frame = Image.frombytes("RGB", (320, 240), raw)
                dots = working_badges(frame)
                if image is None or len(dots) > len(max_dots):
                    image, max_dots = frame, dots
    assert captures >= 2, (theme, "missing WPS frames", captures)
    image.save(directory / "now-playing.png")
    # Scrim uses a WPS layout without a status tray.
    expected = int(theme != "scrim")
    assert len(max_dots) == expected and all(x >= 160 for x, _ in max_dots), (
        theme, "expected working dots in the WPS tray", expected, max_dots)
    return f"{theme}: WPS working badge count {expected} PASS"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--resources", required=True, type=Path, help="built .rockbox resource directory")
    parser.add_argument("--simulator", type=Path, help="reuse an instrumented binary from a previous run")
    parser.add_argument("--theme", choices=THEMES, action="append")
    parser.add_argument("--outage", action="store_true")
    parser.add_argument("--mode", action="store_true")
    parser.add_argument("--compare-motion", action="store_true")
    parser.add_argument("--wps-badge", action="store_true")
    args = parser.parse_args()
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    binary = args.simulator.resolve() if args.simulator else build_simulator(args.output, args.jobs)
    if args.outage:
        print(run_outage(binary, args.output, args.resources.resolve()), flush=True)
        return
    if args.mode:
        print(run_mode_handshake(binary, args.output, args.resources.resolve()), flush=True)
        return
    if args.compare_motion:
        print(compare_agents_motion(binary, args.output, args.resources.resolve()), flush=True)
        return
    run = run_wps_badge if args.wps_badge else run_theme
    for theme in args.theme or THEMES:
        print(run(binary, args.output, args.resources.resolve(), theme), flush=True)
        if run == run_theme and theme.startswith("fruit-company"):
            print(run_navigation(binary, args.output, args.resources.resolve(), theme), flush=True)


if __name__ == "__main__":
    main()
