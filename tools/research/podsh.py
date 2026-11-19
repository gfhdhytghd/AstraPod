#!/usr/bin/env python3
"""podsh: the FU88 research shell over PodLink.

Library and CLI. Requests are "RS\\t1\\t<id>\\t<command line>[\\n<bytes>]";
replies are "RS\\t1\\t<id>\\t<OK|ERR>\\t<text>[\\n<bytes>]". Transports: the
serial port directly (HerdRemote must not hold it), or a `podlink.py serve`
WebSocket bridge. The connection is reopened automatically after a reboot.
See docs/podbox/research-shell.md.
"""

import argparse
from bisect import bisect_right
from contextlib import contextmanager
import json
from pathlib import Path
import re
import secrets
import struct
import subprocess
import sys
import time

TOOLS = Path(__file__).resolve().parents[1]
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import podlink

CRASH_SLOTS = range(16)
MAX_RECORD_BYTES = 4096
DATA_MAX = 12288            # firmware RS_DATA_MAX
SM1, VIC0 = 0x38500000, 0x38E00000
RECONNECT_ERRORS = (OSError, RuntimeError, TimeoutError, EOFError, ConnectionError)


class ShellError(RuntimeError):
    """The firmware answered ERR."""


# --- transports ------------------------------------------------------------

class SerialTransport:
    """Direct PodLink session on the CDC tty (single owner)."""

    def __init__(self, port=None, timeout=5.0):
        self.serial = podlink.serial_port(port, timeout)
        self.link = podlink.Link(self.serial, timeout)
        try:
            self.link.open()
        except BaseException:
            self.serial.close()
            raise

    def send(self, kind, payload):
        self.link.send(kind, payload)

    def receive(self, timeout):
        frame = self.link.receive(timeout)
        return frame.type, frame.payload

    def close(self):
        try:
            self.link.close()
        except RECONNECT_ERRORS:
            pass
        finally:
            self.serial.close()


class WebSocketTransport:
    """A `python3 tools/podlink.py serve` bridge (one client at a time)."""

    def __init__(self, url, timeout=5.0):
        from websockets.sync.client import connect
        self.ws = connect(url, open_timeout=timeout, max_size=podlink.MAX_MESSAGE,
                          compression=None)

    def send(self, kind, payload):
        self.ws.send(payload.decode("ascii") if kind == podlink.TEXT else payload)

    def receive(self, timeout):
        message = self.ws.recv(timeout)
        if isinstance(message, str):
            return podlink.TEXT, message.encode("utf-8")
        return podlink.BINARY, message

    def close(self):
        self.ws.close()


def transport_factory(port=None, ws=None, timeout=5.0):
    if ws:
        return lambda: WebSocketTransport(ws, timeout)
    return lambda: SerialTransport(port, timeout)


# --- shell -----------------------------------------------------------------

