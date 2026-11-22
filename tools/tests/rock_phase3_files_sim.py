#!/usr/bin/env python3
"""Exercise Phase 3 file utilities on isolated SDL volumes."""
import argparse
from base64 import b32encode
from contextlib import contextmanager
import mmap
import os
from pathlib import Path
import signal
import struct
import subprocess
import time
from zipfile import ZipFile

from PIL import Image, ImageOps
from rock_disktidy_sim import FRAME_BYTES, HEIGHT, STATE_BYTES, WIDTH, screen_text


class Simulator:
    def __init__(self, proc, frames, evidence):
        self.proc = proc
        self.frames = frames
        self.evidence = evidence

    def tap(self, *keys):
        for key in keys:
            self.proc.stdin.write(f"down {key}\n".encode())
            self.proc.stdin.flush()
            time.sleep(.13)
            self.proc.stdin.write(f"up {key}\n".encode())
            self.proc.stdin.flush()
            time.sleep(.34)

    def capture(self, label, *terms, timeout=10):
        deadline = time.monotonic() + timeout
        words = ""
        while time.monotonic() < deadline:
            assert self.proc.poll() is None, self.proc.returncode
            sequence, = struct.unpack_from("=I", self.frames, 4)
            if sequence:
                offset = 12 + ((sequence - 1) % 8) * (FRAME_BYTES + 4)
                pixels = self.frames[offset + 4:offset + 4 + FRAME_BYTES]
                stable, = struct.unpack_from("=I", self.frames, offset)
                if stable == sequence:
                    frame = Image.frombytes("RGB", (WIDTH, HEIGHT), pixels)
                    words = screen_text(frame, self.evidence / "ocr.png")
                    if label in {"reading", "position-restored"}:
                        tight = frame.crop((85, 0, 145, 18)).resize((300, 90))
                        tight = ImageOps.expand(tight, border=12, fill="white")
                        image = self.evidence / "word-ocr.png"
                        tight.save(image)
                        words = subprocess.check_output(
                            ["tesseract", str(image), "stdout", "--psm", "7"],
                            text=True, stderr=subprocess.DEVNULL).lower().strip()
                    if all(term.replace(" ", "") in words.replace(" ", "")
                           for term in terms):
                        frame.save(self.evidence / f"{label}.png")
                        print(self.evidence.name, label, words, flush=True)
                        return frame
            time.sleep(.13)
        raise AssertionError((self.evidence, label, terms, words))


@contextmanager
def session(args, plugin, theme):
    case = args.evidence / theme / plugin
    disk = case / "disk"
    rockbox = disk / ".rockbox"
    module = rockbox / "rocks" / "apps" / f"{plugin}.rock"
    module.parent.mkdir(parents=True)
    (rockbox / "rocks" / "viewers").mkdir()
    with ZipFile(args.archive) as archive:
        for font in ("18-noto-sans.pbf", "18-noto-sans-bold.pbf", "24x24-icons.fnt"):
            archive.extract(f".rockbox/fonts/{font}", disk)
        if args.modules:
            module.write_bytes((args.modules / "apps" / module.name).read_bytes())
        else:
            archive.extract(str(module.relative_to(disk)), disk)

    (rockbox / "config.cfg").write_text(
        "start in screen: root\nroot menu order: plugins,files,settings\n"
        "animations: off\nbacklight timeout: off\nvoice menus: off\n"
        "font: /.rockbox/fonts/18-noto-sans.pbf\n"
        "font bold: /.rockbox/fonts/18-noto-sans-bold.pbf\n"
        f"native theme: {theme}\n"
    )

    state = case / "state.bin"
    with state.open("w+b") as handle, (case / "firmware.log").open("wb") as log:
        handle.truncate(STATE_BYTES)
        command = [
            "bwrap", "--die-with-parent", "--ro-bind", "/", "/",
            "--dev", "/dev", "--proc", "/proc", "--tmpfs", "/tmp",
            "--unshare-net", "--bind", str(case), str(case), "--chdir", str(disk),
            "--", str(args.simulator), "--root", ".", "--nobackground", "--zoom",
            "1", "--screenshot", "--keys", "-",
        ]
        env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy",
                   SDL_RENDER_DRIVER="software", XDG_RUNTIME_DIR="/tmp",
                   PODBOX_UI_WALK_STATE=str(state), TZ="UTC")
        with mmap.mmap(handle.fileno(), 0, access=mmap.ACCESS_READ) as frames, \
                subprocess.Popen(command, stdin=subprocess.PIPE, env=env,
                                 cwd=disk, stdout=log, stderr=subprocess.STDOUT,
                                 start_new_session=True) as proc:
            try:
                sim = Simulator(proc, frames, case)
                sim.capture("home", "plugins")
                sim.tap("select")
                sim.capture("categories", "applications")
                sim.tap("select")
                sim.capture("browser", "applications", plugin.replace("_", " "))
                yield sim, disk
            finally:
                if proc.poll() is None:
                    os.killpg(proc.pid, signal.SIGTERM)
                proc.wait(timeout=8)


def choose_document(sim, label, title):
    sim.capture(label, title, "fixture")
    sim.tap("down", "select")


