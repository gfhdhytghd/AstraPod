#!/usr/bin/env python3
"""Compare splash surfaces in real iPod 6G SDL builds.

Pass --prepare to instrument a disposable source copy before configuring it.
The adapter invokes the production splash functions after normal startup and
records their default framebuffer. The candidate also records popup nav calls
and exercises its no-memory fallback.
"""
import argparse
from array import array
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
import time
import zipfile

from PIL import Image

from ui_menu_sim import FPS, SIZE, process


ADAPTER = r"""
#include "draw/viewport.h"
#include "ui/animation.h"

static int splash_probe_popups;
bool splash_probe_no_memory;

void splash_probe_nav(void)
{
    splash_probe_popups++;
}

static void splash_probe_frame(const char *tag, int expected)
{
    extern SDL_Surface *lcd_surface;
    const char *out = getenv("PODBOX_SPLASH_PROBE_DIR");
    if (!out || !lcd_surface)
        abort();

    char path[MAX_PATH];
    snprintf(path, sizeof(path), ROCKBOX_DIR "/splash-%s.rgb565", tag);
    int fd = creat(path, 0666);
    if (fd >= 0)
    {
        write(fd, lcd_framebuffer_default.fb_ptr, FRAMEBUFFER_SIZE);
        close(fd);
    }
    char filename[1024];
    int n = snprintf(filename, sizeof(filename), "%s/screen-%s.bmp", out, tag);
    if (n < 0 || (size_t)n >= sizeof(filename) || SDL_SaveBMP(lcd_surface, filename))
        abort();
    fprintf(stderr, "SPLASH %s %d %d\n", tag, splash_probe_popups, expected);
    splash_probe_popups = 0;
}

static void splash_probe_base(void)
{
    struct viewport *previous = lcd_set_viewport(NULL);
    fb_data *pixels = lcd_framebuffer_default.fb_ptr;
    for (int y = 0; y < LCD_HEIGHT; y++)
        for (int x = 0; x < LCD_WIDTH; x++)
            pixels[y * LCD_WIDTH + x] =
                LCD_RGBPACK(24 + (x / 11) % 7 * 19, 13 + (y / 5) % 13 * 13, 72);
    lcd_update();
    lcd_set_viewport(previous);
}

static void splash_probe_run(void)
{
    struct viewport area;
    splash_probe_base();
    splash_probe_frame("underlay", 0);
    splashf(ANIM_F(8) * HZ / UI_ANIM_FPS, "%s", "Timed notice");
    splash_probe_frame("timed", 1);

    splash_probe_base();
    splashf(0, "%s", "Instant notice 中文提示");
    splash_probe_frame("instant", 1);

    splash_probe_base();
    splashf(0, "%s\n%s", "First line", "This longer line should wrap before the right edge.");
    splash_probe_frame("wrapped", 1);

    splash_probe_base();
    splash_progress_set_delay(0);
    splash_progress(35, 100, "%s", "Preparing library");
    splash_probe_frame("progress", 1);
    splash_progress(100, 100, "%s", "Preparing library");
    splash_probe_frame("progress-done", 1);

    splash_probe_base();
    viewport_set_defaults(&area, SCREEN_MAIN);
    area.width = 265;
    area.height = 174;
    splashf_in(&area, "%s", "Embedded notice");
    splash_probe_frame("embedded", 0);
    splash_progress_in(&area, 70, 100, "%s", "Embedded progress");
    splash_probe_frame("embedded-progress", 0);

    splash_probe_base();
    splash_probe_no_memory = true;
    splashf(0, "%s", "No memory");
    splash_probe_no_memory = false;
    splash_probe_frame("fallback", 0);

    splash_probe_base();
    splashf(0, "%s", "");
    splash_probe_frame("empty", 0);
    fprintf(stderr, "SPLASH DONE\n");
}
"""

CASES = (
    "underlay", "timed", "instant", "wrapped", "progress", "progress-done",
    "embedded", "embedded-progress", "fallback", "empty",
)
NATIVE = {"timed", "instant", "wrapped", "progress", "progress-done"}
CUSTOM_THEME = """podbox-theme=1
color.modal_bg=204060
color.modal_fg=eedcc0
color.modal_border=9aa81c
color.modal_shadow=552288
font.body=12-noto-sans
metric.modal_padding=18
metric.modal_border_width=5
metric.modal_shadow_offset=7
metric.splash_progress_gap=12
metric.splash_progress_radius=0
"""


