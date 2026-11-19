"""Validate the FU108 lyric addenda on a disposable SDL build."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time
import wave
from zipfile import ZipFile

import numpy as np
from PIL import Image
from ui_menu_sim import Screen, process

FIXTURE = """<tt xmlns:ttm="http://www.w3.org/ns/ttml#metadata">
<head><metadata><ttm:agent xml:id="a" type="person"/><ttm:agent xml:id="b" type="person"/></metadata></head>
<body><div>
<p begin="0s" end="3.2s" ttm:agent="a"><span begin="0s" end="2s">MAIN VOICE</span><span ttm:role="x-bg" begin="1s" end="3s"><span begin="1.2s" end="2s">BACKGROUND ECHO</span></span></p>
<p begin="1.5s" end="3.5s" ttm:agent="b">DUET VOICE<span ttm:role="x-bg" begin="1.5s" end="3s">WHOLE ECHO</span></p>
<p begin="12s" end="13s" ttm:agent="a">After the gap</p>
<p begin="13s" end="14s">One native row</p>
<p begin="14s" end="15s">Two native row</p>
<p begin="15s" end="16s">Three native row</p>
<p begin="16s" end="17s">Four native row</p>
<p begin="17s" end="18s">Five native row</p>
</div></body></tt>"""

GAP_FIXTURE = """<tt><body><div>
<p begin="0s" end="1s">First</p>
<p begin="2.5s" end="3.5s">Second</p>
<p begin="6s" end="7.5s">Third</p>
<p begin="9.5s" end="11s">Fourth</p>
</div></body></tt>"""


def instrument(source):
    fu111 = Path(__file__).with_name("fixtures") / "fu111_lyrics_probe.c"
    viewer = source / "apps-ipod/viewers/lyric_viewer/lyric_viewer.c"
    shutil.copyfile(Path(__file__).with_name("fixtures") / "fu108_lyrics_probe.c",
                    viewer.with_name("fu108_lyrics_probe.c"))
    text = viewer.read_text()
    padded = "layout->ink_above" in text
    native_pad = "layout->ink_above" if padded else "0"
    distance = "lv_distance(from, to, -1)" if padded else "lv_distance(from, to, -1, true, -1)"
    prefetch = ""
    if "font_prefetch_busy()" in text:
        prefetch = (
            "        for (unsigned i = 0; font_prefetch_busy() && i < 8 * HZ; ++i) {\n"
            "            lv_draw(true, LV_DRAW_ONLY);\n"
            "            sleep(1);\n"
            "        }\n"
            '        probe_expect(!font_prefetch_busy(), "glyph worker drained", 0);\n'
        )
    paint_end = "                align, vocal ? levels : NULL"
    paint_end += ", layout->ink_above);" if padded else ");"
    shutil.copyfile(fu111, viewer.with_name(fu111.name))
    replacements = {
        "static uint32_t lv_clock(void)\n{":
            "static uint32_t fu108_clock;\nstatic uint32_t lv_clock(void)\n{"
            "\n#ifdef SIMULATOR\n    if (fu108_clock) return fu108_clock;\n#endif",
        "static inline __attribute__((always_inline))\nvoid lv_ink_pixel":
            "#ifdef SIMULATOR\nstatic void probe_pixel(fb_data *, unsigned);\n"
            "static void probe_row(int, int, bool, int, int, int, int);\n"
            "static int probe_kind, probe_vocal;\n"
            "static bool fu111_row;\nstatic uint8_t fu111_actual[LV_PIXELS];\n"
            "static void fu111_begin(int, bool, int, const char *, int, int, int, int);\n"
            "static void fu111_end(void);\n#endif\n"
            "static inline __attribute__((always_inline))\nvoid lv_ink_pixel",
        "    *dst = lv_pack((base << 5) + (fg - base) * w);":
            "    *dst = lv_pack((base << 5) + (fg - base) * w);\n"
            "#ifdef SIMULATOR\n    probe_pixel(dst, coverage);\n"
            "    if (fu111_row) fu111_actual[dst - lv_frame()] = coverage;\n#endif",
        "        lv_text(render, key, layout->font, h, y, alpha, edge, dim, top, bottom,":
            f"#ifdef SIMULATOR\n        probe_row(i, r, background, layout->font, y, alpha, {native_pad});\n"
            "        if (getenv(\"FU111_LYRICS_PROBE\")) {\n"
            "            char reference[LV_TEXT];\n"
            "            strlcpy(reference, line + row->start, MIN(sizeof(reference), row->length + 1u));\n"
            f"            fu111_begin(i, background, layout->font, reference, y, alpha, align, {native_pad});\n"
            "        }\n#endif\n"
            "        lv_text(render, key, layout->font, h, y, alpha, edge, dim, top, bottom,",
        paint_end:
            paint_end + "\n"
            "#ifdef SIMULATOR\n        probe_kind = 0; probe_vocal = -1;\n        fu111_end();\n#endif",
        "    const int center_x = lv_text_x(36, global_settings.lyric_align) + 18;":
            "    const int center_x = lv_text_x(36, global_settings.lyric_align) + 18;\n"
            "#ifdef SIMULATOR\n    probe_kind = 1;\n#endif",
        "\n}\n\n/* Use the same moving neighbour poses":
            "\n#ifdef SIMULATOR\n    probe_kind = 0;\n#endif\n}\n\n/* Use the same moving neighbour poses",
        "\nint lyric_viewer(void)":
            ('\n#ifdef SIMULATOR\n'
             f'#define FU108_MANUAL_DISTANCE(from, to) {distance}\n'
             '#include "fu108_lyrics_probe.c"\n#include "fu111_lyrics_probe.c"\n'
             '#endif\n\nint lyric_viewer(void)'),
        "    unsigned trace = 0;\n    for (;;)":
            '    unsigned trace = 0;\n#ifdef SIMULATOR\n    const char *probe = getenv("FU108_LYRICS_PROBE");\n'
            '    if (probe) {\n' + prefetch +
            '        const char *test = getenv("FU111_LYRICS_PROBE");\n'
            '        if (test) fu111_probe(probe, test);\n'
            '        else fu108_lyrics_probe(probe);\n'
            '        goto done;\n    }\n#endif\n    for (;;)',
    }
    for before, after in replacements.items():
        assert text.count(before) == 1, before
        text = text.replace(before, after)
    viewer.write_text(text)
    menu = source / "apps-ipod/root_menu.c"
    text = menu.read_text()
    before = next(line for line in text.splitlines()
                  if "next_screen =" in line and "root_menu_setup_screens()" in line)
    seed = r"""