class Shell:
    """One research shell session. `open_transport()` returns a transport;
    it is called again whenever the link drops (the device rebooted)."""

    def __init__(self, open_transport, timeout=10.0, reconnect=60.0):
        self.open_transport = open_transport
        self.timeout, self.reconnect_timeout = timeout, reconnect
        self.transport = None
        self.reconnects = 0
        self._scratch = None

    # connection
    def connect(self, deadline=None):
        """(Re)open the link and prove the shell answers, within deadline."""
        deadline = deadline or time.monotonic() + self.reconnect_timeout
        cause = None
        while time.monotonic() < deadline:
            self.close()
            try:
                self.transport = self.open_transport()
                self.ping(min(2.0, max(0.1, deadline - time.monotonic())))
                return self
            except RECONNECT_ERRORS as error:
                cause = error
                self.close()
                time.sleep(max(0, min(0.5, deadline - time.monotonic())))
        raise TimeoutError("PodLink research shell did not answer before the deadline") from cause

    def close(self):
        if self.transport:
            try:
                self.transport.close()
            except RECONNECT_ERRORS:
                pass
            self.transport = None

    def __enter__(self):
        return self if self.transport else self.connect()

    def __exit__(self, *exc):
        self.close()

    def _wait(self, prefix, timeout):
        deadline = time.monotonic() + timeout
        while (remaining := deadline - time.monotonic()) > 0:
            kind, payload = self.transport.receive(remaining)
            if kind in (podlink.TEXT, podlink.BINARY) and payload.startswith(prefix):
                return payload
        raise TimeoutError("research reply deadline expired")

    def ping(self, timeout=2.0):
        nonce = secrets.token_hex(8).encode()
        self.transport.send(podlink.TEXT, b"RS\t1\tPING\t" + nonce)
        self._wait(b"RS\t1\tPONG\t" + nonce, timeout)

    def request(self, line, data=None, timeout=None, retry=True):
        """Run a command line; returns (text, data bytes). Raises ShellError
        on ERR. A dropped link is reopened once, then the request repeats."""
        if "\n" in line or len(line) > 255:
            raise ValueError("command line must be one line of at most 255 characters")
        if data is not None and len(data) > DATA_MAX:
            raise ValueError("payload over 12288 bytes")
        if not self.transport:
            self.connect()
        rid = secrets.token_hex(4).encode()
        body = b"RS\t1\t" + rid + b"\t" + line.encode("ascii")
        if data is not None:
            body += b"\n" + bytes(data)
        try:
            self.transport.send(podlink.BINARY if data is not None else podlink.TEXT, body)
            reply = self._wait(b"RS\t1\t" + rid + b"\t", timeout or self.timeout)
        except RECONNECT_ERRORS:
            if not retry:
                raise
            self.reconnects += 1
            self.connect()
            return self.request(line, data, timeout, retry=False)
        head, _, blob = reply.partition(b"\n")
        fields = head.decode("ascii", "replace").split("\t", 4)
        status, text = fields[3], fields[4] if len(fields) > 4 else ""
        if status == "ERR":
            raise ShellError(text)
        return text, blob

    def run(self, line, data=None, timeout=None):
        return self.request(line, data, timeout)[0]

    # memory
    def peek(self, addr, width=4, count=1):
        return [int(v, 16) for v in self.run(f"peek {width} {addr:#x} {count}").split()]

    def poke(self, addr, value, width=4, force=False):
        self.run(f"poke {width} {addr:#x} {value:#x}" + (" --force" if force else ""))

    def dump(self, addr, length, width=None):
        out = bytearray()
        while len(out) < length:
            n = min(DATA_MAX, length - len(out))
            w = f" {width}" if width else ""
            out += self.request(f"dump {addr + len(out):#x} {n}{w}")[1]
        return bytes(out)

    def load(self, addr, data, width=None, force=False):
        for off in range(0, len(data), DATA_MAX):
            w = f" {width}" if width else ""
            f = " --force" if force else ""
            self.run(f"load {addr + off:#x}{w}{f}", data[off:off + DATA_MAX])

    def fill(self, addr, length, value, width=4, force=False):
        self.run(f"fill {addr:#x} {length} {value:#x} {width}" + (" --force" if force else ""))

    def memcpy(self, dst, src, length, force=False):
        self.run(f"memcpy {dst:#x} {src:#x} {length}" + (" --force" if force else ""))

    def wait(self, addr, mask, value, ms, width=4):
        return self.run(f"wait {addr:#x} {mask:#x} {value:#x} {ms} {width}")

    @staticmethod
    def _fields(text):
        return {k: v for k, v in re.findall(r"(\w+)=(\S+)", text)}

    def call(self, addr, *args, force=False):
        f = self._fields(self.run(f"call {addr:#x} " + " ".join(f"{a:#x}" for a in args)
                                  + (" --force" if force else ""), timeout=30))
        return int(f["r0"], 16), int(f["us"])

    def perf(self, addr, count, *args):
        f = self._fields(self.run(f"perf {addr:#x} {count} " + " ".join(f"{a:#x}" for a in args),
                                  timeout=30))
        return dict(r0=int(f["r0"], 16), n=int(f["n"]), us=int(f["us"]))

    def scratch(self):
        if self._scratch is None:
            f = self._fields(self.run("buf"))
            self._scratch = int(f["buf"], 16), int(f["size"])
        return self._scratch

    def info(self):
        return self._fields(self.run("ver"))

    def regs(self, block):
        return {int(k, 16): int(v, 16) for k, v in
                re.findall(r"([0-9a-f]+)=([0-9a-f]{8})", self.run(f"regs {block}"))}

    # files under /.rockbox/research
    def file_get(self, name):
        out = bytearray()
        while True:
            chunk = self.request(f"file get {name} {len(out)} {DATA_MAX}")[1]
            out += chunk
            if len(chunk) < DATA_MAX:
                return bytes(out)

    def file_put(self, name, data):
        self.run(f"file put {name}", data[:DATA_MAX])
        for off in range(DATA_MAX, len(data), DATA_MAX):
            self.run(f"file put {name} {off}", data[off:off + DATA_MAX])

    def file_rm(self, name):
        self.run(f"file rm {name}")

    def file_ls(self):
        return {k: int(v) for k, v in re.findall(r"(\S+):(\d+)", self.run("file ls"))}

    def crash_list(self):
        return sorted(int(m.group(1)) for name in self.file_ls()
                      if (m := re.fullmatch(r"crash-(\d+)\.txt", name)))

    def crash_get(self, slot):
        return self.file_get(f"crash-{slot}.txt")

    def crash_clear(self, slot):
        self.file_rm(f"crash-{slot}.txt")

    def log_tail(self, lines=32):
        try:
            data = self.file_get("log.txt")
        except ShellError:
            return b""
        return b"".join(data.splitlines(keepends=True)[-lines:])

    def reboot(self, wait=True):
        self.run("reboot")
        self.close()
        if wait:
            time.sleep(1.0)
            self.connect()

    # sm1 helpers (docs/podbox/research-shell.md, FU87-REPORT.md §6)
    def sm1(self, line, data=None, timeout=None):
        return self.run("sm1 " + line, data, timeout)

    def sm1_load_section(self, kind, addr, words):
        """kind 'p': list of rows (4 u32 lanes); 'x': u32 words; 'y': u16 words."""
        if kind == "p":
            raw = b"".join(struct.pack("<4I", *row) for row in words)
            unit = 16
        elif kind == "x":
            raw, unit = struct.pack(f"<{len(words)}I", *words), 4
        else:
            raw, unit = struct.pack(f"<{len(words)}H", *[w & 0xFFFF for w in words]), 2
        step = DATA_MAX // unit * unit
        for off in range(0, len(raw), step):
            self.sm1(f"load {kind} {addr + off // unit:#x}", raw[off:off + step])

    def sm1_param(self, offset, halfwords):
        for i in range(0, len(halfwords), 16):
            part = halfwords[i:i + 16]
            self.sm1(f"param {offset + i:#x} " + " ".join(f"{h & 0xFFFF:#x}" for h in part))

    def sm1_load_image(self, image, passes=None):
        """Load an X17I v1 image (or the chosen passes of it). Returns its mode."""
        mode = None
        for section in parse_x17i(image):
            kind, pass_, addr, words = section
            if kind == "mode":
                mode = addr
            elif passes is None or pass_ in passes:
                if kind == "param":
                    self.sm1_param(addr - 0xFD80 if addr >= 0xFD80 else addr, words)
                else:
                    self.sm1_load_section(kind, addr, words)
        return mode

    def sm1_cmd70(self, register, polls_ms=10):
        """Chikuma's command 0x70 as a sequence: 4, wait +0x114 == 7, 0x10, 0x60.
        The command register offset is a parameter: it is not published."""
        reg = SM1 + register
        return self.run(f"poke 4 {reg:#x} 4; wait {SM1 + 0x114:#x} 7 7 {polls_ms}; "
                        f"poke 4 {reg:#x} 0x10; poke 4 {reg:#x} 0x60")

    def sm1_wait_irq(self, ms):
        return self._fields(self.sm1(f"wait {ms}", timeout=ms / 1000 + 5))

    def sm1_slots(self):
        text = self.sm1("slots")
        f = self._fields(text)
        slots = [tuple(map(int, s)) for s in re.findall(r" (\d+):(\d+)/(\d+)", text)]
        return dict(out=int(f["out"], 16), ring=int(f["ring"]), units=int(f["units"]),
                    slots=[dict(index=i, filled=a, length=b) for i, a, b in slots])

    def sm1_read_pcm(self, slot, release=True):
        pcm = self.request(f"sm1 pcm {slot}")[1]
        if release:
            self.sm1(f"release {slot}")
        return pcm


