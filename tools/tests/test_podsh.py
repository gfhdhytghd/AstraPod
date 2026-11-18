"""FU88 host CLI tests with RAM-only PodLink and a temporary volume."""

from collections import deque
from contextlib import contextmanager
import json
import struct
from pathlib import Path
import sys

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "research"))
import podsh
import podlink


def test_round_trip_and_reconnect():
    class Device:
        def __init__(self):
            self.decoder = podlink.Decoder()
            self.reply = deque()

        def write(self, data):
            for frame in self.decoder.feed(data):
                if frame.type == podlink.OPEN:
                    self.reply.append(podlink.encode(podlink.OPEN, frame.seq, podlink.HELLO_DATA))
                elif frame.type == podlink.TEXT:
                    assert frame.payload.startswith(b"RS\t1\tPING\t")
                    self.reply.extend([
                        podlink.encode(podlink.TEXT, 1, b"DH\t1\tdisk-mode-v1"),
                        podlink.encode(podlink.TEXT, 2, b"RS\t1\tPONG\twrong-nonce"),
                        podlink.encode(podlink.TEXT, 3,
                                       b"RS\t1\tPONG\t" + frame.payload[10:]),
                    ])
            return len(data)

        def read(self, size):
            return self.reply.popleft() if self.reply else b""

    attempts = []

    @contextmanager
    def open_link(path, timeout):
        attempts.append(path)
        if len(attempts) == 1:
            raise OSError("simulated reboot")
        link = podlink.Link(Device(), timeout)
        link.open()
        yield link

    podsh.reconnect_ping("fake", 1.0, open_link=open_link)
    assert attempts == ["fake", "fake"]


def test_deadline():
    @contextmanager
    def disconnected(path, timeout):
        raise OSError("disconnected")
        yield

    with pytest.raises(TimeoutError, match="deadline"):
        podsh.reconnect_ping(None, 0.01, open_link=disconnected)


def test_file_cli_and_json(tmp_path, capsys):
    folder = tmp_path / ".rockbox" / "research"
    folder.mkdir(parents=True)
    (folder / "crash-1.txt").write_bytes(
        b"FU88 1 mock boot=17 kind=4 ticks=100 hz=100\n"
        b"pc=08001008 sp=08500000 lr=08001001\n"
        b"candidate=0800100c\n"
    )
    (folder / "log.txt").write_bytes(b"older\nnewest\n")
    outside = tmp_path / "outside"
    outside.write_bytes(b"outside")
    (folder / "crash-2.txt").symlink_to(outside)
    prefix = ["--volume", str(tmp_path)]
    podsh.main(prefix + ["crash", "list"])
    assert capsys.readouterr().out == "[1]\n"
    podsh.main(prefix + ["log", "tail", "--lines", "1"])
    assert capsys.readouterr().out == "newest\n"
    output = tmp_path / "collect.json"
    podsh.main(prefix + ["collect", str(output)])
    result = json.loads(output.read_text())
    assert result["crashes"][0]["boot"] == 17
    assert result["log"] == "older\nnewest\n"
    files = podsh.ResearchFiles(tmp_path)
    assert podsh.collect(files, [(0x08001000, 0x10, "fixture")])["crashes"][0]["frames"] == [
        {"field": "pc", "address": "0x08001008", "symbol": "fixture", "offset": 8},
        {"field": "lr", "address": "0x08001000", "symbol": "fixture", "offset": 0},
        {"field": "candidate", "address": "0x0800100c", "symbol": "fixture", "offset": 12},
    ]
    with pytest.raises(ValueError, match="regular research file"):
        files.crash_get(2)
    with pytest.raises(ValueError, match="slot"):
        files.crash_clear(16)
    podsh.main(prefix + ["crash", "clear", "1"])
    assert files.crash_list() == []
    assert outside.read_bytes() == b"outside"


# --- the shell library against the real firmware command core -------------

ROOT = Path(__file__).resolve().parents[2]


@pytest.fixture(scope="module")
def core(tmp_path_factory):
    """research_shell.c + the RAM-only port as a shared library."""
    import ctypes
    import shutil
    import subprocess
    if not shutil.which("cc"):
        pytest.skip("no C compiler")
    out = tmp_path_factory.mktemp("core")
    lib = out / "rs.so"
    subprocess.run(["cc", "-shared", "-fPIC", "-O1", "-I", str(ROOT / "firmware/export"),
                    str(ROOT / "firmware/research_shell.c"),
                    str(ROOT / "tools/tests/research_port_host.c"), "-o", str(lib)], check=True)
    files = out / "files"
    files.mkdir()
    import os
    os.environ["RS_TEST_DIR"] = str(files)
    so = ctypes.CDLL(str(lib))
    so.rs_exec.restype = ctypes.c_size_t
    so.rs_exec.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_void_p,
                           ctypes.POINTER(ctypes.c_bool)]
    so.fake_reset()
    return so


class CoreTransport:
    """A transport whose far end is the firmware command core. PING is
    answered like the Rust fast path; unrelated frames are mixed in."""

    def __init__(self, so, state):
        import ctypes
        self.so, self.state, self.ctypes = so, state, ctypes
        self.queue = deque([(podlink.TEXT, b"DH\t1\tdisk-mode-v1")])
        state["opens"] += 1

    def send(self, kind, payload):
        if self.state.get("dead"):
            raise OSError("device is rebooting")
        if payload.startswith(b"RS\t1\tPING\t"):
            self.queue.append((podlink.TEXT, b"RS\t1\tPONG\t" + payload[10:]))
            return
        assert kind == (podlink.BINARY if b"\n" in payload else podlink.TEXT)
        out = self.ctypes.create_string_buffer(16384)
        binary = self.ctypes.c_bool()
        n = self.so.rs_exec(payload, len(payload), out, self.ctypes.byref(binary))
        if n:
            self.queue.append((podlink.TEXT, b"RS\t1\tffff\tOK\tstale"))
            self.queue.append((podlink.BINARY if binary.value else podlink.TEXT, out.raw[:n]))
        if payload.split(b"\t")[3] == b"reboot":
            self.state["dead"] = 2       # refuses the next opens, like a reboot

    def receive(self, timeout):
        if self.queue:
            return self.queue.popleft()
        raise TimeoutError("nothing pending")

    def close(self):
        pass


