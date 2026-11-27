#!/usr/bin/env python3
"""Exercise Fruit Company's Now Playing controls with SDL's dummy drivers.

Usage: fruit_now_playing_sim.py ROCKBOXUI ROCKBOX.ZIP OUTPUT_DIRECTORY
"""
import argparse
from pathlib import Path
import sys

from PIL import Image, ImageChops

sys.dont_write_bytecode = True
from ui_stock_transitions_sim import Sim, make_fixture


ROW = (0, 198, 320, 234)
TRANSPORT = (269, 1, 285, 19)
TITLE = (159, 66, 320, 90)
SHUFFLE_ICON = (292, 25, 310, 39)
SEGMENTS = ((74, 204, 115, 228), (126, 204, 184, 228),
            (196, 204, 267, 228))


def frame(sim):
    _, images = sim.frames_since(0)
    assert images, "the simulator did not draw"
    return Image.frombytes("RGB", (320, 240), images[-1])


def same(a, b, box):
    return not ImageChops.difference(a.crop(box), b.crop(box)).getbbox()


def blue(image, box):
    return sum(b > 140 and b > r + 50 and b > g + 25
               for r, g, b in image.crop(box).getdata())


def stars(image):
    return sum(blue(image, (x, 199, x + 22, 224)) >= 16
               for x in range(105, 215, 22))


def active_shuffle(image):
    for i, segment in enumerate(SEGMENTS):
        if blue(image, segment) > 12:
            return i
    return None


def scrub(image):
    return blue(image, (54, 207, 90, 221)) > 8


def timeline(image):
    return max(image.getpixel((260, 207))) < 230 and stars(image) == 0


def wps(image):
    return min(image.getpixel((318, 237))) > 220 and any(
        max(image.getpixel((x, y))) < 75
        for y in range(4, 15) for x in range(78, 86))


def capture(sim, name):
    image = frame(sim)
    image.save(sim.out / (name + ".png"))
    return image


def playable_fixture(binary, resource, root):
    make_fixture(binary, resource, root)
    config = root / ".rockbox/config.cfg"
    for name in ("config.cfg.new", "config.cfg.old"):
        config.with_name(name).unlink(missing_ok=True)
    # The first playlist always opens the same first track on restart.
    config.write_text(config.read_text() +
                      "\nnative theme: fruit-company\nstart in screen: files\n"
                      "start directory: /Playlists\nshow files: supported\n"
                      "idle poweroff: off\nscrolling enabled: off\n"
                      "repeat: all\nshuffle: off\n")


def play_playlist(sim):
    sim.wait(180)
    capture(sim, "file-list")
    sim.tap("select", 32)
    if not wps(frame(sim)):
        sim.tap("select", 58)
    playing = capture(sim, "playing")
    assert wps(playing), "the fixture playlist did not open Now Playing"
    sim.tap("play", 24)
    paused = capture(sim, "paused")
    assert wps(paused) and not same(paused, playing, TRANSPORT)
    return paused


def choose(sim, key, count=1):
    for _ in range(count):
        sim.tap(key, 20 if key == "select" else 5)


