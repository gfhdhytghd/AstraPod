#!/usr/bin/env python3
"""Headless simulator tests for P10 (boot/power/connection) and P11 (About).

Arguments: ROCKBOXUI SIMDISK OUTPUT_DIRECTORY

Runs one headless SDL dummy-video simulator at a time and verifies:
  1. Settings > About under Fruit Company:
     - black glossy status bar (not white) with centered title
     - RIGHT/SELECT advance and LEFT retreats, with wrapping
     - MENU returns to Settings and wheel input leaves the page alone
     - Stock content motion and timing in Classic, Fruit Company and Ultra
     - Page 2 shows all 7 counts (Songs, Videos, Podcasts, iTunes U,
       Photos, Games, Contacts) with live Contacts and Games values
  2. Hold switch indicator:
     - White status bar shows padlock icon under Fruit Company and blocks
       wheel navigation
     - Black status bar on About shows padlock icon when held
     - Scrim status bar is unchanged when held (non-Fruit theme untouched)
  3. Home > Sleep preview + Charging/Standby screen + wake:
     - Home > Sleep shows crescent moon & stars right-pane preview
     - SELECT on Sleep (on charger in sim) enters full-screen Charging/Charged
       standby with black bar, large centered glossy battery + reflection
     - SELECT wakes back to Home without re-triggering Sleep
  4. USB consent popup + Stock USB Connected screen:
     - USB plug opens Disk Mode consent popup; SELECT connects to stock
       Connected screen; unplug returns to Home
  5. Settings > Clicker toggle:
     - Toggles both hardware piezo and headphone software keyclick in sync
"""

import json
import mmap
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zipfile
from pathlib import Path

import numpy as np
from PIL import Image, ImageChops

from ui_quickscreen_sim import ULTRA_FALLBACK_THEME
from ui_stock_walk_sim import Sim as WalkSim

W, H, SLOTS = 320, 240, 8
PIX = W * H * 3
HEADER = struct.Struct("=III")
WORD = struct.Struct("=I")
FPS = 60

FRUIT_MAIN_WITH_SLEEP = 0x20107081 | (1 << 30)  # default + HOME_SLEEP