def parse_x17i(data):
    """X17I v1 sections as (kind, pass, addr, words); P words are rows."""
    if data[:4] != b"X17I":
        raise ValueError("not an X17I image")
    version, count = struct.unpack_from("<HH", data, 4)
    if version != 1:
        raise ValueError(f"X17I version {version}")
    kinds = {1: "p", 2: "x", 3: "y", 4: "param", 5: "mode"}
    pos, sections = 8, []
    for _ in range(count):
        kind, pass_, _zero, addr, n = struct.unpack_from("<BBHII", data, pos)
        pos += 12
        if kind not in kinds:
            raise ValueError(f"X17I section kind {kind}")
        words = n * 4 if kind == 1 else 0 if kind == 5 else n
        values = list(struct.unpack_from(f"<{words}I", data, pos))
        if len(values) != words:
            raise ValueError("X17I truncated")
        pos += words * 4
        if kind == 1:
            values = [tuple(values[i:i + 4]) for i in range(0, words, 4)]
        sections.append((kinds[kind], pass_, addr, values))
    if pos != len(data):
        raise ValueError("X17I trailing bytes")
    return sections


# --- experiments -------------------------------------------------------------

def run_experiment(shell, spec, base=Path(".")):
    """Run a JSON experiment (see docs) and return its result dictionary.

    Steps: {"run": line, "expect": text|"re:<regex>"}, {"blob": file,
    "addr": "scratch+0x100"}, {"image": file, "passes": [0]}, {"mark": n},
    {"sleep": s}. "{scratch}" or "{scratch+0x40}" in a line is an address in
    the scratch buffer. A
    dropped link means the device rebooted: podsh reconnects, records it and
    collects crash records before it stops."""
    started = time.monotonic()
    result = dict(name=spec.get("name", "experiment"), steps=[], ok=False)
    scratch, size = shell.scratch()
    known = set(shell.crash_list())
    result["baseline"] = dict(info=shell.info(), crashes=sorted(known),
                              irq=shell.run("irq"), sm1=shell.sm1("status"))

    def address(value):
        text = str(value).replace("scratch", f"{scratch:#x}")
        return sum(int(part, 0) for part in text.split("+"))

    for index, step in enumerate(spec["steps"]):
        entry = dict(index=index, step=step)
        t0 = time.monotonic()
        try:
            if "run" in step:
                line = re.sub(r"\{scratch(?:\+(\w+))?\}",
                              lambda m: f"{scratch + int(m.group(1) or '0', 0):#x}", step["run"])
                text, blob = shell.request(line, timeout=step.get("timeout"))
                entry["text"] = text
                if blob:
                    entry["data_hex"] = blob[:256].hex()
                    entry["data_len"] = len(blob)
                expect = step.get("expect")
                if expect is not None:
                    good = (re.search(expect[3:], text) if expect.startswith("re:")
                            else text.strip() == expect)
                    if not good:
                        raise AssertionError(f"expected {expect!r}, got {text!r}")
                if "save" in step:
                    (base / step["save"]).write_bytes(blob)
            elif "blob" in step:
                shell.load(address(step["addr"]), (base / step["blob"]).read_bytes())
            elif "image" in step:
                entry["mode"] = shell.sm1_load_image((base / step["image"]).read_bytes(),
                                                     step.get("passes"))
            elif "mark" in step:
                shell.run(f"mark {int(step['mark'])}")
            elif "sleep" in step:
                time.sleep(float(step["sleep"]))
            entry["ok"] = True
        except (ShellError, AssertionError) as error:
            entry.update(ok=False, error=str(error))
        except RECONNECT_ERRORS as error:
            entry.update(ok=False, error=f"link lost: {error}")
            try:
                shell.connect()
                entry["reconnected"] = True
            except TimeoutError as again:
                entry["reconnect_error"] = str(again)
        entry["seconds"] = round(time.monotonic() - t0, 4)
        result["steps"].append(entry)
        if not entry["ok"]:
            break
    else:
        result["ok"] = True
    try:
        crashes = sorted(set(shell.crash_list()) - known)
        result["new_crashes"] = {n: shell.crash_get(n).decode("utf-8", "replace") for n in crashes}
        result["log_tail"] = shell.log_tail(32).decode("utf-8", "replace")
        result["after"] = dict(irq=shell.run("irq"), sm1=shell.sm1("status"))
    except RECONNECT_ERRORS + (ShellError,) as error:
        result["collect_error"] = str(error)
    if result.get("new_crashes"):
        result["ok"] = False
    result["reconnects"] = shell.reconnects
    result["seconds"] = round(time.monotonic() - started, 3)
    return result


