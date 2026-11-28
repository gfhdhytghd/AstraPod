#!/usr/bin/env python3
"""Exercise the stock photo viewer and slideshow with SDL's dummy drivers."""
import argparse
from pathlib import Path
import time

from PIL import Image, ImageChops

from photos_loose_sim import open_photos, session
from videos_sim import player

COLORS = ((216, 30, 64), (20, 188, 84), (44, 72, 228),
          (212, 169, 21), (167, 48, 188), (40, 188, 208))
TOP = (258, 0, 320, 24)
BOTTOM = (20, 196, 300, 240)


def until(sim, predicate, seconds=4):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        image = sim.image()
        if image is not None and predicate(image):
            return time.monotonic()
        time.sleep(.008)
    raise AssertionError("timed out waiting for photo frame")


def shows(image, index):
    return sum(abs(a - b) for a, b in zip(image.getpixel((160, 120)),
                                        COLORS[index])) < 15


def changed(before, after, box):
    return ImageChops.difference(before.crop(box), after.crop(box)).getbbox()


def held(sim, key, target):
    start = time.monotonic()
    sim.send(f"down {key}")
    try:
        observed = until(sim, lambda im: shows(im, target), 1.5)
        elapsed = observed - start
        assert .49 <= elapsed <= .79, (key, elapsed)
        time.sleep(.14)
    finally:
        sim.send(f"up {key}")
    time.sleep(.40)
    assert shows(sim.image(), target), f"{key} hold release moved again"
    return round(elapsed * 1000)


def enter(sim, theme):
    open_photos(sim, theme)
    sim.step("select", "all-photos", [f"{len(COLORS)} photos"])
    sim.quick("select", .09)
    until(sim, lambda im: shows(im, 0) and im.getpixel((160, 8)) == (0, 0, 0))


def fixture(build, case, theme, effect):
    case.mkdir(parents=True)
    disk = case / "disk"
    disk.mkdir()
    player(build, disk, theme)
    with (disk / ".rockbox/config.cfg").open("a") as cfg:
        cfg.write("root menu order: images,files,settings\n"
                  f"photo duration: 0\nphoto transition: {effect}\n"
                  "photo repeat: off\nphoto shuffle: off\n"
                  "photo music: off\n")
    pictures = disk / "Pictures"
    pictures.mkdir()
    for n, color in enumerate(COLORS):
        Image.new("RGB", (160, 90), color).save(pictures / f"{n}.png")

    return disk


def run(build, case, theme):
    disk = fixture(build, case, theme, "off")
    with session(build, case, disk, theme) as sim:
        enter(sim, theme)
        first = sim.image()
        assert first.getpixel((8, 8)) == (0, 0, 0)
        assert first.getpixel((8, 120)) != (0, 0, 0), "small photo not fitted"
        sim.quick("back", .08)
        time.sleep(.43)
        assert shows(sim.image(), 0), "wrapped past first photo"
        start = time.monotonic()
        sim.quick("right", .08)
        tapped = until(sim, lambda im: shows(im, 1))
        assert .19 <= tapped - start <= .50, tapped - start
        assert sim.image().getpixel((160, 8)) == (0, 0, 0), "viewer overlay"
        forward = held(sim, "right", len(COLORS) - 1)
        sim.quick("right", .08)
        time.sleep(.4)
        assert shows(sim.image(), len(COLORS) - 1), "wrapped past last photo"
        back = held(sim, "back", 0)
        print(f"Viewer tap {round((tapped-start)*1000)} ms, holds {forward}/{back} ms")
        # The simulator sends four wheel position units per keyboard tick.
        sim.quick("down", .08)
        sim.quick("down", .08)
        assert shows(sim.image(), 0)
        sim.quick("down", .08)
        until(sim, lambda im: shows(im, 1))
        for _ in range(3):
            sim.quick("up", .08)
        until(sim, lambda im: shows(im, 0))
        sim.quick("select", 2.5)
        time.sleep(.35)
        assert not changed(first, sim.image(), (0, 0, 320, 240)), "SELECT hold changed view"
        sim.step("menu", "grid-return", [f"{len(COLORS)} photos"])
        sim.quick("select", .09)
        until(sim, lambda im: shows(im, 0))
        sim.quick("menu", .85)
        sim.text("home-return", ["images"] if theme == "classic" else ["photos", "podcasts", "extras"],
                 absent=["all photos"])
        print("Viewer wheel, no wrap, no zoom, SELECT hold, MENU tap and hold")

        sim.step("select", "photos-home-selection", ["all photos", "browse files"])
        sim.step("select", "all-photos-from-home", [f"{len(COLORS)} photos"])
        sim.quick("select", .09)
        until(sim, lambda im: shows(im, 0))
        bare = sim.image()
        sim.quick("select", .08)
        until(sim, lambda im: changed(bare, im, BOTTOM))
        sim.save("caption")
        sim.quick("play", .08)
        until(sim, lambda im: changed(bare, im, TOP))
        sim.save("pause")
        sim.quick("play", .08)
        until(sim, lambda im: not changed(bare, im, TOP))
        sim.quick("right", .8)
        time.sleep(.25)
        assert shows(sim.image(), 0), "slideshow hold or release advanced"
        sim.quick("right", .08)
        until(sim, lambda im: shows(im, 1))
        sim.quick("back", .08)
        until(sim, lambda im: shows(im, 0))
        sim.quick("select", .08)
        time.sleep(.35)
        brightness = sim.save("brightness")
        sim.quick("up", .08)
        until(sim, lambda im: changed(brightness, im, BOTTOM))
        time.sleep(5.15)
        until(sim, lambda im: not changed(bare, im, BOTTOM))
        sim.quick("down", .08)
        until(sim, lambda im: changed(bare, im, BOTTOM))
        sim.save("volume")
        sim.quick("select", .08)
        until(sim, lambda im: not changed(bare, im, BOTTOM))
        sim.step("menu", "slideshow-return", ["all photos", "browse files"])
        sim.step("select", "all-photos-again", [f"{len(COLORS)} photos"])
        sim.quick("select", .09)
        until(sim, lambda im: shows(im, 0))
        sim.quick("play", .08)
        until(sim, lambda im: changed(bare, im, BOTTOM))
        print("Slideshow SELECT/PLAY start, pause/resume, arrows and overlays")
        sim.step("menu", "play-return", ["all photos", "browse files"])