def make_shell(core, state):
    def open_transport():
        if state.get("dead"):
            state["dead"] -= 1
            raise OSError("not enumerated yet")
        return CoreTransport(core, state)
    return podsh.Shell(open_transport, timeout=1, reconnect=5)


def test_shell_library_round_trip(core):
    state = {"opens": 0}
    with make_shell(core, state) as shell:
        assert shell.peek(0x38500C80) == [0x004E7309]
        base, size = shell.scratch()
        assert (base, size) == (0x09000000, 65536)
        blob = bytes((i * 31 + 7) & 0xFF for i in range(30000))   # > one frame
        shell.load(base, blob)
        assert shell.dump(base, len(blob)) == blob
        with pytest.raises(podsh.ShellError, match="flash/bootloader"):
            shell.poke(0x3C300000, 1, force=True)
        with pytest.raises(podsh.ShellError, match="--force"):
            shell.poke(0x38300000, 1)
        assert shell.call(base + 0x101, 1, 2, 3, 4)[0] == 11
        assert shell.perf(base + 0x100, 5)["n"] == 5
        assert shell.regs("sm1")[0xC80] == 0x004E7309
        shell.file_put("big.bin", blob)
        assert shell.file_get("big.bin") == blob
        assert shell.file_ls()["big.bin"] == len(blob)
        shell.file_put("crash-3.txt", b"FU88 1 v boot=3 kind=1 ticks=5 hz=100\npc=08000010\n")
        assert shell.crash_list() == [3]
        assert podsh.parse_crash(3, shell.crash_get(3).decode())["kind"] == 1
        shell.crash_clear(3)
        assert shell.crash_list() == []


def test_sm1_image_and_e1_experiment(core, tmp_path):
    # An X17I v1 image shaped like FU87 probe A: mode 0, 4 P rows, Y:0x600
    rows = [(i, i + 1, i + 2, i + 3) for i in range(0, 16, 4)]
    image = b"X17I" + struct.pack("<HH", 1, 3)
    image += struct.pack("<BBHII", 5, 0xFF, 0, 0, 0)
    image += struct.pack("<BBHII", 1, 0, 0, 0, 4) + b"".join(struct.pack("<4I", *r) for r in rows)
    image += struct.pack("<BBHII", 3, 0, 0, 0x600, 1) + struct.pack("<I", 0xA55A)
    assert len(image) == 112
    assert podsh.parse_x17i(image)[1] == ("p", 0, 0, rows)
    (tmp_path / "probe.x17i").write_bytes(image)
    spec = {"name": "E1-A", "steps": [
        {"run": "sm1 on", "expect": "re:sig=004e7309"},
        {"image": "probe.x17i", "passes": [0]},
        {"run": "peek 4 0x22024000", "expect": "00000001"},
        {"run": "peek 2 0x22038C00", "expect": "a55a"},
        {"run": "poke 4 0x38500984 0; sm1 wait 50; sm1 ack", "expect": "re:hit us=\\d+"},
        {"blob": "probe.x17i", "addr": "scratch+0x200"},
        {"run": "sm1 feed {scratch+0x10} {scratch+0x200} 112", "expect": ""},
        {"run": "peek 4 0x38500100", "expect": "09000011"},
        {"run": "sm1 off"},
    ]}
    state = {"opens": 0}
    shell = make_shell(core, state).connect()
    result = podsh.run_experiment(shell, spec, tmp_path)
    assert result["ok"], result
    assert result["steps"][1]["mode"] == 0
    assert result["baseline"]["crashes"] == []
    bad = dict(spec, steps=[{"run": "peek 2 0x22038C00", "expect": "4f87"}])
    failed = podsh.run_experiment(shell, bad, tmp_path)
    assert not failed["ok"] and "expected '4f87'" in failed["steps"][0]["error"]


def test_reboot_reconnects(core):
    state = {"opens": 0}
    shell = make_shell(core, state).connect()
    shell.reboot()                  # the device drops, refuses two opens, returns
    assert state["opens"] == 2
    assert shell.peek(0x38500C80) == [0x004E7309]


def test_request_retries_once_after_a_drop(core):
    state = {"opens": 0}
    shell = make_shell(core, state).connect()
    state["dead"] = 1               # the next send fails, then one open fails
    assert shell.peek(0x38500C80) == [0x004E7309]
    assert shell.reconnects == 1


def test_cli_with_injected_transport(core, capsys, tmp_path):
    state = {"opens": 0}

    def open_transport():
        return CoreTransport(core, state)
    podsh.main(["run", "peek 4 0x38500c80; ping"], open_transport=open_transport)
    assert capsys.readouterr().out == "004e7309; pong\n"
    out = tmp_path / "d.bin"
    podsh.main(["run", "dump 0x38500c80 4", "--out", str(out)], open_transport=open_transport)
    assert out.read_bytes() == bytes.fromhex("09734e00")
    with pytest.raises(SystemExit) as stop:
        podsh.main(["poke", "4", "0x3c300000", "1"], open_transport=open_transport)
    assert stop.value.code == 3