def sort_case(sim, disk):
    document = disk / "fixture.txt"
    original = b"\xef\xbb\xbfZulu\r\nbeta\r\nAlpha"
    document.write_bytes(original)
    sim.tap("select")
    choose_document(sim, "picker-decline", "select text to sort")
    sim.capture("confirm-decline", "sort the lines")
    sim.tap("menu")
    sim.capture("declined", "applications", "sort")
    assert document.read_bytes() == original
    time.sleep(.6)

    sim.tap("select")
    choose_document(sim, "picker-confirm", "select text to sort")
    sim.capture("confirm", "sort the lines")
    sim.tap("select")
    sim.capture("returned", "applications", "sort")
    assert document.read_bytes() == b"\xef\xbb\xbfAlpha\r\nbeta\r\nZulu"
    assert not (disk / "fixture.txt.tmp").exists()


def editor_case(sim, disk):
    document = disk / "fixture.txt"
    original = b"\xef\xbb\xbf  First  \r\nSecond\r\n"
    document.write_bytes(original)
    sim.tap("select")
    sim.capture("menu", "text editor", "open text file", "new file")
    sim.tap("select")
    choose_document(sim, "picker", "choose text to edit")
    sim.capture("document", "first", "second")

    sim.tap("select")
    sim.capture("keyboard", "first")
    sim.tap("down", "select")
    sim.capture("changed", "second")
    sim.tap("menu")
    sim.capture("save-menu", "save and exit")
    sim.tap("down", "down", "down", "down", "select")
    sim.capture("returned", "applications")
    result = document.read_bytes()
    assert result != original, result
    assert result.startswith(b"\xef\xbb\xbf  First  ")
    assert result.endswith(b"\r\nSecond\r\n")
    assert not (disk / "fixture.txt.tmp").exists()


def speedread_case(sim, disk):
    document = disk / "fixture.txt"
    document.write_bytes(b"\xef\xbb\xbf" + b"Astra " * 160 + b"\n")
    bookmark = disk / ".rockbox" / "rocks" / "viewers" / "speedread.dat"
    sim.tap("select")
    choose_document(sim, "picker", "choose text to read")
    sim.capture("reading", "astra")
    sim.tap("down")
    sim.tap("select")
    sim.capture("reading-menu", "resume reading", "change font")
    sim.tap("down", "down", "down", "select")
    sim.capture("returned", "applications", "speedread")
    offset, word, wpm, name = bookmark.read_text().strip().split(" ", 3)
    assert int(offset) >= 3 and int(word) >= 0 and int(wpm) == 275
    assert name == "/fixture.txt"
    assert not bookmark.with_suffix(".dat.tmp").exists()
    time.sleep(.6)

    sim.tap("select")
    choose_document(sim, "picker-reopen", "choose text to read")
    sim.capture("position-restored", "astra")
    sim.tap("select")
    sim.capture("reading-menu-again", "resume reading")
    sim.tap("down", "down", "select")
    sim.capture("font-picker", "choose a font")
    sim.tap("select")
    sim.capture("reading-menu-font", "change font")
    font_path = bookmark.with_suffix(".cfg").read_text()
    assert font_path.startswith("/.rockbox/fonts/")
    assert (disk / font_path.lstrip("/")).is_file()
    sim.tap("down", "select")
    sim.capture("returned-again", "applications", "speedread")
    offset2, word2, wpm2, name2 = bookmark.read_text().strip().split(" ", 3)
    assert (offset2, wpm2, name2) == (offset, wpm, name)
    assert int(word2) >= int(word)


def otp_case(sim, disk):
    account_file = disk / ".rockbox" / "rocks" / "apps" / "otp.dat"
    account_file.write_bytes(b"OTP1" + struct.pack("<i", 0))
    secret = b32encode(b"12345678901234567890").decode()
    (disk / "fixture.txt").write_text(
        "otpauth://totp/Broken?period=0&secret=" + secret + "\n"
        "otpauth://hotp/Synthetic?counter=0&secret=" + secret + "\n"
    )
    sim.tap("select")
    sim.capture("menu", "one time password", "generate code", "add account")
    sim.tap("down", "select")
    sim.capture("import-menu", "from uri on disk")
    sim.tap("select")
    choose_document(sim, "picker", "choose otp account file")
    sim.capture("import-menu-again", "from uri on disk")
    assert len(account_file.read_bytes()) == 8 + 328
    sim.tap("menu", "up", "select")
    sim.capture("account", "generate code", "synthetic")
    sim.tap("select")
    sim.capture("code", "755224")
    sim.tap("menu")
    sim.capture("account-again", "generate code", "synthetic")
    sim.tap("menu")
    sim.capture("menu-again", "one time password", "generate code")
    sim.tap("menu")
    sim.capture("returned", "applications", "otp")
    data = account_file.read_bytes()
    assert data.startswith(b"OTP1") and len(data) == 8 + 328
    assert struct.unpack_from("<Q", data, 8 + 56)[0] == 1
    assert not account_file.with_suffix(".dat.tmp").exists()


CASES = {"otp": otp_case, "sort": sort_case, "speedread": speedread_case,
         "text_editor": editor_case}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("simulator", type=lambda value: Path(value).resolve())
    parser.add_argument("archive", type=lambda value: Path(value).resolve())
    parser.add_argument("evidence", type=lambda value: Path(value).resolve())
    parser.add_argument("--modules", type=lambda value: Path(value).resolve())
    parser.add_argument("--plugin", choices=CASES, action="append")
    parser.add_argument("--theme", choices=("light", "scrim"), action="append")
    args = parser.parse_args()
    for theme in args.theme or ("scrim", "light"):
        for plugin in args.plugin or CASES:
            with session(args, plugin, theme) as (sim, disk):
                CASES[plugin](sim, disk)


if __name__ == "__main__":
    main()
