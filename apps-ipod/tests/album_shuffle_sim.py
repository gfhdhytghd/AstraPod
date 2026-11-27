#!/usr/bin/env python3
"""Exercise album shuffle with SDL dummy video and the actual playlist engine.

Run with the built simulator and its bundled zip. Requires ffmpeg, Pillow
and Tesseract. The output directory keeps the test disk, frames and logs.
"""

import argparse
from contextlib import contextmanager
from io import BytesIO
from pathlib import Path
import re
import subprocess
import sys
import time
from zipfile import ZipFile

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/tests"))
from ui_stock_transitions_sim import Sim  # noqa: E402

TRACKS = (
    ("Quartz", "Mix/Disc 2/one", "Collection", "Various Artists", "Singer C", 2, 1),
    ("Velvet", "Mix/Disc 1/two", "Collection", "Various Artists", "Singer B", 1, 2),
    ("Pearl", "Solo/two", "Collection", "Solo Artist", "Solo Artist", 1, 2),
    ("Marble", "Loose/10", "", "", "", 0, 0),
    ("Amber", "Mix/Disc 1/one", "Collection", "Various Artists", "Singer A", 1, 1),
    ("Jade", "Solo/one", "Collection", "Solo Artist", "Solo Artist", 1, 1),
    ("Copper", "Loose/2", "", "", "", 0, 0),
    ("Silver", "Duo/second", "Selections", "", "Guest A", 0, 1),
    ("Ivory", "Duo/first", "Selections", "", "Guest B", 0, 2),
)
TITLES = [track[0] for track in TRACKS]


def fixture(root, resources):
    with ZipFile(resources) as archive:
        archive.extractall(root)
    rockbox = root / ".rockbox"
    config = rockbox / "config.cfg"
    config.write_text(
        (rockbox / "default-config.cfg").read_text() +
        "\nnative theme: fruit-company\nstart in screen: files\n"
        "backlight timeout: off\nvoice menus: off\nanimations: off\n"
        "repeat: all\nshuffle: off\n"
    )

    paths = []
    for title, name, album, artist, performer, disc, track in TRACKS:
        path = rockbox / "test-audio" / (name + ".mp3")
        path.parent.mkdir(parents=True, exist_ok=True)
        tags = dict(title=title, album=album, album_artist=artist,
                    artist=performer, disc=disc or "", track=track or "")
        cmd = ["ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error",
               "-f", "lavfi", "-i", "sine=duration=80", "-c:a", "libmp3lame"]
        for key, value in tags.items():
            if value:
                cmd.extend(("-metadata", f"{key}={value}"))
        subprocess.run([*cmd, str(path)], check=True)
        paths.append("/" + path.relative_to(root).as_posix())
    (root / "Tracks.m3u8").write_text("\n".join(paths) + "\n")
    return rockbox


@contextmanager
def session(binary, root, out):
    out.mkdir()
    sim = Sim(binary, root, out)
    try:
        sim.wait(250)
        yield sim
    finally:
        sim.close()


def frame(sim, label):
    _, frames = sim.frames_since(0)
    image = Image.frombytes("RGB", (320, 240), frames[-1])
    image.save(sim.out / f"{label}.png")
    return image


def ocr(image, mode):
    stream = BytesIO()
    image.resize((image.width * 3, image.height * 3)).save(stream, format="PNG")
    result = subprocess.run(["tesseract", "stdin", "stdout", "--psm", mode],
                            input=stream.getvalue(), capture_output=True,
                            check=True)
    return result.stdout.decode().split()


def title(sim, label):
    text = ocr(frame(sim, label).crop((157, 66, 314, 90)), "7")
    matches = [value for value in TITLES if value in text]
    assert len(matches) == 1, (label, text)
    return matches[0]


def setting(sim, label, mode):
    text = ocr(frame(sim, label), "11")
    assert "Shuffle" in text or mode == "Albums", (label, text)
    assert mode in text, (label, text)


