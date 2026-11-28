#!/usr/bin/env python3
"""Check the first USB consent frame with a cold Home glyph cache.

Feed the existing SDL USB key as the simulator starts. Disable animations
to compare the first popup pixel for pixel, and delay background glyph reads
to exercise the disk race. No window opens.
"""

import argparse
import json
import os
import tempfile
import time
from pathlib import Path
from struct import Struct
from unittest.mock import patch

from PIL import Image, ImageChops

from usb_stock_screen_sim import (
    BACKGROUND, HEADER, H, PIX, SLOTS, STOCK_THEMES, W, WORD,
    Sim, check as check_disk_mode, prepare,
)

THEMES = (*STOCK_THEMES, "scrim")
LATE_INSERT = 2.5
SELECT_AT = 3.1
END = 3.7
PANEL_RECORD = Struct("<4sIcBhhhI32h")
# Match the SDL capture's round-to-nearest RGB565 expansion.
PANEL_RGB = [((value >> shift) * 255 + maximum // 2) // maximum
             for shift, maximum in ((3, 31), (2, 63), (3, 31))
             for value in range(256)]


def capture(executable, disk, out, theme, at_boot):
    """Return timestamped, distinct LCD frames from boot to Disk Mode."""
    out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp) / "disk"
        prepare(disk, root, theme, None)
        config = root / ".rockbox/config.cfg"
        with config.open("a") as cfg:
            cfg.write("animations: off\nstart in screen: root\n")
        env = {"PODBOX_GLYPH_DELAY_MS": "200" if at_boot else "0"}
        if at_boot:
            env["PODBOX_PANEL_RECORD"] = str(out / "panel.rec")
        with patch.dict(os.environ, env):
            sim = Sim(executable, root, out)
        frames = []
        previous = None
        seen = 0
        inserted = selected = released = False
        start = time.monotonic()
        try:
            while (elapsed := time.monotonic() - start) < END:
                if not inserted and (at_boot or elapsed >= LATE_INSERT):
                    sim.send("down usb")
                    sim.send("up usb")
                    inserted = True
                if not selected and elapsed >= SELECT_AT:
                    sim.send("down select")
                    selected = True
                if selected and not released and elapsed >= SELECT_AT + 0.08:
                    sim.send("up select")
                    released = True

                _, captured, _ = HEADER.unpack_from(sim.state)
                if captured - seen > SLOTS:
                    raise RuntimeError(f"{theme}: LCD capture ring overrun")
                for seq in range(seen + 1, captured + 1):
                    offset = HEADER.size + ((seq - 1) % SLOTS) * (WORD.size + PIX)
                    pixels = sim.state[offset + WORD.size:offset + WORD.size + PIX]
                    if WORD.unpack_from(sim.state, offset)[0] != seq:
                        raise RuntimeError(f"{theme}: LCD frame overwritten")
                    if pixels != previous:
                        frames.append((elapsed, Image.frombytes("RGB", (W, H), pixels)))
                        previous = pixels
                seen = captured
                sim.wait(0)
                time.sleep(0.002)
        finally:
            sim.close()
    return frames



def panel_frames(path):
    size = PANEL_RECORD.size + W * H * 2
    with path.open("rb") as file:
        while record := file.read(size):
            magic, usec, *_ = PANEL_RECORD.unpack_from(record)
            assert magic == b"PNL1" and len(record) == size
            rgb565 = record[PANEL_RECORD.size:]
            image = Image.frombytes("RGB", (W, H), rgb565, "raw", "BGR;16")
            yield usec, image.point(PANEL_RGB)


def title_mask(image, theme):
    # White title ink identifies the consent popup on both widget styles,
    # independent of the retained Home pixels underneath the glass.
    top, bottom = (93, 108) if theme == "scrim" else (56, 71)
    return (image.crop((74, top, 247, bottom)).convert("L")
            .point(lambda v: 255 if v >= 200 else 0).tobytes())


def differing_pixels(one, two, theme):
    top = 36 if theme == "scrim" else 20  # USB can change status icons.
    box = (0, top, W, H)
    r, g, b = ImageChops.difference(one.crop(box), two.crop(box)).split()
    return (H - top) * W - ImageChops.lighter(ImageChops.lighter(r, g), b).histogram()[0]


def run_theme(executable, disk, out, theme, disk_backdrop):
    out.mkdir(parents=True, exist_ok=True)
    later = capture(executable, disk, out / "run-later", theme, False)
    boot = capture(executable, disk, out / "run-boot", theme, True)
    home = next(image for t, image in reversed(later) if t < LATE_INSERT)
    reference = next(image for t, image in reversed(later) if t < SELECT_AT)
    title = title_mask(reference, theme)
    assert any(title) and title != title_mask(home, theme), f"{theme}: no late popup"
    late_at, late_popup = next((t, im) for t, im in later if title_mask(im, theme) == title)
    _, popup = next((t, im) for t, im in boot if title_mask(im, theme) == title)
    # SDL may coalesce Home and the popup into one render. The panel trace
    # records every LCD submission, including the frame behind the popup.
    parent = None
    for boot_at, image in panel_frames(out / "run-boot/panel.rec"):
        if title_mask(image, theme) == title:
            break
        paint_at, parent = boot_at, image
    else:
        raise RuntimeError(f"{theme}: missing popup submission")
    assert parent is not None, f"{theme}: missing pre-popup submission"
    for name, image in (("home", home), ("boot-parent", parent), ("boot-popup", popup),
                        ("hotplug-popup", late_popup), ("boot-disk", boot[-1][1])):
        image.save(out / f"{name}.png")

    disk_flags = check_disk_mode(theme, boot[-1][1], disk_backdrop)
    boot_log = (out / "run-boot/firmware.log").read_text(errors="replace")
    disk_ok = (disk_flags["ok"] and "USB inserted. Waiting for" in boot_log
               and "All threads have acknowledged the connect." in boot_log)
    parent_diff = differing_pixels(parent, home, theme)
    popup_diff = differing_pixels(popup, late_popup, theme)
    late_ms = round((late_at - LATE_INSERT) * 1000)
    return {
        "ok": parent_diff == popup_diff == 0 and 0 <= late_ms <= 250 and disk_ok,
        "home_vs_boot_parent_pixels": parent_diff,
        "hotplug_popup_vs_boot_popup_pixels": popup_diff,
        "boot_popup_after_parent_ms": round(((boot_at - paint_at) & 0xffffffff) / 1000),
        "hotplug_to_popup_ms": late_ms,
        "disk_mode": disk_ok,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("rockboxui", type=Path)
    parser.add_argument("simdisk", type=Path)
    parser.add_argument("output_directory", type=Path)
    parser.add_argument("themes", nargs="*", default=THEMES, choices=THEMES)
    args = parser.parse_args()
    disk_backdrop = Image.open(BACKGROUND).convert("RGB")
    output = args.output_directory.resolve()
    result = {}
    for theme in args.themes:
        result[theme] = run_theme(args.rockboxui.resolve(), args.simdisk.resolve(),
                                  output / theme, theme, disk_backdrop)
        print(theme, result[theme], flush=True)
    (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    return 0 if all(theme["ok"] for theme in result.values()) else 1


if __name__ == "__main__":
    raise SystemExit(main())
