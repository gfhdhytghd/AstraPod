#!/usr/bin/env python3
"""Check stock one-row detents and hard list edges in headless Fruit Company."""

import argparse
import json
import sys
from pathlib import Path

sys.dont_write_bytecode = True
from ui_stock_transitions_sim import make_fixture, session


def stripe_top(image):
    for y in range(21, 240):
        r, _, b = image.getpixel((2, y))
        if b - r > 70:
            return y
    return -1


def check_list(sim, width):
    def poses(frames, edge):
        # The selected label can marquee at rest. Keep the marker itself
        # and the rest of the rows in the edge check.
        top = stripe_top(frames[0][1]) if edge else 240
        return {image.crop((0, 21, width, max(21, top))).tobytes() +
                image.crop((0, 21, 4, 240)).tobytes()
                for _, image in frames}

    shots = {}
    shots["first-edge"] = sim.record(30, "up")
    shots["one-detent"] = sim.record(30, "down")
    for _ in range(40):
        sim.tap("down", 9)
    sim.wait(24)
    shots["last-edge"] = sim.record(30, "down")
    change = shots["one-detent"]
    result = {name: len(poses(frames, name.endswith("edge")))
              for name, frames in shots.items()}
    result["row_delta"] = stripe_top(change[-1][1]) - stripe_top(change[0][1])
    result["ok"] = (result["first-edge"] == result["last-edge"] == 1 and
                    result["one-detent"] == 2 and result["row_delta"] == 24)
    return shots, result


def pages(sim, results):
    # Settings is split. Its EQ preset picker uses a full-width list.
    for _ in range(5):
        sim.tap("down", 20)
    sim.tap("select", 45)
    split, results["split"] = check_list(sim, 150)
    for _ in range(40):
        sim.tap("up", 9)
    for _ in range(9):
        sim.tap("down", 20)
    sim.tap("select", 55)
    full, results["full"] = check_list(sim, 295)
    return {f"{page}-{name}": frames
            for page, checks in (("split", split), ("full", full))
            for name, frames in checks.items()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for arg in ("binary", "resource", "output"):
        parser.add_argument(arg, type=lambda path: Path(path).resolve())
    parser.add_argument("--fixture", type=lambda path: Path(path).resolve())
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    fixture = args.fixture or args.output / "fixture"
    if not args.fixture:
        make_fixture(args.binary, args.resource, fixture)
    result = {}
    session(args.binary, fixture, args.output / "fruit-company",
            "fruit-company", "list wraparound: on\n",
            lambda sim: pages(sim, result))
    (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result))
    if not all(check["ok"] for check in result.values()):
        sys.exit(1)


if __name__ == "__main__":
    main()