# --- file side channel (mounted volume) and symbols -------------------------

class ResearchFiles:
    """Read diagnostics using the mounted firmware volume as a side channel."""

    def __init__(self, volume):
        volume = Path(volume).resolve(strict=True)
        self.directory = volume / ".rockbox" / "research"
        if self.directory.resolve(strict=True) != self.directory:
            raise ValueError("research directory must be on the supplied volume")

    def _crash_path(self, slot):
        if slot not in CRASH_SLOTS:
            raise ValueError("crash slot must be in 0..15")
        return self.directory / f"crash-{slot}.txt"

    @staticmethod
    def _read(path):
        if path.is_symlink() or not path.is_file():
            raise ValueError(f"expected a regular research file: {path}")
        with path.open("rb") as stream:
            data = stream.read(MAX_RECORD_BYTES + 1)
        if len(data) > MAX_RECORD_BYTES:
            raise ValueError(f"research record too large: {path.name}")
        return data

    def crash_list(self):
        return [slot for slot in CRASH_SLOTS if self._crash_path(slot).is_file()
                and not self._crash_path(slot).is_symlink()]

    def crash_get(self, slot):
        return self._read(self._crash_path(slot))

    def crash_clear(self, slot):
        self._crash_path(slot).unlink(missing_ok=True)

    def log_tail(self, lines):
        if not 1 <= lines <= 64:
            raise ValueError("line count must be in 1..64")
        return b"".join(self._read(self.directory / "log.txt").splitlines(keepends=True)[-lines:])