#ifdef SIMULATOR
    if (getenv("FU108_LYRICS_PROBE"))
    {
        if (playlist_create(NULL, NULL) < 0 ||
            playlist_insert_track(NULL, "/Fixture.wav", PLAYLIST_INSERT_LAST, false, true) < 0)
            abort();
        playlist_start(0, 0, 0);
        next_screen = GO_TO_WPS;
    }
#endif
"""
    menu.write_text(text.replace(before, before + seed, 1))


def run(build, resources, output, test=None):
    output.mkdir(parents=True)
    with tempfile.TemporaryDirectory(prefix="pbf-lyrics-") as temp, tempfile.TemporaryFile() as display:
        disk = Path(temp)
        with ZipFile(resources) as archive:
            for name in archive.namelist():
                if name.startswith(".rockbox/fonts/"):
                    archive.extract(name, disk)
        rockbox = disk / ".rockbox"
        (rockbox / "codecs").mkdir()
        shutil.copyfile(build / "lib/rbcodec/codecs/wav.codec", rockbox / "codecs/wav.codec")
        (rockbox / "config.cfg").write_text(
            "start in screen: files\nshow files: music\nbacklight timeout: off\n"
            "voice menus: off\nanimations: on\nlyrics lead: 0\nlyric align: centre\n",
        )
        with wave.open(str(disk / "Fixture.wav"), "wb") as audio:
            audio.setparams((1, 2, 22050, 0, "NONE", "not compressed"))
            audio.writeframes(bytes(22050 * 2 * 30))
        fixture = FIXTURE
        if test:
            fixture = FIXTURE.replace("MAIN VOICE", "gjpqy。封國目").replace(
                "DUET VOICE", "gjpqy。驚臉重").replace(
                "BACKGROUND ECHO", "gjpqy。個皿").replace(
                "WHOLE ECHO", "gjpqy。妳渾").replace(
                "After the gap", "gjpqy。謊國齒")
            if test == "lead":
                fixture = re.sub(r'(begin|end)="([\d.]+)s"',
                                 lambda m: f'{m[1]}="{float(m[2]) + 5.5:g}s"', fixture)
            elif test == "short":
                fixture = GAP_FIXTURE
        (disk / "Fixture.ttml").write_text(fixture)
        runtime = disk / "runtime"
        runtime.mkdir(mode=0o700)
        with (output / "xvfb.log").open("w") as xlog, (output / "sdl.log").open("w") as log, process(
            ["Xvfb", "-displayfd", str(display.fileno()), "-screen", "0", "800x600x24", "-nolisten", "tcp"],
            xlog, pass_fds=(display.fileno(),),
        ) as xvfb:
            for _ in range(120):
                display.seek(0)
                number = display.read().decode().strip()
                if number:
                    break
                assert xvfb.poll() is None
                time.sleep(.025)
            else:
                raise AssertionError("Xvfb did not start")
            env = dict(os.environ, DISPLAY=":" + number, SDL_VIDEODRIVER="x11", SDL_AUDIODRIVER="dummy",
                       SDL_RENDER_DRIVER="software", XDG_RUNTIME_DIR=str(runtime),
                       FU108_LYRICS_PROBE=str(output.resolve()))
            if test:
                env["FU111_LYRICS_PROBE"] = test
            success = "FU111 PASS" if test else "FU108 PASS"
            with process(
                [str(build / "rockboxui"), "--root", str(disk), "--nobackground", "--zoom", "1"],
                log, env=env, cwd=disk,
            ) as player:
                time.sleep(2.2)
                screen = Screen(env, player)
                screen.tap("Return")
                for _ in range(400):
                    if success in (output / "sdl.log").read_text(errors="replace"):
                        break
                    assert player.poll() is None, "SDL assertion failed. See sdl.log."
                    time.sleep(.05)
                else:
                    raise AssertionError("SDL never finished the lyric checks. See sdl.log.")

    log = (output / "sdl.log").read_text()
    text = re.search(re.escape(success) + r" (.*)", log).group(1)
    result = {key: int(value) for key, value in re.findall(r"(\w+)=(\d+)", text)}
    names = ([file.stem for file in output.glob("*.raw")] if test else
             ["gap-dots", "manual-selected", "manual-crossfade", "main-bg-duet",
              "background-word-sweep", "vocals-dim"])
    for name in names:
        p = np.fromfile(output / (name + ".raw"), dtype="<u2").reshape(240, 320)
        rgb = [(p >> 11) * 255 // 31, (p >> 5 & 63) * 255 // 63, (p & 31) * 255 // 31]
        frame = Image.fromarray(np.stack(rgb, axis=-1).astype("uint8"))
        frame.save(output / (name + ".png"))
        if name == "gap-dots":
            left, top = map(int, re.findall(r"FU108 dot crop (\d+) (\d+)", log)[-1])
            edge_values = np.unique(p[top:top + 8, left:left + 8] >> 5 & 63)
            assert len(edge_values) >= 5, "antialiased dot needs intermediate coverage"
            result["dot_coverage_levels"] = len(edge_values)
            crop = frame.crop((left - 2, top - 2, left + 10, top + 10))
            crop.resize((144, 144), Image.Resampling.NEAREST).save(output / "dot-12x.png")
    (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(result)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--instrument", type=Path)
    parser.add_argument("--fu111", choices=("gap", "lead", "ink", "short", "photo", "photo-base"))
    parser.add_argument("paths", nargs="*", type=lambda p: Path(p).resolve())
    args = parser.parse_args()
    if args.instrument:
        instrument(args.instrument)
    else:
        run(*args.paths, test=args.fu111)
