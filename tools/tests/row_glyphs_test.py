#!/usr/bin/env python3
"""Check that every row of the page menus draws a glyph of its own.

Argument: a simulator build directory (build-sim-ipod6g), with rockboxui
and lang_enum.h. The simulator writes its menu rows through
PODBOX_MENU_DUMP and exits. A row passes when its definition or
row_glyphs.c gives it a glyph, or when list_render.c's label table names its
label. Every glyph row_glyphs.c names must be in 24x24-icons.fnt. Silence
and exit 0 mean all is well.
"""
import os
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FONT = ROOT / 'apps-ipod/fonts/24x24-icons.fnt'
GLYPHS = ROOT / 'apps-ipod/widgets/row_glyphs.c'
RENDER = ROOT / 'apps-ipod/widgets/list_render.c'

# Rows whose label is made at run time and has no glyph: the Equalizer's
# bands, named by their frequency, and Debug Logs' two status lines.
NO_GLYPH_CFG = {'eq low shelf filter', 'eq high shelf filter'} | {
    f'eq peak filter {n}' for n in range(1, 9)}
NO_GLYPH_ROOTS = {'Debug Logs'}


def lang_ids(build):
    text = (build / 'lang_enum.h').read_text()
    return {int(n): name for name, n in
            re.findall(r'(LANG_\w+|VOICE_\w+), /\* (\d+) \*/', text)}


def font_range():
    data = FONT.read_bytes()
    first, size = struct.unpack_from('<II', data, 12)[0], \
        struct.unpack_from('<I', data, 20)[0]
    return first, first + size - 1


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    build = Path(sys.argv[1]).resolve()
    names = lang_ids(build)
    labelled = set(re.findall(r'\{\s*(LANG_\w+),\s*"[^"]+"\s*\}',
                              RENDER.read_text()))
    errors = []

    first, last = font_range()
    table = re.findall(r'\{ (LANG_\w+), 0x([0-9A-F]{4}) \}',
                       GLYPHS.read_text())
    seen = set()
    for name, cp in table:
        if name in seen:
            errors.append(f'row_glyphs.c lists {name} twice')
        seen.add(name)
        if not first <= int(cp, 16) <= last:
            errors.append(f'{name}: U+{cp} is not in {FONT.name}')

    with tempfile.TemporaryDirectory() as temp:
        dump = Path(temp) / 'rows.tsv'
        env = dict(os.environ, PODBOX_MENU_DUMP=str(dump),
                   SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy')
        subprocess.run([str(build / 'rockboxui')], env=env, cwd=temp,
                       check=True, timeout=60)
        rows = [line.split('\t') for line in
                dump.read_text().splitlines()]

    titles = {}
    for path, root, _, lang, cfg, glyph, _ in rows:
        titles[(root, path)] = names.get(int(lang), '')
    for path, root, kind, lang, cfg, glyph, _ in rows:
        name = names.get(int(lang), '')
        if int(glyph) or name in labelled or cfg in NO_GLYPH_CFG:
            continue
        parts = path.split('/')
        parents = [titles.get((root, '/'.join(parts[:i])), '')
                   for i in range(2, len(parts))]
        if 'LANG_DEBUG_LOG' in parents:
            continue
        errors.append(f'{root}{path}: {name or cfg or "dynamic row"} '
                      'has no glyph')

    for error in errors:
        print(error)
    return 1 if errors else 0


if __name__ == '__main__':
    sys.exit(main())