def arm_symbols(elf, nm):
    output = subprocess.check_output([nm, "-n", "-S", "--defined-only", str(elf)],
                                     text=True, timeout=10)
    symbols = []
    for line in output.splitlines():
        fields = line.split(maxsplit=3)
        if len(fields) == 4 and fields[2] in "TtWw":
            start, size = [int(value, 16) for value in fields[:2]]
            symbols.append((start & ~1, size, fields[3]))
    symbols.sort()
    return symbols


def symbolized_frames(text, symbols):
    starts = [entry[0] for entry in symbols]
    frames = []
    for field, value in re.findall(r"(?:^| )(pc|lr|candidate)=([0-9a-f]{8})", text, re.M):
        address = int(value, 16) & ~1
        index = bisect_right(starts, address) - 1
        frame = dict(field=field, address=f"0x{address:08x}")
        if index >= 0:
            start, size, name = symbols[index]
            if address < start + size:
                frame.update(symbol=name, offset=address - start)
        frames.append(frame)
    return frames


def parse_crash(slot, text, symbols=None):
    match = re.match(r"^FU88 1 (\S+) boot=(\d+) kind=(\d+) ticks=(\d+) hz=(\d+)\n", text)
    if not match:
        raise ValueError(f"invalid crash header in slot {slot}")
    firmware, boot, kind, ticks, hz = match.groups()
    record = dict(slot=slot, firmware=firmware, boot=int(boot), kind=int(kind),
                  ticks=int(ticks), hz=int(hz), text=text)
    if symbols is not None:
        record["frames"] = symbolized_frames(text, symbols)
    return record


