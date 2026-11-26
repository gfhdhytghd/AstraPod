#!/usr/bin/env python3
"""Validate PBF metrics and compare a scratch SDL firmware's text to FreeType."""
import argparse
from contextlib import ExitStack
from itertools import groupby
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
from zipfile import ZipFile

from PIL import Image, ImageChops, ImageDraw
import compile as compiler

SAMPLES = [
    "Welcome to New York (New York)", "AVATAR Toyota WAVE", "0123456789",
    "封肖师字目（你好，世界。）", "专辑封面　艺术家像　播放列表",
    "仅显示音乐文件　流派　年份　曲师", "字形与基线：标点、间距、数字",
    "「通知」『打开』【保存】…", "Ångström Œuvre Été — 25%",
]


class BitmapFont:
    def __init__(self, file):
        self.data = file if isinstance(file, bytes) else file.read_bytes()
        (self.magic, self.height, self.ascent, self.max_width, self.max_record,
         anchors, self.replacement, self.count, self.rows, index, self.bitmaps,
         size) = compiler.HEADER.unpack_from(self.data)
        assert self.magic == b"PBF1" and size == len(self.data)
        assert self.bitmaps == index + compiler.INDEX.size * self.count
        self.kern = self.data[compiler.HEADER.size + 4 * anchors:index]
        self.index = dict((cp, (offset, length, advance, left, right))
                          for cp, offset, length, advance, left, right
                          in compiler.INDEX.iter_unpack(self.data[index:self.bitmaps]))
        assert len(self.index) == self.count
        assert list(self.index) == sorted(self.index)
        assert list(self.index)[self.replacement] == compiler.REPLACEMENT

    def check_fixed_cells(self, manifest):
        trailer_at = len(self.data) - 6 - compiler.FIXED_TRAILER.size
        magic, directory, hot, cold, stride, common, count, variable = (
            compiler.FIXED_TRAILER.unpack_from(self.data, trailer_at))
        assert magic == b"FIX1" and stride == manifest["cell_bytes"]
        assert not (len(self.data) | directory | hot | cold) % compiler.BLOCK
        assert common * stride == manifest["common_bytes"]
        order, end = compiler.frequency_order(self.index)
        assert common == end - 1 and count == manifest["fixed_count"]

        ordinal_count = sum(last - first + 1 for first, last in compiler.FIXED_RANGES)
        ranks = struct.unpack_from(f"<{ordinal_count}H", self.data, directory)
        variable_at = directory + 2 * ordinal_count
        variable_end = variable_at + variable * compiler.INDEX.size
        variable_index = {
            cp: (offset, length, advance, left, right)
            for cp, offset, length, advance, left, right in
            compiler.INDEX.iter_unpack(self.data[variable_at:variable_end])
        }
        assert variable_end <= hot <= hot + common * stride <= cold
        used_ranks = set()
        for cp, (offset, length, advance, left, right) in self.index.items():
            ordinal = compiler.fixed_ordinal(cp)
            if ordinal is None or left or right:
                assert variable_index.pop(cp) == (offset, length, advance, left, right)
                continue
            rank = ranks[ordinal]
            assert rank < count and rank not in used_ranks
            used_ranks.add(rank)
            cell = hot + rank * stride if rank < common else cold + (rank - common) * stride
            assert cell + stride <= trailer_at and length + 14 <= stride
            assert self.bitmaps + offset == cell + 6
            assert struct.unpack_from("<HHBB", self.data, cell) == (length, advance, left, right)
            assert self.data[cell + 6] & ~compiler.RECORD_RGB == 1
            assert self.data[cell + 6 + length:cell + 14 + length] == b"\xff" * 8
        assert not variable_index and used_ranks == set(range(count))
        assert all(ranks[compiler.fixed_ordinal(cp)] == i
                   for i, cp in enumerate(order[1:end]))

    def phases(self, cp, *, include_rgb=False):
        offset, size, _, _, _ = self.index[cp]
        record = self.data[self.bitmaps + offset:self.bitmaps + offset + size]
        rgb = bool(record[0] & compiler.RECORD_RGB)
        gray8 = bool(record[0] & compiler.RECORD_GRAY8)
        phase_count = record[0] & ~(compiler.RECORD_RGB | compiler.RECORD_GRAY8)
        assert phase_count in (1, 4) and not (gray8 and (rgb or phase_count != 1))
        start = 1
        for _ in range(phase_count):
            x, y, w, h = struct.unpack_from("<bbBB", record, start)
            end = start + 4 + (w * h if gray8 else (w * h + 1) // 2)
            next_phase = end + ((3 * w * h + 1) // 2 if rgb else 0)
            assert next_phase <= len(record)
            yield (x, y, w, h), record[start + 4:next_phase if include_rgb else end]
            start = next_phase
        assert start == len(record)

    def adjustment(self, a, b):
        row, col = self.index[a][3], self.index[b][4]
        start, stop = struct.unpack_from("<HH", self.kern, row * 2)
        base = 2 * (self.rows + 1)
        return next((d for c, d in compiler.PAIR.iter_unpack(self.kern[base + 3 * start:base + 3 * stop])
                     if c == col), 0)


def font_sources(stack, lang):
    directory = stack.enter_context(tempfile.TemporaryDirectory(prefix="pbf-reference-"))
    lib = compiler.build_raster(Path(directory))
    fonts = []
    for spec in compiler.locate_fonts({}):
        face = compiler.Face(*spec, lib)
        stack.callback(face.close)
        fonts.append(face)
    mapping = compiler.resolve(fonts, compiler.required_codepoints(lang))
    return fonts, mapping


def check_stock(bitmap, stock, faces, mapping):
    """Stock glyphs are copied as converted; every other glyph has one phase,
    a whole-pixel advance and no kerning."""
    assert (bitmap.height, bitmap.ascent) == (stock.height, stock.ascent)
    assert (bitmap.rows, bitmap.kern) == (1, bytes(4))
    for cp, (offset, length, advance, _, _) in bitmap.index.items():
        record = bitmap.data[bitmap.bitmaps + offset:bitmap.bitmaps + offset + length]
        if mapping[cp] is None:
            assert record == b"\x01\0\0\0\0"
        elif cp in stock.glyphs:
            assert (record, advance) == stock.glyphs[cp], f"U+{cp:04X}"
        else:
            assert len(list(bitmap.phases(cp))) == 1 and not advance % 64
            assert advance == (faces[mapping[cp]].advance(cp) + 32) & ~63


def check_compiler(first, second, lang):
    manifest = json.loads((first / "sources.json").read_text())
    stock_sets = compiler.stock_glyph_sets(compiler.ROOT / "apps-ipod/fonts/stock")
    assert len(manifest["fonts"]) == len(compiler.FONTS) + len(stock_sets)
    with ExitStack() as stack:
        fonts, mapping = font_sources(stack, lang)
        for filename in [*manifest["fonts"], "sources.json"]:
            path = filename if filename.endswith(".json") else filename + ".pbf"
            assert (first / path).read_bytes() == (second / path).read_bytes(), path
        scratch = Path(stack.enter_context(tempfile.TemporaryDirectory(prefix="pbf-fusion-")))
        for name, face_info in manifest["fonts"].items():
            bitmap = BitmapFont(first / (name + ".pbf"))
            size, weight = face_info["size"], face_info["weight"]
            required = (compiler.index_subset(mapping, compiler.INDEX_GLYPHS)
                        if size == 56 else
                        compiler.index_subset(mapping, stock_sets[name].glyphs)
                        if name.endswith("-index") else mapping).keys()
            assert set(bitmap.index) == set(required)
            bitmap.check_fixed_cells(face_info)
            for face in fonts:
                face.set_size(size, weight)
            assert (bitmap.height, bitmap.ascent) == (face_info["height"], face_info["ascent"])
            if name in stock_sets:
                check_stock(bitmap, stock_sets[name], fonts, mapping)
                continue
            shifts = compiler.cjk_alignment(fonts)
            fusion = compiler.load_fusion(size, compiler.FUSION_CACHE, scratch) if size in (8, 10) else {}
            for cp in bitmap.index:
                source = mapping[cp]
                phases = list(bitmap.phases(cp, include_rgb=True))
                if source is None:
                    assert phases == [((0, 0, 0, 0), b"")]
                    continue
                face = fonts[source]
                dot = fusion.get(cp) if compiler.ideograph(cp) else None
                if dot:
                    (x, _, w, h), ink = compiler.glyph_bitmap(dot)
                    top = bitmap.ascent - (bitmap.height - h) // 2
                    assert bitmap.index[cp][2] == dot.adv * 64
                    assert phases == [((x, top, w, h), compiler.pack_alpha(ink))]
                    continue
                assert bitmap.index[cp][2] == face.advance(cp)
                assert len(phases) == (1 if compiler.cjk(cp) else 4)
                if source >= 2 or cp in map(ord, "封肖师字目AVTo"):
                    for phase, ((x, y, w, h), alpha) in enumerate(phases):
                        if size == 12:
                            (left, top, width, height), expected = compiler.lcd_bitmap(face, cp, phase * 16)
                        else:
                            (left, top, width, height), ink = face.bitmap(cp, phase * 16)
                            expected = compiler.pack_alpha(ink)
                        y_shift = shifts.get(source, 0) if compiler.cjk(cp) else 0
                        assert (x, y, w, h) == (left, top + y_shift, width, height)
                        assert alpha == expected
            for a, b in ("AV", "WA", "To", "ク。", "ガ、"):
                cp = ord(a), ord(b)
                source = 2 if compiler.cjk(cp[0]) else 0
                script = int.from_bytes(b"Hani", "big") if source == 2 else 0
                ref = fonts[source].layout(cp, script)
                plain = fonts[source].layout(cp, script, False)
                assert ref and plain
                assert bitmap.adjustment(*cp) == ref[-1] - plain[-1] != 0, (name, a, b)
    print(json.dumps({"fonts": len(manifest["fonts"]), "glyphs_per_font": len(mapping),
                      "deterministic": True, "noto_glyphs": sum(
                          i is not None and fonts[i].name.startswith("Noto")
                          for i in mapping.values())}))


def instrument(source):
    main = source / "apps-ipod/main.c"
    s = main.read_text()
    s = s.replace('#include "config.h"', '#include "config.h"\n#ifdef SIMULATOR\n#include <SDL_surface.h>\n#endif', 1)
    marker = "static void init(void);"
    assert s.count(marker) == 1
    s = s.replace(marker, '#ifdef SIMULATOR\n#include "sdl_probe.c"\n#endif\n' + marker)
    shutil.copyfile(Path(__file__).with_name("sdl_probe.c"), main.with_name("sdl_probe.c"))
    marker = "    CHART(\"<init\");"
    assert s.count(marker) == 1
    s = s.replace(marker, marker + "\n#ifdef SIMULATOR\n    font_probe();\n#endif")
    main.write_text(s)


def run_simulator(binary, base, fonts, samples, work):
    work.mkdir(parents=True, exist_ok=True)
    with ZipFile(base) as archive:
        for entry in archive.namelist():
            if entry.startswith((".rockbox/fonts/", ".rockbox/langs/", ".rockbox/codepages/")):
                archive.extract(entry, work)
    rb = work / ".rockbox"
    if fonts:
        for path in fonts.glob("*.pbf"):
            shutil.copyfile(path, rb / "fonts" / path.name)
    (rb / "font-probe-input.txt").write_text("\n".join(samples) + "\n")
    # SDL runtime gets its own directory and simulated disk.
    runtime = work / "runtime"
    runtime.mkdir(mode=0o700, exist_ok=True)
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy",
               SDL_RENDER_DRIVER="software", XDG_RUNTIME_DIR=str(runtime))
    with (work / "simulator.log").open("wb") as log:
        subprocess.run([str(binary.resolve()), "--root", str(work.resolve()),
                        "--nobackground", "--zoom", "1"], cwd=work, env=env,
                       stdout=log, stderr=subprocess.STDOUT, check=True, timeout=180)
    return (rb / "font-probe-output.txt").read_text().splitlines()


def comparison(before, after, output):
    old = Image.open(before).convert("RGB")
    new = Image.open(after).convert("RGB")
    assert old.size == new.size == (320, 240), (old.size, new.size)
    mag = 2
    canvas = Image.new("RGB", (320 * 3 * mag, 240 * mag + 32), "#171b22")
    diff = ImageChops.difference(old, new).point(lambda v: min(255, v * 2))
    d = ImageDraw.Draw(canvas)
    for i, (label, image) in enumerate(zip(("RB12", "PBF1", "Pixel difference"), (old, new, diff))):
        x = i * 320 * mag
        d.text((x + 7, 10), label, fill="white")
        canvas.paste(image.resize((320 * mag, 240 * mag), Image.Resampling.NEAREST), (x, 32))
    canvas.save(output)


def verify_sdl(trace, samples, fonts, lang):
    bitmap = BitmapFont(fonts / "18-noto-sans.pbf")
    sdl_height, sdl_ascent, cache = map(int, trace[0].split()[1:])
    assert (sdl_height, sdl_ascent) == (bitmap.height, bitmap.ascent)
    assert len(trace) == len(samples) + 2, (len(trace), len(samples))
    expected_hits = max_pre = max_draw = glyph_count = 0
    sample_errors = []
    with ExitStack() as stack:
        faces, mapping = font_sources(stack, lang)
        for face in faces:
            face.set_size(18, 400)
        for index, (sample, record) in enumerate(zip(samples, trace[1:-1], strict=True)):
            nr, width, elapsed, *entries, pen = record.split()
            assert int(nr) == index
            glyphs = [tuple(map(int, v.split(",")[1:])) for v in entries]
            cp = [int(s.split(",")[0], 16) for s in entries]
            assert cp == list(map(ord, sample)), (index, sample, cp)
            expected = []
            fpen = 0
            j = 0
            # Whole same-face runs are shaped by the FreeType-backed layout
            # engine. Source changes split shaping, with a common baseline.
            for source, points in groupby(cp, key=lambda c: mapping[c]):
                segment = tuple(points)
                if source is None:
                    positions = (0,) * (len(segment) + 1)
                else:
                    positions = faces[source].layout(segment)
                    if positions is None:
                        positions = tuple([0] + [sum(faces[source].advance(c)
                                                     for c in segment[:i])
                                                  for i in range(1, len(segment) + 1)])
                for delta in positions[:-1]:
                    expected.append(fpen + delta)
                    j += 1
                fpen += positions[-1]
            assert j == len(glyphs)
            case_pre = abs(int(pen[1:]) - fpen)
            case_draw = 0
            for c, (origin, x, phase, missing), ref in zip(cp, glyphs, expected, strict=True):
                expected_hits += missing
                glyph_count += 1
                w = list(bitmap.phases(c))
                assert phase < len(w)
                bearing = w[phase][0][0]
                case_pre = max(case_pre, abs(origin - ref))
                rounded = (ref + 32) // 64
                case_draw = max(case_draw, abs(x - bearing - rounded))
            assert (int(pen[1:]) + 32) // 64 == int(width), (index, sample)
            if case_pre > 16 or case_draw > 1:
                sample_errors.append((index, sample, case_pre, case_draw))
            max_pre = max(max_pre, case_pre)
            max_draw = max(max_draw, case_draw)

    _, hot, micros = trace[-1].split()
    result = {"strings": len(samples), "glyphs": glyph_count, "missing": expected_hits,
              "max_26_6_error": max_pre, "max_px_error": max_draw,
              "over_budget": len(sample_errors), "first_errors": sample_errors[:20],
              "buffer_bytes": cache, "hot_glyphs_per_ms": int(hot) * 1000 / int(micros)}
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    commands = p.add_subparsers(required=True, dest="command")
    a = commands.add_parser("instrument")
    a.add_argument("source", type=Path)
    a = commands.add_parser("compiler")
    a.add_argument("first", type=Path)
    a.add_argument("second", type=Path)
    a = commands.add_parser("sdl")
    for arg in ("old", "new", "base", "fonts", "output"):
        a.add_argument(arg, type=Path)
    args = p.parse_args()
    lang = compiler.ROOT / "apps-ipod/lang"
    if args.command == "instrument":
        instrument(args.source)
    elif args.command == "compiler":
        check_compiler(args.first, args.second, lang)
    else:
        samples = SAMPLES + sorted({s for name, s in compiler.lang_strings(lang)
                                   if name in ("chinese-simp.lang", "chinese-trad.lang") and s})
        before = run_simulator(args.old, args.base, None, SAMPLES, args.output / "old")
        trace = run_simulator(args.new, args.base, args.fonts, samples, args.output / "new")
        comparison(args.output / "old/font-probe.bmp", args.output / "new/font-probe.bmp",
                   args.output / "contact-sheet.png")
        result = verify_sdl(trace, samples, args.fonts, lang)
        _, n, us = before[-1].split()
        result["old_hot_glyphs_per_ms"] = int(n) * 1000 / int(us)
        (args.output / "result.json").write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n")
        print(json.dumps(result, ensure_ascii=False))
        assert not result["missing"] and not result["over_budget"], result


if __name__ == "__main__":
    main()
