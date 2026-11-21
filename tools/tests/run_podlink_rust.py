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
import struct
import subprocess
import zlib
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


def bench_replies(trace):
    """Compare demo frames across USB submission and session owner changes."""
    stream = bytearray()
    pos = 0
    while pos < len(trace):
        tag, length = struct.unpack_from('<II', trace, pos)
        pos += 8
        assert tag == 1 and pos + length <= len(trace)
        stream.extend(trace[pos:pos + length])
        pos += length
    pos = 0
    replies = []
    while pos < len(stream):
        magic, kind, flags, seq, length = struct.unpack_from('<4sHHII', stream, pos)
        end = pos + 16 + length
        assert magic == b'PLNK' and end + 4 <= len(stream)
        assert zlib.crc32(stream[pos + 4:end]) == struct.unpack_from('<I', stream, end)[0]
        data = bytes(stream[pos + 16:end])
        if kind == 3 or (kind == 2 and data != b'DH\t1\tdisk-mode-v1'):
            replies.append((kind, flags, data))
        pos = end + 4
    return replies


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
        assert hashlib.sha256(expected).hexdigest() == manifest[name]['sha256']
        if name == 'podlink_bench':
            assert bench_replies(raw) == bench_replies(expected)
            print('PASS bench golden: identical demo replies with the resident worker', flush=True)
            # A traced serial fixture selects RING for legacy USB trace parity.
            subprocess.run([str(binary)], env={k: v for k, v in os.environ.items()
                           if k != 'PODLINK_TRACE'}, check=True, timeout=120)
            continue
        assert len(raw) == manifest[name]['bytes']
        assert raw == expected, (name, 'golden mismatch', next(
            (i for i, (x, y) in enumerate(zip(raw, expected)) if x != y), len(raw)))
        print(f'PASS C golden {name}: {len(raw)} identical bytes', flush=True)

# Threaded liveness (FU49 round 2): real worker thread, real CDC ring, emulated
# Rockbox mutex/semaphore/IRQ-mask semantics. ILP32 keeps the firmware ABI.
live_lib = library('i686-unknown-linux-gnu')
live = out / 'podlink_liveness'
subprocess.run([os.environ.get('CC', 'cc'), '-m32', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                '-g', '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                '-Itools/tests/podlink_liveness_stubs', '-Itools/tests/podlink_stubs',
                '-Ifirmware/export', '-Ifirmware/usbstack', 'tools/tests/podlink_liveness.c',
                str(live_lib), '-lpthread', '-ldl', '-lm', '-o', str(live)], check=True)
subprocess.run([str(live), os.environ.get('PODLINK_LIVENESS_SECONDS', '20')], check=True, timeout=120)
control = subprocess.run([str(live), '12'], env=os.environ | {'LIVE_FAULT_USB_STALL': '3500'},
                         capture_output=True, text=True, timeout=120)
assert control.returncode == 1 and 'FAIL: host bytes always drained' in control.stdout, control.stdout
print('PASS liveness negative control: a 3.5 s USB-thread stall is detected', flush=True)