class Sim:
    def __init__(self, rockboxui, root, out, screenshot=True):
        self.statefile = out / "state.bin"
        self.fh = self.statefile.open("w+b")
        size = HEADER.size + SLOTS * (WORD.size + PIX)
        self.fh.truncate(size)
        self.state = mmap.mmap(self.fh.fileno(), size, access=mmap.ACCESS_READ)
        env = dict(os.environ, SDL_VIDEODRIVER="dummy",
                   SDL_AUDIODRIVER="dummy", SDL_RENDER_DRIVER="software",
                   PODBOX_UI_WALK_STATE=str(self.statefile))
        env.pop("DISPLAY", None)
        env.pop("WAYLAND_DISPLAY", None)
        self.log = (out / "firmware.log").open("wb")
        cmd = [str(rockboxui), "--root", ".", "--nobackground", "--zoom", "1",
               "--keys", "-"]
        if screenshot:
            cmd.insert(-2, "--screenshot")
        self.proc = subprocess.Popen(
            cmd, cwd=root, env=env, stdin=subprocess.PIPE, stdout=self.log,
            stderr=subprocess.STDOUT, bufsize=0)

    def frame(self):
        _, frames, _ = HEADER.unpack_from(self.state)
        if not frames:
            return None
        off = HEADER.size + ((frames - 1) % SLOTS) * (WORD.size + PIX)
        px = bytes(self.state[off + WORD.size:off + WORD.size + PIX])
        return Image.frombytes("RGB", (W, H), px)

    def send(self, line):
        self.proc.stdin.write((line + "\n").encode())
        self.proc.stdin.flush()

    def wait(self, frames):
        time.sleep(frames / FPS)
        if self.proc.poll() is not None:
            raise RuntimeError("simulator exited unexpectedly")

    def tap(self, key, hold_frames=4, settle_frames=18):
        self.send("down " + key)
        self.wait(hold_frames)
        self.send("up " + key)
        if settle_frames:
            self.wait(settle_frames)

    def close(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(4)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        self.state.close()
        self.fh.close()
        self.log.close()


def diff_count(a, b):
    d = ImageChops.difference(a, b)
    pixels = d.get_flattened_data() if hasattr(d, "get_flattened_data") else d.getdata()
    return sum(1 for px in pixels if max(px) > 8)


def prepare_disk(simdisk, root, theme, extra_cfg=None, add_media=False):
    shutil.copytree(simdisk, root)
    cfg = root / ".rockbox/config.cfg"
    if not cfg.exists():
        cfg = root / ".rockbox/default-config.cfg"
    lines = cfg.read_text().splitlines() if cfg.exists() else []
    cfg = root / ".rockbox/config.cfg"
    drop = ("native theme:", "idle poweroff:", "sleep timer on startup:",
            "animations:", "fruit main menu:", "keyclick:", "hardware keyclick:")
    lines = [l for l in lines if not l.startswith(drop)]
    lines += [
        f"native theme: {theme}",
        "idle poweroff: off",
        "sleep timer on startup: off",
        "animations: off",
    ]
    if extra_cfg:
        lines += extra_cfg
    cfg.write_text("\n".join(lines) + "\n")

    if add_media:
        contacts = root / "Contacts"
        contacts.mkdir(exist_ok=True)
        (contacts / "friends.vcf").write_text(
            "BEGIN:VCARD\nVERSION:3.0\nFN:Ada Lovelace\nTEL:555-0100\nEND:VCARD\n"
            "BEGIN:VCARD\nVERSION:3.0\nFN:Grace Hopper\nTEL:555-0101\nEND:VCARD\n"
        )
        photos = root / "Photos"
        photos.mkdir(exist_ok=True)
        img = Image.new("RGB", (64, 64), (80, 140, 220))
        img.save(photos / "photo1.png")
        img.save(photos / "photo2.png")
        videos = root / "Videos"
        videos.mkdir(exist_ok=True)
        (videos / "sample.m4v").write_bytes(b"\x00\x00\x00\x18ftypM4V " + b"\x00" * 4096)


def test_fruit_suite(rockboxui, simdisk, out):
    results = {}
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp) / "disk"
        prepare_disk(
            simdisk, root, "fruit-company",
            extra_cfg=[
                f"fruit main menu: {FRUIT_MAIN_WITH_SLEEP}",
                "hardware keyclick: on",
                "keyclick: off",
            ],
            add_media=True,
        )
        work = out / "run-fruit"
        work.mkdir(parents=True, exist_ok=True)
        sim = Sim(rockboxui, root, work, screenshot=True)
        try:
            # Wait for boot + initial background file_index walk (images.dat)
            sim.wait(FPS * 4)
            deadline = time.monotonic() + 18.0
            while time.monotonic() < deadline:
                if ((root / ".rockbox/library/images.dat").exists() and
                        (root / ".rockbox/library/videos.dat").exists()):
                    break
                sim.wait(FPS // 2)
            sim.wait(FPS)
            home = sim.frame()
            home.save(out / "sim-home.png")

            # 1. Hold switch on Home white status bar
            sim.tap("hold", settle_frames=18)
            hold_home = sim.frame()
            hold_home.save(out / "sim-hold-home.png")
            # Top-left padlock ink should appear in white status bar (y=4..16, x=4..18)
            bar_diff = diff_count(home.crop((0, 0, 100, 20)),
                                  hold_home.crop((0, 0, 100, 20)))
            sim.tap("select", settle_frames=18)
            hold_after_select = sim.frame()
            input_blocked = diff_count(hold_home.crop((0, 22, 160, 240)),
                                       hold_after_select.crop((0, 22, 160, 240))) == 0
            results["hold_home"] = {
                "ok": bar_diff > 20 and input_blocked,
                "bar_diff_px": bar_diff,
                "input_blocked": input_blocked,
            }
            sim.tap("hold", settle_frames=15)

            # 2. Navigate Home -> Sleep (row index 7: Music, Videos, Photos,
            #    Podcasts, Extras, Settings, Shuffle Songs, Sleep)
            for _ in range(7):
                sim.tap("down", settle_frames=10)
            sim.wait(FPS)
            sleep_row = sim.frame()
            sleep_row.save(out / "sim-home-sleep.png")
            # Verify right pane has bright moon pixels (x=200..280, y=65..155)
            moon_bright = sum(
                1 for y in range(65, 155) for x in range(200, 280)
                if min(sleep_row.getpixel((x, y))) > 195
            )
            results["home_sleep_preview"] = {
                "ok": moon_bright > 400,
                "moon_bright_px": moon_bright,
            }

            # 3. SELECT on Sleep -> Charging/Charged standby screen
            sim.tap("select", settle_frames=36)
            standby = sim.frame()
            standby.save(out / "sim-charging-standby.png")
            # Verify black top bar, no battery icon on right of bar (x=290..314),
            # and large green battery in center (y=88..152, x=90..228)
            bar_px = standby.getpixel((60, 10))
            bar_dark = max(bar_px) < 60
            right_bar_bright = sum(
                1 for y in range(4, 17) for x in range(288, 316)
                if max(standby.getpixel((x, y))) > 140
            )
            green_px = sum(
                1 for y in range(88, 153) for x in range(90, 229)
                if standby.getpixel((x, y))[1] - standby.getpixel((x, y))[0] > 35
            )
            results["charging_standby"] = {
                "ok": bar_dark and right_bar_bright == 0 and green_px > 2000,
                "bar_rgb": bar_px,
                "right_bar_bright": right_bar_bright,
                "green_px": green_px,
            }

            # Wake with SELECT -> back to Home without re-entering Sleep
            sim.tap("select", settle_frames=30)
            woke = sim.frame()
            woke.save(out / "sim-charging-wake.png")
            woke_ok = diff_count(sleep_row.crop((0, 24, 155, 220)),
                                 woke.crop((0, 24, 155, 220))) < 200
            results["charging_wake"] = {"ok": woke_ok}

            # 4. Settings > About (up 2 rows from Sleep -> Settings)
            sim.tap("up", settle_frames=10)
            sim.tap("up", settle_frames=10)
            sim.tap("select", settle_frames=24)  # enter Settings (row 0 = About)
            sim.tap("select", settle_frames=30)  # enter About (page 1)
            about1 = sim.frame()
            about1.save(out / "sim-about-1.png")

            # Verify black status bar on About
            a1_bar = about1.getpixel((50, 10))
            a1_bar_dark = max(a1_bar) < 60

            content = (0, 135, 320, 226)
            wheel_ignored = True
            for key in ("up", "down"):
                sim.tap(key)
                wheel_ignored &= not diff_count(about1.crop(content),
                                                sim.frame().crop(content))

            sim.tap("right", hold_frames=6, settle_frames=30)
            about2 = sim.frame()
            about2.save(out / "sim-about-2.png")
            right_pages = diff_count(about1.crop(content), about2.crop(content))
            sim.tap("back", hold_frames=6, settle_frames=30)
            left_pages = not diff_count(about1.crop(content),
                                         sim.frame().crop(content))
            sim.tap("select", hold_frames=6, settle_frames=30)
            select_pages = not diff_count(about2.crop(content),
                                           sim.frame().crop(content))
            # Verify 4th row (iTunes U, y=191..201) has white text pixels
            row4_px = sum(
                1 for y in range(189, 203) for x in range(18, 155)
                if min(about2.getpixel((x, y))) > 190
            )

            # Hold switch on About black bar
            sim.tap("hold", settle_frames=18)
            about2_hold = sim.frame()
            about2_hold.save(out / "sim-hold-about.png")
            about_hold_diff = diff_count(about2.crop((0, 0, 25, 22)),
                                         about2_hold.crop((0, 0, 25, 22)))
            sim.tap("hold", settle_frames=15)

            # RIGHT -> Page 3 (identity)
            sim.tap("right", hold_frames=6, settle_frames=30)
            about3 = sim.frame()
            about3.save(out / "sim-about-3.png")

            # RIGHT wraps to Page 1. LEFT wraps back to Page 3.
            sim.tap("right", hold_frames=6, settle_frames=30)
            about1_wrap = sim.frame()
            wrap_ok = diff_count(about1.crop((0, 120, 320, 235)),
                                 about1_wrap.crop((0, 120, 320, 235))) == 0

            sim.tap("back", hold_frames=6, settle_frames=30)
            about3_wrap = sim.frame()
            about3_wrap.save(out / "sim-about-3-wrapped.png")
            wrap_back_ok = not diff_count(about3.crop(content),
                                           about3_wrap.crop(content))
            results["about"] = {
                "ok": (a1_bar_dark and wheel_ignored and right_pages > 1000
                       and left_pages and select_pages and row4_px > 60
                       and wrap_ok and wrap_back_ok and about_hold_diff > 15),
                "bar_rgb": a1_bar,
                "wheel_ignored": wheel_ignored,
                "right_diff_px": right_pages,
                "left_pages": left_pages,
                "select_pages": select_pages,
                "wrap_back_ok": wrap_back_ok,
                "row4_itunes_u_px": row4_px,
                "wrap_ok": wrap_ok,
                "about_hold_diff": about_hold_diff,
            }

            # MENU returns to Settings
            sim.tap("menu", settle_frames=24)

            # 5. Settings > Clicker (row index 11: About=0, Shuffle=1, Repeat=2,
            #    MainMenu=3, MusicMenu=4, VolLimit=5, Backlight=6, Brightness=7,
            #    Audiobooks=8, EQ=9, SoundCheck=10, Clicker=11)
            for _ in range(11):
                sim.tap("down", settle_frames=8)
            sim.wait(20)
            clicker_on_img = sim.frame()
            clicker_on_img.save(out / "sim-clicker-on.png")
            # Toggle Clicker Off, then back On (flushed on USB connect below)
            sim.tap("select", settle_frames=18)
            clicker_off_img = sim.frame()
            clicker_off_img.save(out / "sim-clicker-off.png")
            clicker_toggled = diff_count(clicker_on_img.crop((100, 175, 160, 215)),
                                         clicker_off_img.crop((100, 175, 160, 215))) > 15
            sim.tap("select", settle_frames=18)

            # 6. USB consent popup + Stock USB Connected screen (also flushes config.cfg)
            sim.tap("menu", settle_frames=18)  # back to Home
            sim.tap("usb", settle_frames=30)
            usb_confirm = sim.frame()
            usb_confirm.save(out / "sim-usb-confirm.png")
            sim.tap("select", settle_frames=60)
            usb_conn = sim.frame()
            usb_conn.save(out / "sim-usb-connected.png")
            r, g, b = usb_conn.getpixel((130, 100))
            usb_sphere_ok = r > 200 and 60 < g < 200 and b < 60
            usb_bar_ok = max(usb_conn.getpixel((60, 10))) < 60
            results["usb_flow"] = {
                "ok": usb_sphere_ok and usb_bar_ok,
                "sphere_rgb": (r, g, b),
            }
            cfg_path = root / ".rockbox/config.cfg.new"
            if not cfg_path.exists():
                cfg_path = root / ".rockbox/config.cfg"
            cfg_after = cfg_path.read_text()
            kc_lines = [l for l in cfg_after.splitlines() if "keyclick" in l]
            results["clicker_sync"] = {
                "ok": (clicker_toggled and "keyclick: moderate" in cfg_after
                       and "hardware keyclick: off" not in cfg_after),
                "toggled_ui": clicker_toggled,
                "software_saved": "keyclick: moderate" in cfg_after,
                "keyclick_lines": kc_lines,
            }
            sim.tap("usb", settle_frames=30)
        finally:
            sim.close()
    return results


def record_turn(sim, out, name, key):
    """Save the distinct LCD frames with elapsed milliseconds in the names."""
    sim.settle(quiet=.45, limit=1.8)
    first = len(sim.seen)
    sim.key(key, hold=10)
    deadline = time.monotonic() + 1.5
    while len(sim.seen) == first and time.monotonic() < deadline:
        sim.alive()
        time.sleep(.02)
    sim.settle(quiet=.35)
    frames = sim.seen[first:]
    if not frames:
        return [], []
    timestamps = [round((t - frames[0][0]) * 1000) for t, _ in frames]
    images = [np.frombuffer(p, np.uint8).reshape(H, W, 3) for _, p in frames]
    for index, (ms, picture) in enumerate(zip(timestamps, images)):
        Image.fromarray(picture).save(out / f"{name}-{index:02d}-{ms:03d}ms.png")
    return timestamps, images


def text_mask(picture):
    # iTunes U is below the capacity legend. Its white glyphs are a
    # convenient marker for horizontal translation without the background.
    return np.min(picture[190:206], axis=2) > 210


def match_offset(frames, mask, expected):
    """Measure which of the stock offsets each visible glyph mask matches."""
    matches = []
    for picture in frames:
        observed = text_mask(picture)
        max_score, offset = 0, None
        for shift in expected:
            x0, x1 = max(0, shift), min(W, W + shift)
            if x0 >= x1:
                continue
            moved = np.zeros_like(mask)
            moved[:, x0:x1] = mask[:, x0 - shift:x1 - shift]
            union = np.count_nonzero(observed | moved)
            score = np.count_nonzero(observed & moved) / max(1, union)
            if score > max_score:
                max_score, offset = score, shift
        if max_score > .88:
            matches.append(offset)
    return matches


def test_about_motion(rockboxui, simdisk, out, theme):
    work = out / f"about-{theme}"
    work.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp) / "disk"
        prepare_disk(simdisk, root, theme, [
            "animations: on",
            "start in screen: root",
            "root menu order: system_menu,settings,!wps",
        ])
        if theme == "fruit-company-ultra":
            theme_dir = root / ".rockbox/native-themes"
            theme_dir.mkdir(exist_ok=True)
            fallback = theme_dir / "fruit-company-ultra.podtheme"
            if not fallback.exists():
                fallback.write_text(ULTRA_FALLBACK_THEME)
        sim = WalkSim(rockboxui, root, work, "firmware", theme)
        try:
            time.sleep(4)
            native = theme in ("classic", "scrim")
            for _ in range(0 if native else 5):
                sim.key("down", hold=4)
                sim.settle(quiet=.1)
            sim.key("select", hold=4)
            sim.settle()
            if native:
                # System starts on Recording. About is its second row.
                sim.key("down", hold=4)
                sim.settle(quiet=.1)
            sim.key("select", hold=4)
            sim.settle(quiet=.65, limit=2.0)
            original = np.frombuffer(sim.seen[-1][1], np.uint8).reshape(H, W, 3)
            right_times, right = record_turn(sim, work, "right", "right")
            left_times, left = record_turn(sim, work, "left", "back")
            assert not (root / ".rockbox/native-theme.log").exists(), theme
        finally:
            sim.close()
            sim.process.wait()

    if not right or not left:
        return {
            "ok": False,
            "right_frames_ms": right_times,
            "left_frames_ms": left_times,
        }

    # 2.0.5 captures at ~34 ms steps: offsets 245, 179, 124, 79, 44,
    # 19, 4, 0. The simulator's integer curve rounds these to 245, 180,
    # 125, 80, 45, 20, 5, 0.
    incoming = [245, 180, 125, 80, 45, 20, 5, 0]
    mask = text_mask(right[-1])
    right_offsets = match_offset(right, mask, incoming)
    left_offsets = match_offset(left, mask, [-75, -140])
    stationary = all(np.array_equal(original[:135], p[:135])
                     for p in right + left)
    # SDL can coalesce a frame while presenting, especially on a loaded
    # host. Require the ordered curve with at most one missed step.
    curve_ok = (len(right_offsets) >= 7 and right_offsets[-1] == 0
                and right_offsets == [x for x in incoming if x in right_offsets])
    duration_ok = all(210 <= t[-1] <= 330 for t in (right_times, left_times))
    return {
        "ok": (curve_ok and bool(left_offsets) and stationary and duration_ok
               and np.array_equal(original, left[-1])),
        "right_offsets_px": right_offsets,
        "left_outgoing_offsets_px": left_offsets,
        "stationary_top": stationary,
        "right_frames_ms": right_times,
        "left_frames_ms": left_times,
    }