def collect_transition(sim, target):
    start = sim.image()
    initial = start.crop((0, 120, 320, 121)).tobytes()
    sequence, _ = sim.frame()
    sim.quick("right", .08)
    first_changed = None
    frames = []
    until_time = time.monotonic() + 2
    while time.monotonic() < until_time:
        current, _ = sim.frame()
        if current != sequence:
            sequence = current
            image = sim.image()
            if image is None:
                continue
            colors = tuple(image.getpixel((x, 120)) for x in (10, 160, 310))
            if shows(image, target) and colors[0] == colors[1] == colors[2]:
                elapsed = 0 if first_changed is None else time.monotonic() - first_changed
                return elapsed, frames
            if image.crop((0, 120, 320, 121)).tobytes() != initial:
                if first_changed is None:
                    first_changed = time.monotonic()
                frames.append(colors)
        time.sleep(.004)
    raise AssertionError("transition never reached the destination")


def run_transition(build, case, theme, effect):
    disk = fixture(build, case, theme, effect)
    with session(build, case, disk, theme) as sim:
        enter(sim, theme)
        sim.quick("select", .08)
        time.sleep(.2)
        elapsed, frames = collect_transition(sim, 1)
        assert .36 <= elapsed <= .62, ("initial cross fade", effect, elapsed)
        assert frames and all(a == b == c for a, b, c in frames)
        elapsed, frames = collect_transition(sim, 2)
        if effect == "off":
            assert not frames, frames
        else:
            first_full = .29 if effect in ("zoom", "random") else .36
            assert first_full <= elapsed <= .62, (effect, elapsed)
            assert len(frames) >= 5, (effect, len(frames))
            if effect == "fade":
                assert all(a == b == c for a, b, c in frames)
            elif effect == "black":
                assert min(sum(b) for _, b, _ in frames) < 40
            elif effect == "zoom":
                assert any(a == c and sum(a) < sum(b) for a, b, c in frames)
            elif effect == "slide":
                assert any(a != c and b in (a, c) for a, b, c in frames)
            elif effect == "center":
                assert any(a == c and b != a for a, b, c in frames)
        print(f"PASS initial cross fade and {effect}: {round(elapsed * 1000)} ms "
              f"between changed and final frames", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path)
    parser.add_argument("case", type=Path)
    parser.add_argument("--theme", default="classic",
                        choices=("classic", "fruit-company"))
    parser.add_argument("--transition",
                        choices=("off", "fade", "black", "zoom", "slide",
                                 "center", "random"))
    args = parser.parse_args()
    if args.transition is None:
        run(args.build.resolve(), args.case.resolve(), args.theme)
    else:
        run_transition(args.build.resolve(), args.case.resolve(), args.theme,
                       args.transition)
    print("PASS")


if __name__ == "__main__":
    main()