def inject(source):
    main = source / "apps-ipod/main.c"
    body = main.read_text()
    define = "static void init(void);"
    call = "    root_menu();"
    assert body.count(define) == body.count(call) == 1
    includes = '#include "config.h"'
    assert body.count(includes) == 1
    body = body.replace(includes, includes + """
#include <SDL_surface.h>
#include <stdio.h>
#include <stdlib.h>""")
    main.write_text(body.replace(define, ADAPTER + "\n" + define)
                    .replace(call, "    splash_probe_run();\n" + call))

    retained = source / "apps-ipod/ui/retained.c"
    if retained.exists():
        body = retained.read_text()
        marker = "    nav->entry[nav->depth++] = (struct ui_nav_entry){page, type};"
        assert body.count(marker) == 1
        retained.write_text(body.replace(marker, marker + """
    if (type == UI_POPUP)
    {
        extern void splash_probe_nav(void);
        splash_probe_nav();
    }"""))
        splash = source / "apps-ipod/widgets/splash.c"
        body = splash.read_text()
        marker = "    int handle = core_alloc_free_space(pixels_needed * sizeof(fb_data));"
        assert body.count(marker) == 1
        splash.write_text(body.replace(marker, """
    extern bool splash_probe_no_memory;
    int handle = splash_probe_no_memory ? -1 :
                 core_alloc_free_space(pixels_needed * sizeof(fb_data));"""))


def save_png(pixels, path):
    data = array("H")
    data.frombytes(pixels)
    if sys.byteorder != "little":
        data.byteswap()
    rgb = bytearray()
    for pixel in data:
        r, g, b = (pixel >> 11) & 31, (pixel >> 5) & 63, pixel & 31
        rgb.extend(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)))
    Image.frombytes("RGB", SIZE, rgb).save(path)


def run_case(binary, archive, out, mode, env):
    disk = out / ("disk-" + mode)
    disk.mkdir()
    with zipfile.ZipFile(archive) as package:
        for n in package.namelist():
            if n.startswith((".rockbox/fonts/", ".rockbox/themes/", ".rockbox/wps/",
                             ".rockbox/icons/", ".rockbox/backdrops/")):
                package.extract(n, disk)
    rockbox = disk / ".rockbox"
    config = (rockbox / "themes/scrim.cfg").read_text()
    theme = mode if mode in ("light", "custom") else "scrim"
    (rockbox / "config.cfg").write_text(
        config + "\nvoice menus: off\nbacklight timeout: off\n"
        f"native ui: {'on' if mode != 'off' else 'off'}\n"
        f"native theme: {theme}\n"
    )
    if mode == "custom":
        themes = rockbox / "native-themes"
        themes.mkdir()
        (themes / "custom.podtheme").write_text(CUSTOM_THEME)
    logfile = out / (mode + ".log")
    env = env | {"PODBOX_SPLASH_PROBE_DIR": str(disk)}
    with logfile.open("w") as log, process(
            [str(binary), "--root", str(disk), "--nobackground", "--zoom", "1",
             "--screenshot"], log, env=env, cwd=disk) as sim:
        for _ in range(FPS * 10):
            log_lines = logfile.read_text(errors="replace").splitlines()
            if "SPLASH DONE" in log_lines:
                break
            assert sim.poll() is None, logfile
            time.sleep(1 / FPS)
        else:
            raise AssertionError(f"Splash probe timed out. See {logfile}")

    visited = {}
    for line in log_lines:
        if line.startswith("SPLASH ") and line != "SPLASH DONE":
            _, name, count, expected = line.split()
            visited[name] = int(count)
            assert visited[name] == (int(expected) if mode not in {"old", "off"} else 0), line
    assert set(visited) == set(CASES)

    images = {}
    for name in CASES:
        pixels = (rockbox / f"splash-{name}.rgb565").read_bytes()
        assert len(pixels) == SIZE[0] * SIZE[1] * 2
        save_png(pixels, out / (mode + "-" + name + ".png"))
        with Image.open(disk / f"screen-{name}.bmp") as capture:
            panel = capture.convert("RGB")
            assert panel.size == SIZE
            panel.save(out / (mode + "-panel-" + name + ".png"))
            images[name] = (pixels, panel.tobytes())
    assert images["empty"] == images["underlay"]
    assert all(images[name][0] != images["underlay"][0] for name in CASES
               if name not in {"underlay", "empty"}), mode
    return images, visited


