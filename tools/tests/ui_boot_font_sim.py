#!/usr/bin/env python3
"""Capture the real boot painter from a scratch iPod 6G SDL build.

Run --instrument SOURCE_COPY before compiling the simulator, then pass
ROCKBOXUI PHASE4_ZIP EVIDENCE. The adapter records actual LCD frames and
caption cell advances after paint. It neither chooses nor draws a font.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import tempfile
import time
from zipfile import ZipFile

from PIL import Image, ImageChops

from ui_menu_sim import FPS, process

FONT = ".rockbox/fonts/12-noto-sans.fnt"
LOCALES = ("english", "chinese-simp", "chinese-trad")
CAPTIONS = {
    "english": ("Performing maintenance...", "Loading...", "Applying theme..."),
    "chinese-simp": ("Performing maintenance...", "读取...", "正在应用主题..."),
    # This language pack falls back to English for LANG_APPLYING_THEME.
    "chinese-trad": ("Performing maintenance...", "讀取中...", "Applying theme..."),
}

ADAPTER = r'''
#ifdef SIMULATOR
#include "rbunicode.h"

static void probe_boot_frame(const struct viewport *vp, const char *caption)
{
    extern SDL_Surface *lcd_surface;
    static unsigned frame;
    const char *directory = getenv("PODBOX_BOOT_CAPTURES");
    if (!directory || !lcd_surface || !caption)
        return;

    char path[1024];
    unsigned index = ++frame;
    int length = snprintf(path, sizeof path, "%s/%03u.bmp", directory, index);
    if (length < 0 || (size_t)length >= sizeof path ||
        SDL_SaveBMP(lcd_surface, path) != 0)
        abort();

    int width, height;
    font_getstringsize((const unsigned char *)caption, &width, &height, vp->font);
    struct font *font = font_get(vp->font);
    bool overlay = font_filename_matches_loaded_id(vp->font, FONT_DIR "/12-noto-sans.fnt");
    fprintf(stderr, "BOOT_CAPTURE\t%u\t%d\t%d\t%d\t%d\t%s\t", index,
            (LCD_WIDTH - width) / 2, BOOT_CAPTION_Y, height, overlay, caption);
    const unsigned char *next = (const unsigned char *)caption;
    while (*next)
    {
        ucschar_t cp;
        next = utf8decode(next, &cp);
        fprintf(stderr, "%d%s", font_get_width(font, cp), *next ? "," : "");
    }
    fprintf(stderr, "\n");
    fflush(stderr);
}
#endif
'''


def instrument(source):
    main = source / "apps-ipod/main.c"
    text = main.read_text()
    assert "#define BOOT_CAPTION_Y" in text
    marker = "static void boot_paint(int step, int total) INIT_ATTR;"
    assert text.count(marker) == 1 and "probe_boot_frame" not in text
    start = text.index(marker)
    end = text.index("/* The bar alone", start)
    before, painter, after = text[:start], text[start:end], text[end:]
    marker = '#include "config.h"'
    assert before.count(marker) == 1
    # SDL includes math.h, whose logf declaration must precede Rockbox's macro.
    before = before.replace(marker, marker + """
#ifdef SIMULATOR
#include <SDL_surface.h>
#include <stdlib.h>
#include <stdio.h>
#endif""")
    marker = "    lcd_update();"
    assert painter.count(marker) == 1
    marker = "    struct viewport *last_vp;"
    assert painter.count(marker) == 1
    painter = painter.replace(marker, marker + "\n    const char *probe_caption = NULL;")
    draw = "        screen->putsxy((vp.width - tw) / 2, BOOT_CAPTION_Y, "
    assert painter.count(draw) == 1
    expression = painter.split(draw, 1)[1].split(");", 1)[0]
    painter = painter.replace(draw, f"        probe_caption = {expression};\n" + draw)
    marker = "    lcd_update();"
    painter = painter.replace(marker, marker + """
#ifdef SIMULATOR
    probe_boot_frame(&vp, probe_caption);
#endif""")
    main.write_text(before + ADAPTER + painter + after)


def captures(log):
    samples = {}
    for line in log.read_text(errors="replace").splitlines(keepends=True):
        if not line.startswith("BOOT_CAPTURE\t") or not line.endswith("\n"):
            continue
        _, index, x, y, height, overlay, caption, advances = line.rstrip("\n").split("\t")
        samples.setdefault(caption, {"file": f"{int(index):03d}.bmp",
                                     "x": int(x), "y": int(y), "height": int(height),
                                     "overlay": bool(int(overlay)),
                                     "advances": [int(v) for v in advances.split(",")]})
    return samples


