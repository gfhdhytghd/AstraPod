#!/usr/bin/env python3
"""Run the complete Rust C-ABI assertion streams against frozen FU49 C goldens.

No C protocol implementation is built. Fixtures never access a device.
"""
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import subprocess
from podlink_rust_build import ROOT, library

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--build', type=Path, default=ROOT / 'build-fu49')
a = p.parse_args()
os.chdir(ROOT)
b = a.build.resolve()
os.environ['PODLINK_TEST_BUILD'] = str(b)
out = b / 'host'
out.mkdir(parents=True, exist_ok=True)
lib = library()
goldens = ROOT / 'tools/tests/fixtures/podlink-golden'
manifest = json.loads((goldens / 'traces.json').read_text())
flags = [os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra', '-Werror',
         '-g', '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
         '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
         '-Itools/tests/podlink_stubs', '-Ifirmware/export', '-Ifirmware/usbstack',
         '-Iapps-ipod/screens/system']
for name in ['podlink_firmware', 'podlink_wire', 'podlink_bench', 'podlink_resident',
             'fu47_podlink_dispatch', 'podlink_screen', 'podlink_rust_fuzz',
             'podlink_registry_race']:
    source = ROOT / 'tools/tests' / (name + ('.c' if name in
             ['podlink_rust_fuzz', 'podlink_registry_race'] else '_test.c'))
    command = flags + [str(source)]
    if name in ['podlink_wire']:
        panic = out / 'panic.c'
        panic.write_text('#include <stdlib.h>\nvoid pfu_panic(void){abort();}\n')
        command += [str(panic)]
    if name != 'podlink_screen':
        command += ([] if name == 'podlink_registry_race' else
                    ['firmware/usbstack/remote_log.c', 'apps-ipod/screens/system/herdremote_model.c'])
        command += [str(lib),
                    '-lpthread', '-ldl', '-lm']
    if name == 'podlink_rust_fuzz':
        command += ['-lcrypto']
    binary = out / name
    subprocess.run(command + ['-o', str(binary)], check=True)
    trace = out / (name + '.trace')
    trace.unlink(missing_ok=True)
    subprocess.run([str(binary)] + ([str(trace)] if name == 'podlink_rust_fuzz' else []),
                   env=os.environ | {'PODLINK_TRACE': str(trace)}, check=True, timeout=120)
    if name in manifest:
        raw = trace.read_bytes()
        expected = gzip.decompress((goldens / (name + '.trace.gz')).read_bytes())
        assert len(raw) == manifest[name]['bytes']
        assert hashlib.sha256(expected).hexdigest() == manifest[name]['sha256']
        assert raw == expected, (name, 'golden mismatch', next(
            (i for i, (x, y) in enumerate(zip(raw, expected)) if x != y), len(raw)))
        print(f'PASS C golden {name}: {len(raw)} identical bytes', flush=True)