def pixel_values(image):
    return struct.unpack(f"<{SIZE[0] * SIZE[1]}H", image[0])


def pack(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def bounds(pixels, color):
    coords = [(i % SIZE[0], i // SIZE[0]) for i, pixel in enumerate(pixels) if pixel == color]
    left = min(x for x, _ in coords)
    top = min(y for _, y in coords)
    right = max(x for x, y in coords if y == top) + 1
    bottom = max(y for x, y in coords if x == left) + 1
    return left, top, right, bottom


def verify_theme(old, light, custom, archive):
    scrim_box = bounds(pixel_values(old["timed"]), pack(0x63, 0x65, 0x63))
    light_box = bounds(pixel_values(light["timed"]), pack(0x73, 0x70, 0x6C))
    assert scrim_box == light_box
    assert pack(0xFF, 0xFF, 0xFF) in pixel_values(light["timed"])
    assert pack(0x18, 0x18, 0x18) in pixel_values(light["timed"])

    with zipfile.ZipFile(archive) as package:
        font = package.read(".rockbox/fonts/12-noto-sans.fnt")
    body_height = struct.unpack_from("<H", font, 6)[0]
    border = pack(0x9A, 0xA8, 0x1C)
    fill = pack(0x20, 0x40, 0x60)
    foreground = pack(0xEE, 0xDC, 0xC0)
    shadow = pack(0x55, 0x22, 0x88)
    timed = pixel_values(custom["timed"])
    x, y, right, bottom = bounds(timed, border)
    assert bottom - y == body_height + 2 * 18
    assert timed[y * SIZE[0] + x + 3] == border
    assert timed[(y + 4) * SIZE[0] + x + 3] == border
    assert timed[(y + 5) * SIZE[0] + x + 5] == fill
    assert timed[(bottom + 6) * SIZE[0] + x + 7] == shadow
    assert timed[(bottom + 6) * SIZE[0] + right + 6] == shadow
    progress = pixel_values(custom["progress"])
    x, y, _, bottom = bounds(progress, border)
    assert bottom - y == body_height * 2 + 2 * 18 + 12
    meter_y = bottom - 18 - body_height
    assert progress[meter_y * SIZE[0] + x + 18] == foreground  # square corner
    assert custom["underlay"] == light["underlay"] == old["underlay"]
    return {"light_same_geometry": True, "custom_body_font_height": body_height,
            "custom_popup_colors_metrics": True}


def compare(old_binary, new_binary, archive, out):
    out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as runtime:
        env = os.environ | {"SDL_VIDEODRIVER": "dummy", "SDL_AUDIODRIVER": "dummy",
                            "SDL_RENDER_DRIVER": "software", "XDG_RUNTIME_DIR": runtime}
        old, old_events = run_case(old_binary, archive, out, "old", env)
        off, off_events = run_case(new_binary, archive, out, "off", env)
        on, on_events = run_case(new_binary, archive, out, "on", env)
        light, light_events = run_case(new_binary, archive, out, "light", env)
        custom, custom_events = run_case(new_binary, archive, out, "custom", env)
    mismatches = {
        mode: [name for name in CASES if old[name] != images[name]]
        for mode, images in (("off", off), ("on", on))
    }
    result = {"size": SIZE, "cases": CASES, "mismatches": mismatches,
              "popup_nav_pushes": {"old": old_events, "off": off_events, "on": on_events,
                                   "light": light_events, "custom": custom_events}}
    (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    assert not any(mismatches.values()), mismatches
    assert {name for name, value in on_events.items() if value} == NATIVE
    result["theme_checks"] = verify_theme(old, light, custom, archive)
    (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(f"PASS {len(CASES)} splash cases match legacy; Light and custom token checks passed")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prepare", type=Path)
    parser.add_argument("files", nargs="*", type=lambda s: Path(s).resolve())
    args = parser.parse_args()
    if args.prepare is not None and not args.files:
        inject(args.prepare.resolve())
    elif args.prepare is None and len(args.files) == 4:
        compare(*args.files)
    else:
        parser.error("Use --prepare SOURCE or OLD NEW PACKAGE EVIDENCE_DIR")
