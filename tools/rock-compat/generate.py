#!/usr/bin/env python3
"""Build AstraPod plugin source-compatibility headers for a configured target."""
import argparse
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
# Removed hardware, skin paths and per-plugin controls have no write target.
FIXED = set("""
backdrop_file buttonlight_brightness buttonlight_timeout colors_file hw_eq_bands
icon_file invert pitch_mode_semitone pitch_mode_timestretch rec_mic_gain
remote_backlight_on_button_hold remote_backlight_timeout remote_backlight_timeout_plugged
remote_icon_file remote_viewers_icon_file rsbs_file rwps_file sbs_file statusbar
tagcache_db_path touch_mode viewers_icon_file wps_file
""".split())

# Recognized removals. Leave these identifiers undefined so use fails at compile time.
UNSUPPORTED = set("""
CONTEXT_ALARMSCREEN CONTEXT_FM CONTEXT_PITCHSCREEN ACTION_WPS_PITCHSCREEN
ACTION_FM_MENU ACTION_FM_PRESET ACTION_FM_RECORD ACTION_FM_FREEZE ACTION_FM_STOP
ACTION_FM_MODE ACTION_FM_EXIT ACTION_FM_PLAY ACTION_FM_RECORD_DBLPRE ACTION_FM_NEXT_PRESET
ACTION_FM_PREV_PRESET ACTION_REC_LCD ACTION_REC_PAUSE ACTION_REC_NEWFILE ACTION_REC_F2
ACTION_REC_F3 ACTION_PS_INC_SMALL ACTION_PS_INC_BIG ACTION_PS_DEC_SMALL ACTION_PS_DEC_BIG
ACTION_PS_NUDGE_LEFT ACTION_PS_NUDGE_RIGHT ACTION_PS_NUDGE_LEFTOFF ACTION_PS_NUDGE_RIGHTOFF
ACTION_PS_TOGGLE_MODE ACTION_PS_RESET ACTION_PS_EXIT ACTION_PS_SLOWER ACTION_PS_FASTER
ACTION_TOUCH_SHUFFLE ACTION_TOUCH_REPMODE ACTION_TOUCH_MUTE ACTION_TOUCH_SCROLLBAR
ACTION_TOUCH_SCROLLBAR_SET ACTION_TOUCH_SCROLLBAR_END ACTION_TOUCH_VOLUME ACTION_TOUCH_SOFTLOCK
ACTION_TOUCH_SETTING FILE_ATTR_BMARK FILE_ATTR_FMS FILE_ATTR_RFMS FILE_ATTR_WRS FILE_ATTR_RWRS
""".split())


def source(path):
    # Keep quoted literals intact while removing comments.
    tokens = r'''("(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|/\*.*?\*/|//[^\n]*)'''
    return re.sub(tokens, lambda m: ' ' if m[0].startswith('/') else m[0],
                  path.read_text(errors='surrogateescape'), flags=re.S)


def fields(path):
    body = source(path).split('struct user_settings\n{', 1)[1].split('\n};', 1)[0]
    pattern = r'^\s*(int|bool|unsigned char)\s+(\w+)\s*(\[[^\]]+\])?\s*;'
    return {name: (ctype, dim) for ctype, name, dim in re.findall(pattern, body, re.M)}