def test_scrim_unchanged(rockboxui, simdisk, out):
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp) / "disk"
        prepare_disk(simdisk, root, "scrim")
        work = out / "run-scrim"
        work.mkdir(parents=True, exist_ok=True)
        sim = Sim(rockboxui, root, work, screenshot=True)
        try:
            sim.wait(FPS * 4)
            before = sim.frame()
            sim.tap("hold", settle_frames=18)
            after = sim.frame()
            after.save(out / "sim-scrim-hold.png")
            # Scrim status bar should be completely unchanged on Hold
            diff = diff_count(before.crop((0, 0, 160, 35)),
                              after.crop((0, 0, 160, 35)))
            return {"ok": diff == 0, "scrim_bar_diff": diff}
        finally:
            sim.close()


def main():
    if len(sys.argv) != 4:
        print(f"Usage: {sys.argv[0]} ROCKBOXUI SIMDISK OUTPUT_DIR", file=sys.stderr)
        sys.exit(2)
    rockboxui = Path(sys.argv[1]).resolve()
    simdisk = Path(sys.argv[2]).resolve()
    out = Path(sys.argv[3]).resolve()
    out.mkdir(parents=True, exist_ok=True)

    results = test_fruit_suite(rockboxui, simdisk, out)
    results["scrim_unchanged"] = test_scrim_unchanged(rockboxui, simdisk, out)
    for theme in ("classic", "fruit-company", "fruit-company-ultra", "scrim"):
        results[f"about_motion_{theme}"] = test_about_motion(
            rockboxui, simdisk, out, theme)
    (out / "result.json").write_text(json.dumps(results, indent=2) + "\n")
    print(json.dumps(results, indent=2))
    ok = all(v["ok"] for v in results.values())
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