def collect(files, symbols=None):
    records = [parse_crash(slot, files.crash_get(slot).decode("utf-8", errors="replace"), symbols)
               for slot in files.crash_list()]
    return dict(crashes=sorted(records, key=lambda record: record["boot"]),
                log=files.log_tail(64).decode("utf-8", errors="replace")
                if (files.directory / "log.txt").is_file() else "")


# Kept for callers of the FU88 first cut
def ping_link(link, timeout):
    nonce = secrets.token_hex(8).encode("ascii")
    link.send(podlink.TEXT, b"RS\t1\tPING\t" + nonce)
    expected = b"RS\t1\tPONG\t" + nonce
    deadline = time.monotonic() + timeout
    while (remaining := deadline - time.monotonic()) > 0:
        frame = link.receive(remaining)
        if frame.type == podlink.TEXT and frame.payload == expected:
            return
    raise TimeoutError("research PONG deadline expired")


@contextmanager
def direct_link(port, timeout):
    with podlink.serial_port(port, timeout) as serial:
        link = podlink.Link(serial, timeout)
        link.open()
        try:
            yield link
        finally:
            try:
                link.close()
            except (OSError, RuntimeError, TimeoutError):
                pass


def reconnect_ping(port, timeout, open_link=direct_link):
    """Find PodLink again after a reboot within one monotonic deadline."""
    if not 0 < timeout <= 120:
        raise ValueError("timeout must be in (0, 120] seconds")
    deadline = time.monotonic() + timeout
    cause = None
    while (remaining := deadline - time.monotonic()) > 0:
        try:
            with open_link(port, min(remaining, 1.0)) as link:
                ping_link(link, min(deadline - time.monotonic(), 1.0))
            return
        except (OSError, RuntimeError, TimeoutError) as error:
            cause = error
            time.sleep(max(0, min(0.1, deadline - time.monotonic())))
    raise TimeoutError("PodLink did not respond before the deadline") from cause


# --- CLI --------------------------------------------------------------------

def number(text):
    return int(text, 0)