def settings(root, rows):
    upstream = fields(root/'apps/settings.h')
    native = fields(root/'apps-ipod/settings/settings.h')
    used = set()
    lang_refs = set()
    for path in (root/'apps/plugins').rglob('*'):
        if path.suffix in ('.c', '.h', '.inc'):
            code = re.sub(r'"(?:\\.|[^"\\])*"', ' ', source(path))
            lang_refs.update(re.findall(r'\bLANG_\w+', code))
            used.update(re.findall(r'\brb\s*->\s*global_settings\s*->\s*(\w+)', code))
    known_lang = set(re.findall(r'^\s*id:\s*(\w+)', (root/'apps/lang/english.lang').read_text(), re.M))
    unknown = lang_refs - known_lang - {'LANG_LAST_INDEX_IN_ARRAY'}
    if unknown:
        raise ValueError(f'unknown upstream language references: {sorted(unknown)}')
    # The text viewer documents this member in comments.
    used.add('statusbar')
    decl, copy, flush = [], [], []
    for name in sorted(used):
        if name == 'hw_eq_bands':
            block = re.search(r'struct hw_eq_band\s*\{[^}]+\}\s*hw_eq_bands\[[^\]]+\];',
                              source(root/'apps/settings.h'))
            if not block:
                raise ValueError('review upstream hardware EQ struct')
            decl.extend(['#ifdef AUDIOHW_HAVE_EQ', block[0], '#endif'])
            rows.append((name, 'fixed zero when hardware EQ exists'))
            continue
        if name not in upstream:
            raise ValueError(f'unrecognized upstream declaration for plugin setting {name}')
        ctype, dim = upstream[name]
        decl.append(f'    {ctype} {name}{dim};')
        if name in FIXED:
            if name in native:
                raise ValueError(f'review newly available native {name}')
            rows.append((name, 'fixed empty' if dim else 'fixed zero'))
            continue
        if native.get(name) != (ctype, dim) and not (
                name == 'playback_log' and native.get(name) == ('int', '') and ctype == 'bool'):
            raise ValueError(f'review setting {name}: {upstream[name]} -> {native.get(name)}')
        dst, old, src = (f'{owner}->{name}' for owner in ('view', 'baseline', 'native'))
        expr = {'playback_log': f'({src} == 1)',
                'album_art': f'({src} == AA_PREFER_CACHE ? AA_PREFER_EMBEDDED : {src})'}.get(name, src)
        guard = {'start_in_screen': f' && {dst} >= 0 && {dst} <= GO_TO_MAINMENU + 2',
                 'album_art': f' && {dst} >= AA_OFF && {dst} <= AA_PREFER_IMAGE_FILE'}.get(name, '')
        if name == 'battery_capacity':
            copy.append('#if BATTERY_CAPACITY_INC > 0')
            flush.append('#if BATTERY_CAPACITY_INC > 0')
        if dim:
            copy.extend([f'    _Static_assert(sizeof({dst}) == sizeof({src}), "{name}");',
                         f'    memcpy({dst}, {src}, sizeof({dst}));'])
            flush.extend([f'    if (memcmp({dst}, {old}, sizeof({dst})) &&',
                          f'        memchr({dst}, 0, sizeof({dst})))',
                          f'        memcpy({src}, {dst}, sizeof({dst}));'])
        else:
            copy.append(f'    {dst} = {expr};')
            flush.extend([f'    if ({dst} != {old}{guard})', f'        {src} = {dst};'])
        if name == 'battery_capacity':
            copy.append('#endif')
            flush.append('#endif')
        rows.append((name, f'{expr}; compare before write{guard}'))
    header = ['struct rock_compat_settings {', *decl, '};']
    args = ('struct rock_compat_settings *view, struct rock_compat_settings *baseline, '
            'struct user_settings *native')
    sync = [f'static inline void rock_compat_begin({args})', '{',
            '    *view = (struct rock_compat_settings){0};', *copy, '    *baseline = *view;', '}',
            f'static inline void rock_compat_flush({args})', '{', *flush,
            '    rock_compat_begin(view, baseline, native);', '}']
    return header, sync


def lang_names(path):
    text = path.read_text().split('LANG_LAST_INDEX_IN_ARRAY,')[0]
    pairs = re.findall(r'^\s*(\w+),\s*/\* (\d+) \*/', text, re.M)
    if not pairs or [int(n) for _, n in pairs] != list(range(len(pairs))):
        raise ValueError(f'incomplete genlang enum: {path}')
    return [name for name, _ in pairs]


def language(root, build, features, rows):
    native = lang_names(build/'lang_enum.h')
    native_features = (build/'apps-ipod/genlang-features').read_text().strip()
    profile = features.read_text().strip()
    if features == build/'apps-ipod/genlang-features':
        # Stock ipod6g has these lexical feature groups even on firmware that
        # removed their services. Preserve the strings for unedited sources.
        profile += ':pitchscreen:recording:recording_linein:recording_mic:videoout:utf8proc'
    english = root/'apps/lang/english.lang'
    with tempfile.TemporaryDirectory() as directory:
        prefix = Path(directory)/'lang'
        prefix.mkdir()

        def genlang(catalog, target):
            subprocess.run([root/'tools/genlang', f'-e={catalog}', f'-p={prefix}',
                            '-t=ipod6g:' + target, catalog], check=True)

        genlang(root/'apps-ipod/lang/english.lang', native_features)
        if native != lang_names(prefix.with_name('lang_enum.h')):
            raise ValueError('stale native lang_enum.h: rebuild the native language outputs')
        genlang(english, profile)
        stock = lang_names(prefix.with_name('lang_enum.h'))
        strings = re.findall(r'^    (".*)\\0"$', (prefix/'lang_core.c').read_text(), re.M)
    if len(stock) != len(strings):
        raise ValueError('incomplete target English output')
    extra = []
    native_ids = set(native)
    for name, literal in zip(stock, strings):
        if name in native_ids:
            rows.append((name, 'native'))
        else:
            rows.append((name, 'English fallback'))
            extra.append((name, literal + '"'))
    header = ['enum {', '    ROCK_COMPAT_NATIVE_LANG_COUNT = LANG_LAST_INDEX_IN_ARRAY,']
    for i, (name, _) in enumerate(extra):
        header.append(f'    {name} = ROCK_COMPAT_NATIVE_LANG_COUNT + {i},')
    header.extend([f'    ROCK_COMPAT_LANG_COUNT = ROCK_COMPAT_NATIVE_LANG_COUNT + {len(extra)}',
                   '};', '_Static_assert((int)ROCK_COMPAT_LANG_COUNT < (int)VOICEONLY_DELIMITER, "language ID space");'])
    sync = ['static inline void rock_compat_lang_init(unsigned char **dst, unsigned char **native)',
            '{', '    memcpy(dst, native, ROCK_COMPAT_NATIVE_LANG_COUNT * sizeof(*dst));']
    sync.extend(f'    dst[{name}] = (unsigned char *){literal};' for name, literal in extra)
    sync += ['}', '/* Resolve plugin-only ID2P tokens before calling native widgets. */',
             'static inline const unsigned char *rock_compat_widget_text(',
             '    const unsigned char *text, unsigned char *const *strings)', '{',
             '    uintptr_t id = (uintptr_t)text - (uintptr_t)VIRT_PTR;',
             '    if (id >= ROCK_COMPAT_NATIVE_LANG_COUNT && id < ROCK_COMPAT_LANG_COUNT)',
             '        return strings[id];', '    return text;', '}']
    return header, sync


