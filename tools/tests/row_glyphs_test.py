#!/usr/bin/env python3
"""Check page-menu glyph definitions and the actual Scrim icon-cell pixels.

Pass an ipod6g simulator build directory made with build-sim.sh. The simulator
walks menus with animations off and a private disk. It draws each visible
row through the native list renderer, including rows below the viewport.
"""
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FONT = ROOT / 'apps-ipod/fonts/24x24-icons.fnt'
GLYPHS = ROOT / 'apps-ipod/widgets/row_glyphs.c'
RENDER = ROOT / 'apps-ipod/widgets/list_render.c'

# The equalizer band records represent the graphical EQ editor in the settings
# service. They aren't rows that the editor draws through the page-menu painter.
EQ_BAND_RECORDS = {'eq low shelf filter', 'eq high shelf filter'} | {
    f'eq peak filter {n}' for n in range(1, 9)}
RECORDING_ROWS = {'LANG_REC_FORMAT', 'LANG_ENCODER_SETTINGS',
                  'LANG_REC_FREQUENCY', 'LANG_RECORDING_SOURCE',
                  'LANG_REC_CHANNELS', 'LANG_RECORDING_MONO_MODE',
                  'LANG_BITRATE'}


def font_errors():
    data = FONT.read_bytes()
    first, _, count = struct.unpack_from('<III', data, 12)
    table = re.findall(r'\{ (LANG_\w+), 0x([0-9A-F]{4}) \}',
                       GLYPHS.read_text())
    errors = []
    seen = set()
    for name, cp in table:
        if name in seen:
            errors.append(f'{name}: duplicate glyph assignment')
        seen.add(name)
        if not first <= int(cp, 16) < first + count:
            errors.append(f'{name}: U+{cp} is not in {FONT.name}')
    return errors


def read_rows(path):
    return [line.split('\t') for line in path.read_text().splitlines()]


def definitions_errors(build, disk, names):
    dump = disk / 'definitions.tsv'
    env = dict(os.environ, PODBOX_MENU_DUMP=str(dump),
               SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy')
    subprocess.run([str(build / 'rockboxui')], env=env, cwd=disk,
                   check=True, timeout=60)
    labelled = set(re.findall(r'\{\s*(LANG_\w+),\s*"[^"]+"\s*\}',
                              RENDER.read_text()))
    errors = []
    for path, root, _, lang, cfg, glyph, *_ in read_rows(dump):
        name = names.get(int(lang), '')
        if int(glyph) or name in labelled or cfg in EQ_BAND_RECORDS:
            continue
        errors.append(f'{root}{path}: {name or cfg} has no assigned glyph')
    return errors


def render_rows(build, disk, theme, chinese=False):
    rockbox = disk / '.rockbox'
    rockbox.mkdir(parents=True)
    resources = build / 'simdisk/.rockbox'
    for name in ('fonts', 'codepages'):
        shutil.copytree(resources / name, rockbox / name)
    shutil.copyfile(resources / 'tagnavi.config', rockbox / 'tagnavi.config')
    if chinese:
        (rockbox / 'langs').mkdir()
        shutil.copyfile(build / 'apps-ipod/lang/chinese-simp.lng',
                        rockbox / 'langs/chinese-simp.lng')
    (rockbox / 'config.cfg').write_text(
        'start in screen: root\nsettings mode: everything\n'
        'root menu order: settings,database,system_menu,files,plugins\n'
        'backlight timeout: off\nvoice menus: off\nanimations: off\n'
        'scrollbar: off\nshow icons: on\n'
        f'native theme: {theme}\n' +
        ('lang: /.rockbox/langs/chinese-simp.lng\n' if chinese else ''))
    output = disk / 'pixels.tsv'
    env = dict(os.environ, PODBOX_MENU_PIXELS=str(output),
               SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy')
    subprocess.run([str(build / 'rockboxui'), '--root', str(disk),
                    '--nobackground'], env=env, cwd=disk,
                   check=True, timeout=90)
    return read_rows(output)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    build = Path(sys.argv[1]).resolve()
    names = {int(n): name for name, n in re.findall(
        r'(LANG_\w+), /\* (\d+) \*/', (build / 'lang_enum.h').read_text())}
    errors = font_errors()
    with tempfile.TemporaryDirectory(prefix='podbox-row-glyphs-') as tmp:
        disk = Path(tmp)
        errors.extend(definitions_errors(build, disk, names))
        expected_rows = None
        for theme, chinese in (('scrim', False), ('classic', False),
                               ('fruit-company', False), ('scrim', True)):
            case = theme + ('-zh' if chinese else '')
            rows = render_rows(build, disk / case, theme, chinese)
            ink = {}
            recording = set()
            for path, root, _, lang, cfg, _, _, count in rows:
                if cfg in EQ_BAND_RECORDS:
                    continue
                name = names.get(int(lang), '')
                if root == 'settings':
                    recording.add(name)
                ink[(root, path)] = int(count)
                if int(count) <= 0:
                    errors.append(f'{case} {root}{path} ({name or cfg}): '
                                  f'no icon pixels ({count})')
            if not RECORDING_ROWS <= recording:
                errors.append(f'{case}: missing Recording coverage: '
                              f'{sorted(RECORDING_ROWS - recording)}')
            if expected_rows is None:
                expected_rows = ink
            elif not chinese and ink != expected_rows:
                errors.append(f'{case}: live Scrim icon cells differ from cold Scrim')
            print(f'{case}: checked {len(ink)} visible menu rows')
    for error in errors:
        print(error, file=sys.stderr)
    return 1 if errors else 0


if __name__ == '__main__':
    sys.exit(main())