def main(argv=None, open_transport=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", help="serial tty (default: find the iPod)")
    parser.add_argument("--ws", help="WebSocket bridge URL, e.g. ws://127.0.0.1:8765/")
    parser.add_argument("--timeout", type=float, default=10, help="per request, seconds")
    parser.add_argument("--reconnect", type=float, default=60, help="reconnect deadline, seconds")
    parser.add_argument("--volume", type=Path, help="mounted volume: file commands read it directly")
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("ping")
    raw = commands.add_parser("run", help="send one command line (batches with ';')")
    raw.add_argument("line")
    raw.add_argument("--data", type=Path, help="payload file")
    raw.add_argument("--out", type=Path, help="write reply bytes here")
    for name in ("peek", "poke", "dump", "load", "fill", "memcpy", "wait", "call", "perf",
                 "regs", "irq", "clock", "boost", "mark", "sm1", "reboot", "watchdog"):
        sub = commands.add_parser(name, help=f"firmware '{name}' (arguments as in the docs)")
        sub.add_argument("args", nargs="*")
        if name in ("dump",):
            sub.add_argument("--out", type=Path, required=True)
        if name in ("load",):
            sub.add_argument("--data", type=Path, required=True)
    crash = commands.add_parser("crash").add_subparsers(dest="action", required=True)
    crash.add_parser("list")
    for action in ("get", "clear"):
        crash.add_parser(action).add_argument("slot", type=int, choices=CRASH_SLOTS)
    log = commands.add_parser("log").add_subparsers(dest="action", required=True)
    log.add_parser("tail").add_argument("--lines", type=int, default=16)
    files = commands.add_parser("file").add_subparsers(dest="action", required=True)
    files.add_parser("ls")
    for action in ("get", "put", "rm"):
        sub = files.add_parser(action)
        sub.add_argument("name")
        if action != "rm":
            sub.add_argument("path", type=Path)
    collector = commands.add_parser("collect", help="crash records + log as JSON")
    collector.add_argument("output", type=Path)
    collector.add_argument("--elf", type=Path, help="matching unstripped ARM ELF")
    collector.add_argument("--nm", default="arm-elf-eabi-nm")
    experiment = commands.add_parser("run-experiment", help="run a JSON experiment")
    experiment.add_argument("spec", type=Path)
    experiment.add_argument("--result", type=Path, required=True, help="JSON result path")
    image = commands.add_parser("sm1-image", help="load an X17I image's passes")
    image.add_argument("image", type=Path)
    image.add_argument("--pass", dest="passes", type=lambda s: int(s, 0), action="append")
    args = parser.parse_args(argv)

    try:
        if args.volume is not None and args.command in ("crash", "log", "collect"):
            store = ResearchFiles(args.volume)
            if args.command == "collect":
                symbols = arm_symbols(args.elf, args.nm) if args.elf else None
                args.output.write_text(json.dumps(collect(store, symbols), indent=2) + "\n")
            elif args.command == "log":
                sys.stdout.buffer.write(store.log_tail(args.lines))
            elif args.action == "list":
                print(json.dumps(store.crash_list()))
            elif args.action == "get":
                sys.stdout.buffer.write(store.crash_get(args.slot))
            else:
                store.crash_clear(args.slot)
            return
        shell = Shell(open_transport or transport_factory(args.port, args.ws, args.timeout),
                      timeout=args.timeout, reconnect=args.reconnect)
        with shell:
            if args.command == "ping":
                print("PONG")
            elif args.command == "run":
                text, blob = shell.request(args.line, args.data.read_bytes() if args.data else None)
                print(text)
                if args.out:
                    args.out.write_bytes(blob)
            elif args.command == "dump":
                text, blob = shell.request("dump " + " ".join(args.args))
                args.out.write_bytes(blob)
                print(text)
            elif args.command == "load":
                print(shell.run("load " + " ".join(args.args), args.data.read_bytes()))
            elif args.command == "reboot":
                shell.reboot()
                print("rebooted, shell answers again")
            elif args.command == "crash":
                if args.action == "list":
                    print(json.dumps(shell.crash_list()))
                elif args.action == "get":
                    sys.stdout.buffer.write(shell.crash_get(args.slot))
                else:
                    shell.crash_clear(args.slot)
            elif args.command == "log":
                sys.stdout.buffer.write(shell.log_tail(args.lines))
            elif args.command == "file":
                if args.action == "ls":
                    print(json.dumps(shell.file_ls()))
                elif args.action == "get":
                    args.path.write_bytes(shell.file_get(args.name))
                elif args.action == "put":
                    shell.file_put(args.name, args.path.read_bytes())
                else:
                    shell.file_rm(args.name)
            elif args.command == "collect":
                symbols = arm_symbols(args.elf, args.nm) if args.elf else None
                records = [parse_crash(n, shell.crash_get(n).decode("utf-8", "replace"), symbols)
                           for n in shell.crash_list()]
                args.output.write_text(json.dumps(dict(
                    crashes=sorted(records, key=lambda r: r["boot"]),
                    log=shell.log_tail(64).decode("utf-8", "replace")), indent=2) + "\n")
            elif args.command == "run-experiment":
                result = run_experiment(shell, json.loads(args.spec.read_text()), args.spec.parent)
                args.result.write_text(json.dumps(result, indent=2) + "\n")
                print("PASS" if result["ok"] else "FAIL", args.result)
                if not result["ok"]:
                    sys.exit(2)
            elif args.command == "sm1-image":
                print("mode", shell.sm1_load_image(args.image.read_bytes(), args.passes))
            else:
                print(shell.run(" ".join([args.command] + args.args)))
    except ShellError as error:
        parser.exit(3, f"podsh: device: {error}\n")
    except (ImportError, OSError, RuntimeError, ValueError, subprocess.SubprocessError) as error:
        parser.exit(1, f"podsh: {error}\n")


if __name__ == "__main__":
    main()