def exercise(sim):
    paused = play_playlist(sim)
    sim.tap("down", 24)
    volume = capture(sim, "volume")
    assert wps(volume) and not same(volume, paused, ROW)
    sim.tap("select", 24)
    assert timeline(frame(sim)), "SELECT closes volume before scrubbing"

    sim.tap("select", 24)
    seek = capture(sim, "scrub")
    assert scrub(seek) and same(seek, paused, TRANSPORT)

    sim.tap("select", 24)
    dots = capture(sim, "rating-empty")
    assert stars(dots) == 0 and not same(dots, paused, ROW)
    choose(sim, "down", 3)
    rated = capture(sim, "rating-edited")
    assert stars(rated) == 3, "three detents must add three stars"
    sim.tap("select", 24)
    assert timeline(capture(sim, "rating-confirmed")), "SELECT accepts the edit"

    choose(sim, "select", 3)
    options = capture(sim, "shuffle-off")
    assert active_shuffle(options) == 0
    sim.tap("down", 10)
    assert active_shuffle(capture(sim, "shuffle-songs")) == 1
    sim.tap("down", 10)
    albums = capture(sim, "shuffle-albums")
    assert active_shuffle(albums) == 2
    assert same(options, albums, SHUFFLE_ICON), "wheel only previews shuffle"
    sim.tap("select", 24)
    applied = capture(sim, "shuffle-applied")
    assert not same(options, applied, SHUFFLE_ICON)
    assert same(options, applied, TITLE), "shuffle must keep the current track"

    choose(sim, "select", 3)
    assert active_shuffle(frame(sim)) == 2
    choose(sim, "up", 2)
    sim.tap("select", 24)
    assert same(frame(sim), options, SHUFFLE_ICON)

    sim.tap("select", 24)
    assert scrub(frame(sim))
    choose(sim, "down", 10)
    target = capture(sim, "scrub-target")
    assert blue(target, (67, 207, 110, 221)) > 14
    sim.tap("select", 24)
    seeked = capture(sim, "seeked")
    assert blue(seeked, (58, 207, 80, 221)) > 50, "seek did not land"
    assert same(seeked, paused, TRANSPORT), "seek resumed paused playback"

    # The timeout ends the rating panel while keeping the saved stars.
    choose(sim, "select", 2)
    assert stars(frame(sim)) == 3
    sim.wait(204)
    assert stars(frame(sim)) == 3, "rating timed out before four seconds"
    sim.wait(56)
    assert timeline(capture(sim, "rating-timed-out")), "rating did not time out"

    sim.send("down play")
    sim.wait(60)
    sim.send("up play")
    sim.wait(24)
    held = capture(sim, "play-held")
    assert wps(held) and same(held, seeked, TITLE), "PLAY opened a menu"

    sim.tap("right", 35)
    advanced = capture(sim, "next-track")
    assert wps(advanced) and not same(advanced, seeked, TITLE)
    sim.tap("back", 35)
    assert same(capture(sim, "previous-track"), seeked, TITLE)
    sim.tap("menu", 35)
    assert not wps(capture(sim, "menu")), "MENU did not leave Now Playing"


def run(binary, disk, out, script):
    out.mkdir()
    sim = Sim(binary, disk, out)
    try:
        script(sim)
    finally:
        sim.close()


def check_saved_rating(sim):
    play_playlist(sim)
    choose(sim, "select", 2)
    assert stars(capture(sim, "reloaded-rating")) == 3, "rating was not saved"


def set_config(disk, **settings):
    config = disk / ".rockbox/config.cfg"
    for name in ("config.cfg.new", "config.cfg.old"):
        config.with_name(name).unlink(missing_ok=True)
    values = {key.replace("_", " "): value for key, value in settings.items()}
    lines = [line for line in config.read_text().splitlines()
             if line.partition(":")[0] not in values]
    lines.extend(f"{key}: {value}" for key, value in values.items())
    config.write_text("\n".join(lines) + "\n")


def check_cover_flow(sim):
    sim.wait(180)
    sim.tap("select", 42)  # Music.
    sim.tap("select", 155)  # Cover Flow's opened library.
    covers = capture(sim, "covers")
    sim.tap("play", 100)
    assert wps(capture(sim, "cover-playing"))
    sim.tap("select", 24)
    assert scrub(capture(sim, "cover-scrub"))
    sim.tap("menu", 90)
    returned = capture(sim, "cover-return")
    # The cover caption may be moving, but the screen returns to its covers.
    assert not wps(returned) and same(covers, returned, (0, 25, 320, 165))


def check_classic(sim):
    play_playlist(sim)
    sim.tap("select", 42)
    assert not wps(capture(sim, "classic-select")), "Classic lost its Lyrics action"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("binary", "resource", "output"):
        parser.add_argument(name, type=lambda value: Path(value).resolve())
    args = parser.parse_args()
    args.output.mkdir(parents=True)
    disk = args.output / "fixture"
    playable_fixture(args.binary, args.resource, disk)
    run(args.binary, disk, args.output / "controls", exercise)
    run(args.binary, disk, args.output / "restart", check_saved_rating)
    set_config(disk, start_in_screen="root")
    run(args.binary, disk, args.output / "cover-flow", check_cover_flow)
    set_config(disk, native_theme="classic", start_in_screen="files")
    run(args.binary, disk, args.output / "classic", check_classic)
    print("PASS Fruit Now Playing controls, rating persistence, Cover Flow and Classic")


if __name__ == "__main__":
    main()