def identifiers(root, rows):
    # Source recompilation binds common identifiers directly to native values.
    families = [('action.h', 'input/action.h', r'(?:ACTION|CONTEXT)_\w+'),
                ('gui/icon.h', 'draw/icon.h', r'Icon_\w+'),
                ('recorder/icons.h', 'draw/icon_bitmaps.h', r'Icon_\w+'),
                ('appevents.h', 'system/appevents.h', r'\w+_EVENT_\w+'),
                ('filetypes.h', 'files/filetypes.h', r'FILE_ATTR_\w+')]
    pluginlib = source(root/'apps/plugins/lib/pluginlib_actions.h')
    if not re.search(r'PLA_UP\s*=\s*LAST_ACTION_PLACEHOLDER\s*\+\s*1', pluginlib):
        raise ValueError('review pluginlib custom action base')
    rows.extend((name, 'pluginlib native custom range') for name in
                re.findall(r'^\s*(PLA_\w+)\b', pluginlib, re.M))
    for stock, fork, pattern in families:
        def names(path):
            body = re.sub(r'^#if 0\s*\n.*?^#endif', '', source(path), flags=re.M | re.S)
            return set(re.findall(r'^\s*(?:#define\s+)?(' + pattern + r')\b', body, re.M))
        shared = names(root/'apps-ipod'/fork)
        for name in sorted(names(root/'apps'/stock)):
            if name not in shared and name not in UNSUPPORTED:
                raise ValueError(f'unmapped {name} in {stock}')
            rows.append((name, 'native' if name in shared else 'unsupported'))


def action_maps(root):
    """Keep upstream enum order and preprocessor guards in a private namespace."""
    stock = source(root/'apps/action.h')
    result = ['#pragma once', '#include "input/action.h"']
    for family in ('ACTION', 'CONTEXT'):
        last = f'LAST_{family}_PLACEHOLDER'
        blocks = [b for b in re.findall(r'enum\s*\{([^{}]*)\};', stock) if last in b]
        if len(blocks) != 1:
            raise ValueError(f'review upstream {family} enum')
        body = blocks[0]
        pattern = rf'\b({family}_\w+|{last})\b'
        result += ['enum {', re.sub(pattern, r'ROCK_STOCK_\1', body).strip(), '};',
                   f'static inline int rock_compat_stock_{family.lower()}(int code)',
                   '{', '    switch (code) {']
        for line in body.splitlines():
            line = line.strip()
            if not line or line.startswith('#'):
                result.append(line)
                continue
            match = re.fullmatch(r'(\w+)(?:\s*=\s*[^,]+)?,', line)
            if not match:
                raise ValueError(f'review {family} enum entry: {line}')
            name = match[1]
            if name != last and name not in UNSUPPORTED:
                result.append(f'    case ROCK_STOCK_{name}: return {name};')
        result += ['    default: return -1;', '    }', '}']
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=ROOT)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--stock-features', type=Path, help='reference apps/genlang-features')
    args = parser.parse_args()
    build_config = (args.build/'Makefile').read_text()
    if not re.search(r'^export MODELNAME\s*=\s*ipod6g\s*$', build_config, re.M):
        raise ValueError('configure an ipod6g build for the compatibility tables')
    rows = []
    header, sync = settings(args.root, rows)
    features = args.stock_features or args.build/'apps-ipod/genlang-features'
    lang, init = language(args.root, args.build, features, rows)
    identifiers(args.root, rows)
    outputs = {
        'rock_compat_tables.h': ['#pragma once', '#include "settings/settings.h"',
                                 '#include "lang_enum.h"', '#include "input/action.h"', *header, *lang],
        'rock_compat_sync.h': ['#pragma once', '#include "rock_compat_tables.h"',
                               '#include "root_menu.h"', '#include <string.h>', *sync, *init],
        'rock_compat_input.h': action_maps(args.root),
        'mapping.tsv': ['identifier\tpolicy', *('\t'.join(row) for row in rows)],
    }
    args.out.mkdir(parents=True, exist_ok=True)
    for name, lines in outputs.items():
        data = '\n'.join(lines) + '\n'
        path = args.out/name
        if not path.exists() or path.read_text() != data:
            with tempfile.NamedTemporaryFile(mode='w', dir=args.out, delete=False) as tmp:
                tmp.write(data)
            Path(tmp.name).replace(path)


if __name__ == '__main__':
    main()
