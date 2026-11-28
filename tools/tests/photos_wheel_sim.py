#!/usr/bin/env python3
"""The Photos grid's wheel steps, against stock's rule.

Drives a built ipod6g simulator headless (SDL dummy video):

    python3 tools/tests/photos_wheel_sim.py build-sim-ipod6g EVIDENCE_DIR

Stock moves the grid one cell for every five of the wheel's 96 positions,
at any speed. A move past the bottom row goes to the last cell of the row
below, so a continued turn steps a row at a time; a move past the top row
goes up a row and right by the selection's column. The simulator's wheel
key counts one four-position detent, so a key press is 0.8 of a cell.

Turns down through 60 photos and back up, one key at a time, and checks the
framed cell after every key against that rule.
"""
import argparse
from pathlib import Path
import shutil
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from photos_loose_sim import (GRID_TOP, open_photos, picture,  # noqa: E402
                              session)
from videos_sim import player  # noqa: E402

COLS, ROWS = 5, 3
PHOTOS = 60
FRAME = (255, 194, 25)    # the frame's outer colour as the LCD shows it


def framed_cell(image):
    """The (row, column) on screen of the selection frame, or None: the
    first line with 40 frame-coloured pixels in a row is its top edge"""
    pixels = image.load()
    for y in range(GRID_TOP, 240):
        run = 0
        for x in range(320):
            run = run + 1 if pixels[x, y] == FRAME else 0
            if run == 40:
                return (y - GRID_TOP + 2) // 65, (x - 39 + 2) // 64
    return None


class Model:
    """Stock 2.0.5's grid wheel: 0x81b0e4c with no acceleration, then
    0x8160fdc's wheel case"""
    def __init__(self, total):
        self.total, self.sel, self.top, self.rem = total, 0, 0, 0

    def turn(self, positions):
        acc = self.rem + positions
        steps = int(acc / 5)
        self.rem = acc - steps * 5
        if not steps:
            return
        cur = self.sel
        if cur + steps < self.top:
            steps = cur % COLS - COLS
        if cur + steps - ROWS * COLS >= self.top:
            steps = COLS - cur % COLS + COLS - 1
        new = min(self.total - 1, max(0, cur + steps))
        if new >= self.top + COLS * ROWS:
            self.top = new // COLS * COLS - (ROWS - 1) * COLS
        elif new < self.top:
            self.top = new // COLS * COLS
        self.sel = new

    def cell(self):
        return divmod(self.sel - self.top, COLS)


def run(build, evidence):
    case = evidence / "wheel"
    if case.exists():
        shutil.rmtree(case)
    case.mkdir(parents=True)
    disk = case / "disk"
    disk.mkdir()
    player(build, disk, "classic")
    with (disk / ".rockbox/config.cfg").open("a") as cfg:
        cfg.write("root menu order: images,files,settings\n")
    for i in range(PHOTOS):
        picture(disk / "Pictures" / f"{i + 1:03d}.jpg", (320, 240), i + 1)

    model, log, failures = Model(PHOTOS), [], []
    with session(build, case, disk, "classic") as sim:
        open_photos(sim, "classic")
        sim.tap("select")
        sim.settle(quiet=1.5, timeout=120)
        opened = sim.save("grid")
        assert framed_cell(opened) == (0, 0), \
            f"grid opened framing {framed_cell(opened)}"
        keys = ["down"] * 70 + ["up"] * 70
        for n, key in enumerate(keys):
            sim.tap(key)
            sim.settle(quiet=0.4)
            model.turn(4 if key == "down" else -4)
            image = sim.image()
            seen, want = framed_cell(image), model.cell()
            log.append(f"{n:3d} {key:4s} seen {seen} want {want} "
                       f"sel {model.sel} top {model.top}")
            if seen != want:
                image.save(case / f"mismatch-{n:03d}.png")
                failures.append(log[-1])
    (case / "steps.txt").write_text("\n".join(log) + "\n")
    shutil.rmtree(disk, ignore_errors=True)
    return log, failures


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("build", type=Path)
    parser.add_argument("evidence", type=Path)
    args = parser.parse_args()
    log, failures = run(args.build.resolve(), args.evidence.resolve())
    print("\n".join(log))
    if failures:
        sys.exit("FAIL: %d steps differ:\n%s" % (len(failures),
                                                 "\n".join(failures[:10])))
    print("PASS")


if __name__ == "__main__":
    main()
