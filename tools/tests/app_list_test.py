#!/usr/bin/env python3
"""Check the Games, Other Apps and Demos lists against what ships.

Every module bundle-rocks.sh installs is listed or deliberately left out,
every listed app has an English and a Simplified Chinese name and an icon,
and the icon pack holds each icon at the slot size app_list.c reads.
Run from anywhere; silence and exit 0 mean all is well.
"""
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
APP_LIST = ROOT / 'apps-ipod/screens/browse/app_list.c'
ICONS = ROOT / 'apps-ipod/bitmaps/source/apps'
# Opened with a file or by another plugin, or rows of the Extras menu.
LEFT_OUT = {'apps/search', 'apps/file_picker', 'apps/view_text',
            'apps/alarmclock', 'apps/calendar', 'apps/clock',
            'apps/stopwatch'}
# bundle-rocks.sh adds these to the allowlist itself.
CONDITIONAL = {'apps/pitch_detector', 'games/doom'}


def phrases(name):
    text = (ROOT / 'apps-ipod/lang' / name).read_text()
    out = {}
    for block in text.split('<phrase>')[1:]:
        ident = re.search(r'id: (\S+)', block)
        dest = re.search(r'<dest>\s*\*: "([^"]*)"', block)
        if ident:
            out[ident.group(1)] = dest.group(1) if dest else ''
    return out


def main():
    errors = []
    table = re.findall(r'\{ "([^"]+)",\s*(LANG_\w+),\s*(APP_LIST_\w+) \}',
                       APP_LIST.read_text())
    listed = {app for app, _, _ in table}
    modules = {line.strip() for line in
               (ROOT / 'apps-ipod/plugins/rock-modules.txt').read_text()
               .splitlines() if line.strip()} | CONDITIONAL
    for module in sorted(modules - listed - LEFT_OUT):
        if not module.startswith('viewers/'):
            errors.append('%s: installed but in no list' % module)
    for app in sorted(listed - modules - {'native/music_quiz'}):
        errors.append('%s: listed but never installed' % app)

    english, chinese = phrases('english.lang'), phrases('chinese-simp.lang')
    for app, lang, _ in table:
        if not english.get(lang):
            errors.append('%s: %s missing from english.lang' % (app, lang))
        if not chinese.get(lang):
            errors.append('%s: %s missing from chinese-simp.lang' % (app, lang))
        if not (ICONS / (app.replace('/', '__') + '.png')).exists():
            errors.append('%s: no icon in %s' % (app, ICONS))

    with tempfile.TemporaryDirectory() as tmp:
        pack = Path(tmp) / 'app-icons.bin'
        subprocess.run([sys.executable, str(ICONS / 'gen_app_icons.py'),
                        '--pack', str(pack)], check=True)
        data = pack.read_bytes()
    magic, version, count, nsizes = struct.unpack_from('<4sIII', data)
    sizes = struct.unpack_from('<%dI' % nsizes, data, 16)
    at = 16 + 4 * nsizes
    entries = {}
    for n in range(count):
        name = data[at:at + 48].split(b'\0')[0].decode()
        entries[name] = struct.unpack_from('<%dI' % nsizes, data, at + 48)
        at += 48 + 4 * nsizes
    if magic != b'AICN' or version != 1 or '*' not in entries:
        errors.append('app-icons.bin: bad header or no placeholder')
    for app in sorted(listed - set(entries)):
        errors.append('%s: not in app-icons.bin' % app)
    # app_list.c reads slot_bytes(size) from each offset.
    for name, offsets in entries.items():
        for size, offset in zip(sizes, offsets):
            slot = (-(-size * size * 2 // 4) * 4 +
                    -(-((size + 1) // 2 * size) // 4) * 4)
            if offset + slot > len(data):
                errors.append('%s: %d px icon runs past the pack' % (name, size))

    for error in errors:
        print(error)
    sys.exit(1 if errors else 0)


if __name__ == '__main__':
    main()
