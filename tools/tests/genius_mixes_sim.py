#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Capture Genius Mixes from a live, headless SDL simulator in each theme."""

import argparse
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import time

from PIL import Image, ImageChops, ImageDraw

from fruit_wheel_transport_sim import Playback
from power_about_stock_sim import Sim, prepare_disk
from ui_quickscreen_sim import ULTRA_FALLBACK_THEME


THEMES = ("fruit-company", "fruit-company-ultra", "classic", "scrim")
GROUPS = (
    ("Alternative Rock", "2007", ("Radiohead", "Muse", "Coldplay", "Metric")),
    ("Jazz", "1999", ("Esperanza", "Bill Evans", "Ahmad Jamal", "Nina Simone")),
)
CACHE = Path(".rockbox/genius-mixes.dat")
HEADER = struct.Struct("=IIiiii")
MIX = struct.Struct("=192si120i4i")


def wait_for(test, label, limit=75):
    deadline = time.monotonic() + limit
    while time.monotonic() < deadline:
        if result := test():
            return result
        time.sleep(.1)
    raise AssertionError(label)


def fixture(root, build):
    albums = root / "Music"
    for group, (genre, year, names) in enumerate(GROUPS):
        for artist, name in enumerate(names):
            for album in range(2):
                release = f"{name} Sessions {album + 1}"
                folder = albums / name / release
                folder.mkdir(parents=True)
                rgb = ((150, 51 + artist * 49, 28 + album * 50) if group == 0
                       else (26 + album * 44, 90 + artist * 40, 150))
                cover = Image.new("RGB", (200, 200), rgb)
                draw = ImageDraw.Draw(cover)
                draw.ellipse((34, 33, 166, 165), fill=(253, 238, 190))
                draw.text((68, 92), f"{artist + 1} · {album + 1}",
                          fill=(35, 35, 35))
                cover.save(folder / "folder.jpg")
                for track in range(2):
                    tags = {
                        "title": f"{release} – Track {track + 1}",
                        "artist": name, "album_artist": name, "album": release,
                        "genre": genre, "date": year, "track": str(track + 1),
                    }
                    metadata = [part for k, v in tags.items()
                                for part in ("-metadata", f"{k}={v}")]
                    hz = 210 + 40 * group + 22 * artist + 10 * album + track
                    subprocess.run([
                        "ffmpeg", "-nostdin", "-v", "error", "-f", "lavfi",
                        "-i", f"sine=f={hz}:duration=24", "-c:a", "libmp3lame",
                        "-q:a", "9", "-threads", "1", *metadata,
                        str(folder / f"{track + 1:02}.mp3"),
                    ], check=True)
    codecs = root / ".rockbox/codecs"
    codecs.mkdir(exist_ok=True)
    shutil.copyfile(build / "lib/rbcodec/codecs/mpa.codec", codecs / "mpa.codec")


def records(path):
    raw = path.read_bytes()
    version, _, entries, commit, deleted, count = HEADER.unpack_from(raw)
    assert version == 1 and len(raw) == HEADER.size + MIX.size * count
    mixes = []
    for i in range(count):
        name, n, *ids = MIX.unpack_from(raw, HEADER.size + MIX.size * i)
        name = name.split(b"\x00")[0].decode()
        assert 6 <= n <= 120
        assert len(set(ids[:n])) == n
        mixes.append((name, ids[:n], ids[-4:]))
    return entries, commit, deleted, mixes


def snap(sim, out, name):
    im = sim.frame()
    assert im is not None
    im.save(out / (name + ".png"))
    return im


def mosaic_ready(sim):
    im = sim.frame()
    if im is None:
        return False
    # A real tile has a saturated cover color. Placeholder tiles are gray.
    for x, y in ((95, 35), (165, 35), (95, 105), (165, 105)):
        rgb = im.getpixel((x, y))
        if max(rgb) - min(rgb) < 37:
            return False
    return True


def open_mixes(sim, theme, out):
    snap(sim, out, "home")
    sim.tap("select")
    snap(sim, out, "music")
    if theme.startswith("fruit"):
        sim.tap("down")
    else:
        for _ in range(30):
            sim.tap("down", hold_frames=2, settle_frames=4)
    snap(sim, out, "highlight")
    sim.tap("select")


