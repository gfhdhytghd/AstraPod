#!/usr/bin/env python3
"""FU88 research build checks: shell unit tests, the PodLink round trip
through the real Rust service, host podsh tests and, with --elf, the ARM
crash-capture emulation and the IRAM1 audit. No device is accessed."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

from podlink_rust_build import ROOT, library

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--build', type=Path, default=ROOT / 'build-fu88/tests/research')
p.add_argument('--elf', type=Path, help='research rockbox.elf for the ARM checks')
a = p.parse_args()
os.chdir(ROOT)
out = a.build.resolve()
out.mkdir(parents=True, exist_ok=True)
cc = [os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-g',
      '-fsanitize=address,undefined', '-Ifirmware/export', '-Itools/tests']


def run(command, **kw):
    print('+', ' '.join(map(str, command)), flush=True)
    subprocess.run(command, check=True, timeout=300, **kw)


files = out / 'files'
shutil.rmtree(files, ignore_errors=True)
files.mkdir()
env = os.environ | {'RS_TEST_DIR': str(files)}

# 1. Parser, guard list and commands against the RAM-only port
run(cc + ['firmware/research_shell.c', 'tools/tests/research_shell_test.c',
          '-o', str(out / 'research_shell_test')])
run([str(out / 'research_shell_test')], env=env)

# 2. Host frame -> Rust PodLink service -> shell -> reply frame
os.environ['PODLINK_TEST_BUILD'] = str(out / 'podlink')
lib = library(research=True)
run(cc[:6] + ['-Itools/tests/podlink_stubs'] + cc[6:] + [
          '-fno-pie', '-no-pie', '-ffunction-sections', '-fdata-sections',
          '-Wl,--gc-sections', '-DFU88_PROTOCOL_TEST', '-Ifirmware/usbstack', '-Iapps-ipod/screens/system',
          'tools/tests/podlink_resident_test.c', 'firmware/research_shell.c',
          'firmware/usbstack/remote_log.c', 'apps-ipod/screens/system/herdremote_model.c',
          str(lib), '-lpthread', '-ldl', '-lm', '-o', str(out / 'research_protocol')])
shutil.rmtree(files, ignore_errors=True)
files.mkdir()
run([str(out / 'research_protocol')], env=env)

# 3. podsh with a fake transport
run([sys.executable, '-m', 'pytest', '-q', 'tools/tests/test_podsh.py'])

# 4. ARM: crash capture, reset and export; nothing writes IRAM1
if a.elf:
    run([sys.executable, 'tools/tests/arm_fu88_crash.py', '--elf', str(a.elf)])
print('PASS FU88 research checks')
