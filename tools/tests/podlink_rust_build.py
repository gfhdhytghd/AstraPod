"""Shared offline Rust ABI builder for C assertion fixtures (no devices)."""
import os
from pathlib import Path
import subprocess
import fcntl

ROOT = Path(__file__).resolve().parents[2]
TOOLCHAIN = 'nightly-2026-02-03'


def library(target=None, research=False):
    build = Path(os.environ.get('PODLINK_TEST_BUILD', ROOT / 'build-fu49')).resolve()
    out = build / ('host-target' if target is None else target)
    if research:
        out = out.with_name(out.name + '-research')
    out.mkdir(parents=True, exist_ok=True)
    # Firmware script runners can build independent fixtures concurrently.
    with (out / 'build.lock').open('w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        command = ['cargo', '+' + TOOLCHAIN, 'build', '--locked', '--manifest-path',
                   str(ROOT / 'firmware/usbstack/podlink-rust/Cargo.toml'),
                   '--release', '--features', 'host-test,research' if research else 'host-test',
                   '--target-dir', str(out)]
        if target:
            command += ['--target', target, '-Zbuild-std=core']
        subprocess.run(command, check=True)
    return out / (target or '') / 'release/libpodlink_rust.a'


def wire_args(target=None):
    """Link Rust wire/CRC ABI; unused worker sections are garbage collected."""
    lib = library(target)
    panic = lib.parent / 'panic.c'
    panic.write_text('#include <stdlib.h>\nvoid pfu_panic(void){abort();}\n')
    return [str(panic), str(lib), '-ffunction-sections', '-fdata-sections',
            '-Wl,--gc-sections', '-lpthread', '-ldl', '-lm']


def shared_wire(output):
    lib = library()
    exports = lib.parent / 'wire.exports'
    exports.write_text('{global: podlink_encode; podlink_decode; podlink_decode_bytes; podlink_crc32; local: *;};\n')
    subprocess.run(['cc', '-shared', '-fPIC', '-Wl,--gc-sections',
                    '-Wl,--version-script=' + str(exports),
                    '-Wl,--undefined=podlink_encode', '-Wl,--undefined=podlink_decode',
                    '-Wl,--undefined=podlink_decode_bytes', '-Wl,--undefined=podlink_crc32',
                    *wire_args(), '-o', str(output)], check=True)