def verify_theme(binary, root, theme, out):
    out.mkdir(parents=True, exist_ok=True)
    sim = Sim(binary, root, out)
    live = None
    try:
        cache = root / CACHE
        wait_for(lambda: cache.exists() and records(cache)[3],
                 f"{theme}: background mix generation")
        entries, _, _, mixes = records(cache)
        assert entries == 32 and len(mixes) == 2, mixes
        assert {len(m[1]) for m in mixes} == {16}, mixes
        assert all(" and " in m[0] for m in mixes)
        print(theme, [m[0] for m in mixes], flush=True)
        open_mixes(sim, theme, out)

        tiles = theme != "scrim"
        if tiles:
            wait_for(lambda: mosaic_ready(sim), f"{theme}: first four covers")
        before = snap(sim, out, "mixes-page-1" if tiles else "mixes-list")
        sim.tap("down")
        if tiles:
            wait_for(lambda: mosaic_ready(sim), f"{theme}: second four covers")
        after = snap(sim, out, "mixes-page-2" if tiles else "mixes-selection")
        assert ImageChops.difference(before, after).getbbox()
        sim.tap("select")

        live = Playback(binary, sim.proc.pid)
        wait_for(lambda: live.activity() == live.activities["ACTIVITY_WPS"]
                 and live.get("status") == 1,
                 f"{theme}: SELECT starts Now Playing", limit=8)
        snap(sim, out, "now-playing")
        if tiles:
            sim.tap("menu")
            wait_for(lambda: mosaic_ready(sim), f"{theme}: MENU returns to mix",
                     limit=8)
            returned = snap(sim, out, "returned-to-mix")
            assert ImageChops.difference(after.crop((0, 24, 320, 234)),
                                         returned.crop((0, 24, 320, 234))
                                         ).getbbox() is None
            index = live.get("track")
            sim.tap("right")
            wait_for(lambda: live.get("track") != index,
                     f"{theme}: RIGHT skips track", limit=5)
            sim.tap("back")
            wait_for(lambda: live.get("track") == index,
                     f"{theme}: LEFT skips back", limit=5)
            sim.tap("play")
            wait_for(lambda: live.get("status") == 3,
                     f"{theme}: PLAY pauses queue", limit=5)
            transport = snap(sim, out, "transport-kept-page")
            assert ImageChops.difference(after.crop((0, 24, 320, 234)),
                                         transport.crop((0, 24, 320, 234))
                                         ).getbbox() is None
            sim.tap("up")
            wait_for(lambda: mosaic_ready(sim), f"{theme}: wheel pages back")
            first = snap(sim, out, "wheel-returned-to-first")
            assert ImageChops.difference(before.crop((0, 24, 320, 234)),
                                         first.crop((0, 24, 320, 234))
                                         ).getbbox() is None
        print(f"PASS {theme}", flush=True)
    except Exception:
        if sim.frame() is not None:
            snap(sim, out, "failure")
        raise
    finally:
        if live:
            live.close()
        sim.close()



def verify_cache_lifecycle(binary, root, out):
    cache = root / CACHE
    config = root / ".rockbox/config.cfg"
    stamp = cache.stat().st_mtime_ns
    original = cache.read_bytes()
    case = out / "cached-on-restart"
    case.mkdir(parents=True, exist_ok=True)
    sim = Sim(binary, root, case)
    try:
        sim.wait(60 * 13)
        open_mixes(sim, "fruit-company", case)
        wait_for(lambda: mosaic_ready(sim), "cached mix available after restart")
        assert cache.stat().st_mtime_ns == stamp and cache.read_bytes() == original
        snap(sim, case, "mixes-page-1")
    finally:
        sim.close()
    print("PASS cache reuse on restart", flush=True)

    with config.open("a") as stream:
        stream.write("\ntagcache_scan_on_startup: on\n")
    for names, expected in ((GROUPS[1][2], 1), (GROUPS[0][2][1:], 0)):
        for name in names:
            shutil.rmtree(root / "Music" / name)
        case = out / ("deletion-update" if expected else "small-library")
        case.mkdir(parents=True, exist_ok=True)
        sim = Sim(binary, root, case)
        try:
            def updated():
                try:
                    return cache.read_bytes() != original and (
                        len(records(cache)[3]) == expected)
                except FileNotFoundError:
                    return False

            wait_for(updated, f"catalog did not regenerate to {expected} mixes")
            original = cache.read_bytes()
            open_mixes(sim, "fruit-company", case)
            if expected:
                wait_for(lambda: mosaic_ready(sim), "remaining four covers")
            snap(sim, case, "result")
        finally:
            sim.close()
        print(f"PASS deletion-only update: {expected} mixes", flush=True)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("simulator", type=lambda path: Path(path).resolve())
    parser.add_argument("evidence", type=lambda path: Path(path).resolve())
    parser.add_argument("--theme", choices=THEMES, action="append")
    parser.add_argument("--lifecycle", action="store_true")
    args = parser.parse_args()
    build = args.simulator.parent
    args.evidence.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="genius-mixes-") as folder:
        work = Path(folder)
        seed = work / "seed"
        seed.mkdir()
        (seed / ".rockbox").mkdir()
        fixture(seed, build)
        for theme in args.theme or THEMES:
            root = work / theme
            prepare_disk(build / "simdisk", root, theme, extra_cfg=[
                "start in screen: root", "root menu order: database,files",
                "backlight timeout: off", "voice menus: off",
                "list wraparound: off", "debug log tasks: on",
                "database scan paths: /Music",
                "animations: on",
                "show background tasks: off", "show disk activity: off",
            ])
            shutil.copytree(seed / "Music", root / "Music")
            shutil.copytree(seed / ".rockbox/codecs", root / ".rockbox/codecs",
                            dirs_exist_ok=True)
            themes = root / ".rockbox/native-themes"
            if theme == "fruit-company-ultra":
                themes.mkdir(exist_ok=True)
                fallback = themes / "fruit-company-ultra.podtheme"
                if not fallback.exists():
                    fallback.write_text(ULTRA_FALLBACK_THEME)
            verify_theme(args.simulator, root, theme, args.evidence / theme)
            if args.lifecycle and theme == "fruit-company":
                verify_cache_lifecycle(args.simulator, root, args.evidence)


if __name__ == "__main__":
    main()