def go_shuffle(sim, returned_from_files):
    sim.tap("menu", 60)
    if returned_from_files:
        sim.tap("menu", 60)
    for _ in range(5):
        sim.tap("down", 20)
    sim.tap("select", 55)


def return_playing(sim):
    sim.tap("menu", 40)
    sim.tap("down", 20)
    sim.tap("down", 20)
    sim.tap("select", 65)


def album_records(control):
    text = control.read_text()
    for record in re.finditer(r"^B:(\d+):(\d+)$", text, re.M):
        offset = int(record[2])
        rows = text[offset:record.start()].splitlines()
        assert rows and all(row.startswith("#") for row in rows)
        order = "".join(row[1:] for row in rows)
        yield [int(order[i:i + 4], 16) for i in range(0, len(order), 4)]


def grouped(order, first_album):
    expected = [first_album, ["Jade", "Pearl"], ["Copper", "Marble"],
                ["Silver", "Ivory"]]
    for album in expected:
        i = order.index(album[0])
        assert order[i:i + len(album)] == album, (order, album)


def wait_saved(sim, path, pattern):
    for _ in range(40):
        if path.exists():
            text = path.read_text()
            if re.search(pattern, text, re.M):
                return text
        sim.wait(60)
    raise AssertionError(f"{path.name} did not save {pattern}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("binary", "resources", "output"):
        parser.add_argument(name, type=lambda value: Path(value).resolve())
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    root = args.output / "disk"
    root.mkdir()
    rb = fixture(root, args.resources)
    control = rb / ".playlist_control"
    config = rb / "config.cfg.new"
    resume = rb / ".resume.cfg.new"

    with session(args.binary, root, args.output / "first") as sim:
        sim.tap("select", 85)
        assert title(sim, "original-first") == "Quartz"
        sim.tap("right", 65)
        assert title(sim, "before-shuffle") == "Velvet"

        go_shuffle(sim, True)
        setting(sim, "off", "Off")
        sim.tap("down", 25)
        for mode in ("Songs", "Albums", "Off", "Songs", "Albums"):
            sim.tap("select", 60)
            setting(sim, f"setting-{mode}-{time.monotonic_ns()}", mode)
        wait_saved(sim, config, r"^shuffle: albums$")
        return_playing(sim)

        order = [title(sim, "pin-current")]
        assert order == ["Velvet"], order
        for i in range(len(TRACKS) - 1):
            sim.tap("right", 55)
            order.append(title(sim, f"album-track-{i + 1}"))
        assert sorted(order) == sorted(TITLES), order
        grouped(order, ["Velvet", "Amber", "Quartz"])

        before = list(album_records(control))
        sim.tap("right", 85)
        records = list(album_records(control))
        assert records[:-1] == before and len(records) == len(before) + 1
        wrapped = [order[i] for i in records[-1]]
        grouped(wrapped, ["Amber", "Velvet", "Quartz"])
        assert title(sim, "repeat-wrap") == wrapped[0]
        sim.tap("right", 65)
        assert title(sim, "before-restart") == wrapped[1]
        wait_saved(sim, resume, r"^IDX: 1$")

    # Boot directly into the queue using the settings/status the player saved.
    with config.open("a") as stream:
        stream.write("\nstart in screen: wps\n")

    with session(args.binary, root, args.output / "restarted") as sim:
        assert title(sim, "resumed") == wrapped[1]
        assert list(album_records(control)) == records
        sim.tap("right", 65)
        current = title(sim, "resumed-next")
        assert current == wrapped[2]

        go_shuffle(sim, False)
        setting(sim, "albums-after-restart", "Albums")
        sim.tap("down", 25)
        sim.tap("select", 70)
        setting(sim, "disabled", "Off")
        assert control.read_text().endswith("U:0\n")
        return_playing(sim)
        assert title(sim, "unshuffle-current") == current
        sim.tap("right", 65)
        assert title(sim, "unshuffle-next") == TITLES[(TITLES.index(current) + 1)
                                                     % len(TITLES)]

    print("Fruit cycle, play-current, album order, Repeat All, resume and Off pass")


if __name__ == "__main__":
    main()
