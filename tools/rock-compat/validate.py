#!/usr/bin/env python3
"""Reproduce the design study's header-only probe against the current source tree."""
import argparse
import concurrent.futures
import json
import re
import shlex
import shutil
import subprocess
from pathlib import Path

from generate import ROOT


def run(flags, code, options):
    result = subprocess.run([*flags, *options, '-x', 'c', '-'], input=code,
                            capture_output=True, text=True)
    if result.returncode:
        raise ValueError(result.stderr)
    return result.stdout


def button_check(stock, native):
    header = '#include "button.h"\n'
    macros = run(stock, header, ['-dM', '-E'])
    names = sorted(re.findall(r'^#define (BUTTON_\w+)\s+\S', macros, re.M))
    expressions = run(stock, header + '\n'.join(f'RC_{n} {n}' for n in names), ['-E', '-P'])
    values = dict(re.findall(r'^RC_(BUTTON_\w+) (.+)$', expressions, re.M))
    if set(values) != set(names) or 'BUTTON_REMOTE' not in values:
        raise ValueError('incomplete button probe')
    # These five input bits joined the remote aggregate in AstraPod.
    values['BUTTON_REMOTE'] += (' | BUTTON_RC_MENU | BUTTON_RC_NEXT_ALBUM | BUTTON_RC_PREV_ALBUM'
                                ' | BUTTON_RC_NEXT_PLAYLIST | BUTTON_RC_PREV_PLAYLIST')
    checks = [f'_Static_assert({name} == ({value}), "{name}");' for name, value in values.items()]
    run(native, header + '\n'.join(checks), ['-fsyntax-only'])
    return len(checks)


def validate(study, out):
    stock = study/'stock'
    flags = shlex.split((study/'sdk-flags.txt').read_text())[1:]
    flags = [x.replace(str(study/'fork') + '/', str(ROOT) + '/') for x in flags]
    manifest_candidates = re.findall(r"^[\w/]+\.c$", (ROOT/"apps/plugins/SOURCES").read_text(), re.M) + ["lib/pluginlib_actions.c"]
    commands = {}
    for line in (study/'compile-rocks.log').read_text().splitlines():
        if ' -c ' not in line or '/stock/apps/plugins/' not in line:
            continue
        if not any(name in line for name in manifest_candidates):
            continue
        args = shlex.split(line)
        for arg in args:
            if arg.startswith(str(stock/'apps/plugins') + '/') and arg.endswith('.c'):
                commands[Path(arg).relative_to(stock/'apps/plugins').as_posix()] = args
    stockflags = [x for x in commands['snow.c'] if x.startswith(('-D', '-I', '-U', '-mcpu='))]
    stockflags = [flags[0], '-ffreestanding', *stockflags]
    manifest = run(stockflags, (ROOT/'apps/plugins/SOURCES').read_text(),
                   ['-E', '-P', '-imacros', 'config.h', '-imacros', 'button.h']).split()
    probe = out/'probe'
    probe.mkdir(exist_ok=True)
    for wrapper in (study/'sdk').glob('*.h'):
        shutil.copyfile(wrapper, probe/wrapper.name)
    # Change just the type of the prototype SDK's settings slot.
    stub = probe/'plugin.h'
    before = 'struct user_settings* global_settings;'
    template = stub.read_text()
    if template.count(before) != 1:
        raise ValueError('review stub settings slot')
    stub.write_text(template.replace('struct plugin_api {',
                                     '#include "rock_compat_tables.h"\nstruct plugin_api {')
                    .replace(before, 'struct rock_compat_settings* global_settings;'))
    plugins = ROOT/'apps/plugins'
    sdkflags = [flags[0], f'-I{probe}', f'-I{out}', f'-I{plugins}', f'-I{plugins}/lib',
                f'-I{study}/stock-build/apps/plugins', *flags[1:], f'-idirafter{study}/stock-build', '-DPLUGIN']
    run(flags + [f'-I{out}'], '#include "rock_compat_sync.h"\n', ['-fsyntax-only'])
    buttons = button_check(stockflags, flags)
    firmware = study/'fork-build/firmware'
    objects = [firmware/name for name in ('asm/arm/memcpy.o', 'asm/arm/memset.o',
                                          'libc/memchr.o', 'libc/memcmp.o')]
    test = probe/'tables.elf'
    subprocess.run([*flags, f'-I{out}', '-Wl,-e,_start', '-o', test,
                    ROOT/'tools/rock-compat/probe.c', *objects], check=True)
    subprocess.run(['qemu-arm', test], check=True)

    def compile_one(name):
        extras = []
        for x in commands[name]:
            if x.startswith('-I' + str(stock/'apps/plugins')):
                extras.append(x.replace(str(stock), str(ROOT), 1))
            elif x.startswith('-I' + str(study/'stock-build/apps/plugins')):
                extras.append(x)
            elif x.startswith('-D') and x not in stockflags:
                extras.append(x)
        result = subprocess.run([*sdkflags, *extras, '-fsyntax-only',
                                 '-Werror=implicit-function-declaration',
                                 '-Werror=incompatible-pointer-types', '-Werror=int-conversion',
                                 plugins/name], capture_output=True, text=True)
        return name, dict(status=result.returncode, errors=[line for line in result.stderr.splitlines()
                                                           if 'error:' in line])

    entries = [n for n in manifest if n.endswith('.c')]
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as executor:
        results = dict(executor.map(compile_one, entries + ['lib/pluginlib_actions.c']))
    diagnostics = re.compile(r'\b(?:LANG_|ACTION_|CONTEXT_|PLA_)\w+|rock_compat_settings')
    table_errors = [name for name in entries if any(diagnostics.search(line.split('error:', 1)[1])
                                                   for line in results[name]['errors'])]
    summary = dict(button_masks=buttons, manifest=manifest, table_errors=table_errors, results=results)
    (out/'validation.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(f'{len(manifest)} manifest entries, {len(entries)} C, '
          f'{sum(results[n]["status"] == 0 for n in entries)} C syntax passed; '
          f'{len(entries) - len(table_errors)} without table diagnostics; '
          f'{buttons} stock button values checked (BUTTON_REMOTE is a native superset)')
    for n, result in results.items():
        if result['status']:
            print(n, *result['errors'][:2], sep='\n  ')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--study', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    validate(args.study.resolve(), args.out.resolve())
