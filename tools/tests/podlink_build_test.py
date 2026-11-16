#!/usr/bin/env python3
"""Configure's Rust requirement, target matrix and early failure contracts."""
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
out = Path(sys.argv[1]).resolve()
out.mkdir(parents=True, exist_ok=True)
# Exercise missing cargo without hiding the POSIX tools used by configure.
missing = out / 'without-rust'
missing.mkdir(exist_ok=True)
for tool in Path('/usr/bin').iterdir():
    if tool.name not in ['cargo', 'rustc', 'rustup']:
        link = missing / tool.name
        if not link.exists() and not link.is_symlink():
            link.symlink_to(tool)


def fail_early(name, path, message):
    build = out / name
    build.mkdir(exist_ok=True)
    env = os.environ | {'PATH': path, 'TMPDIR': str(out)}
    result = subprocess.run([str(ROOT / 'tools/configure'), '--target=ipod6g',
                             '--type=n', '--appsdir=apps-ipod'], cwd=build,
                            env=env, capture_output=True, text=True)
    (build / 'result.log').write_text(result.stdout + result.stderr)
    assert result.returncode == 10, (name, result.returncode, result.stderr)
    assert message in result.stderr, result.stderr
    assert not (build / 'Makefile').exists()
    assert not (build / 'autoconf.h').exists()


fail_early('missing-cargo', str(missing), 'requires Cargo and Rust nightly-2026-02-03')
fake = out / 'fake-toolchain'
fake.mkdir(exist_ok=True)
for name, body in [('cargo', 'exit 1'), ('rustc', 'exit 1')]:
    p = fake / name
    p.write_text('#!/bin/sh\n' + body + '\n')
    p.chmod(0o755)
fail_early('missing-pinned', str(fake) + ':' + os.environ['PATH'], 'with rust-src')
(fake / 'cargo').write_text('#!/bin/sh\nexit 0\n')
(fake / 'rustc').write_text('#!/bin/sh\necho ' + str(out / 'empty-sysroot') + '\n')
fail_early('missing-src', str(fake) + ':' + os.environ['PATH'], 'with rust-src')
# Check the public feature macro as seen by real firmware/host preprocessors.
config = ROOT / 'firmware/export/config/ipod6g.h'
for kind, defines in [('normal', []), ('bootloader', ['BOOTLOADER']),
                      ('simulator', ['SIMULATOR']), ('checkwps', ['CHECKWPS']),
                      ('database', ['__PCTOOL__']), ('application', ['APPLICATION'])]:
    command = ['cc', '-E', '-dM', '-x', 'c', *['-D' + d for d in defines], str(config)]
    macros = subprocess.check_output(command, text=True)
    enabled = '#define HAVE_USB_PODLINK' in macros
    assert enabled == (kind == 'normal'), (kind, enabled)
print('PASS: missing Cargo/pinned nightly/rust-src fail before generated files; normal/boot/sim/checkwps/database/application feature matrix')