def run(sim, resources, languages, locale, output, omit_font=False):
    output.mkdir()
    with tempfile.TemporaryDirectory(prefix="podbox-boot-font-") as folder:
        fixture = Path(folder)
        with ZipFile(resources) as archive:
            for path in archive.namelist():
                if omit_font and path == FONT:
                    continue
                if path.startswith((".rockbox/fonts/", ".rockbox/themes/",
                                    ".rockbox/wps/", ".rockbox/icons/", ".rockbox/backdrops/")):
                    archive.extract(path, fixture)
        rbdir = fixture / ".rockbox"
        # ARM and SDL may number optional string IDs differently. Use the
        # simulator's own strings with the exact packaged phase-4 fonts.
        (rbdir / "langs").mkdir()
        shutil.copyfile(languages / f"{locale}.lng", rbdir / f"langs/{locale}.lng")
        legacy_config = rbdir / "themes/scrim.cfg"
        config = legacy_config.read_text() if legacy_config.is_file() else ""
        config += "\nbacklight timeout: off\nvoice menus: off\n"
        config += f"lang: /.rockbox/langs/{locale}.lng\n"
        (rbdir / "config.cfg").write_text(config)
        runtime = fixture / "runtime"
        runtime.mkdir(mode=0o700)
        env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy",
                   SDL_RENDER_DRIVER="software", XDG_RUNTIME_DIR=str(runtime),
                   PODBOX_BOOT_CAPTURES=str(output))
        log = output / "sim.log"
        with log.open("w") as slog, process(
            [str(sim), "--root", str(fixture), "--nobackground", "--zoom", "1", "--screenshot"],
            slog, env=env, cwd=fixture,
        ) as firmware:
            for _ in range(10 * FPS):
                found = captures(log)
                if all(caption in found for caption in CAPTIONS[locale]) or (
                    omit_font and all(caption in found for caption in CAPTIONS["english"])
                ):
                    return found
                if firmware.poll() is not None:
                    break
                time.sleep(1 / FPS)
        raise AssertionError(f"Expected boot captions missing for {locale}. See {log}")


def inspect(output, caption, sample):
    with Image.open(output / sample["file"]) as source:
        frame = source.convert("RGB")
    assert frame.size == (320, 240), frame.size
    background = frame.getpixel((0, 0))
    x, y = sample["x"], sample["y"]
    assert len(sample["advances"]) == len(caption)
    distinct = {}
    for ch, width in zip(caption, sample["advances"]):
        cell = frame.crop((x, y, x + width, y + sample["height"]))
        if ord(ch) > 127:
            diff = ImageChops.difference(cell, Image.new("RGB", cell.size, background))
            box = diff.getbbox()
            assert box, f"No ink rendered for {ch} in {caption}"
            distinct[ch] = (diff.crop(box).size, diff.crop(box).tobytes())
        x += width
    return len(set(distinct.values())), len(distinct)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--instrument", type=Path)
    parser.add_argument("--lang-dir", type=Path, help="Simulator-built .lng directory")
    parser.add_argument("--reference", type=Path, help="Earlier run for pixel comparisons")
    parser.add_argument("sim", type=Path, nargs="?")
    parser.add_argument("resources", type=Path, nargs="?")
    parser.add_argument("out", type=Path, nargs="?")
    args = parser.parse_args()
    if args.instrument:
        instrument(args.instrument)
        return
    if not (args.sim and args.resources and args.out):
        parser.error("Pass ROCKBOXUI PHASE4_ZIP EVIDENCE or --instrument SOURCE_COPY")
    out = args.out.resolve()
    languages = args.lang_dir or args.sim.resolve().parent / "apps-ipod/lang"
    out.mkdir(parents=True, exist_ok=True)
    summary = {}
    failures = []
    for name, locale, omit in [(locale, locale, False) for locale in LOCALES] + [
        ("missing-font", "chinese-simp", True),
    ]:
        directory = out / name
        found = run(args.sim.resolve(), args.resources.resolve(), languages.resolve(),
                    locale, directory, omit)
        summary[name] = {}
        captions = CAPTIONS[locale]
        if omit:
            expected = CAPTIONS["english"]
            if all(caption in found for caption in expected):
                captions = expected
            else:
                failures.append("Missing 12 px font did not fall back to English")
        for i, caption in enumerate(captions):
            sample = found[caption]
            frame_index = sample["file"]
            with Image.open(directory / frame_index) as frame:
                frame.convert("RGB").save(directory / f"caption-{i}.png")
            shapes, chars = inspect(directory, caption, sample)
            sample.update(distinct_han_shapes=shapes, distinct_han_chars=chars)
            summary[name][caption] = sample
            if not omit and shapes != chars:
                failures.append(f"{name} {caption}: {shapes} distinct Han shapes for {chars} characters")
            if sample["overlay"] != bool(chars and not omit):
                failures.append(f"{name} {caption}: unexpected font face")
            if args.reference:
                ref = args.reference / ("english" if omit else name) / f"caption-{i}.png"
                with Image.open(ref) as first, Image.open(directory / f"caption-{i}.png") as second:
                    diff = ImageChops.difference(first.convert("RGB"), second.convert("RGB"))
                bounds = diff.getbbox()
                sample["diff_bounds"] = bounds
                if not chars and bounds:
                    failures.append(f"{name} {caption}: legacy paint changed")
                if chars and (not bounds or bounds[1] < sample["y"] or
                              bounds[3] > sample["y"] + sample["height"]):
                    failures.append(f"{name} {caption}: changes outside the caption strip")
    (out / "result.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n")
    if failures:
        raise AssertionError("\n".join(failures))
    print("PASS actual Simplified/Traditional boot glyphs, English paint and missing-font fallback")


if __name__ == "__main__":
    main()
