#!/usr/bin/env python3
"""FU50 freeze watch: host decision test and source contract; no device.

Builds tools/tests/hang_watch_test.c against the firmware's own
hang_watch.h with ASan/UBSan and runs it. Then checks the paths that must
never count as a freeze mask the FIQ (panic/HANG screens, ROLO, reboot), that
power-off/reboot disarm it, and that the scheduler notes the UI switch-in.
The FIQ code itself runs under Unicorn in arm_freeze_watch.py (needs an ELF).
"""
import os
import re
import subprocess
import sys
from pathlib import Path

R = Path(__file__).resolve().parents[2]
O = Path(sys.argv[1] if len(sys.argv) > 1 else R / 'build-fu50/tests').resolve()
O.mkdir(parents=True, exist_ok=True)
cc = os.environ.get('CC', 'gcc')
exe = O / 'hang_watch_test'
subprocess.run([cc, '-std=c99', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
                '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                '-I', str(R / 'firmware/kernel/include'),
                str(R / 'tools/tests/hang_watch_test.c'), '-o', str(exe)],
               check=True)
out = subprocess.run([str(exe)], check=True, capture_output=True, text=True)
assert 'hang_watch_test: ok' in out.stdout, out.stdout

def src(p):
    return (R / p).read_text()

def body(text, name):
    m = re.search(r'\n[^\n;]*\b' + name + r'\s*\([^;{]*\)\s*\{', text)
    assert m, name
    i, depth = m.end(), 1
    while depth:
        depth += {'{': 1, '}': -1}.get(text[i], 0)
        i += 1
    return text[m.end():i]

panic = body(src('firmware/panic.c'), 'panic_screen_enter')
assert 'disable_interrupt(IRQ_FIQ_STATUS)' in panic
assert 'disable_interrupt(IRQ_FIQ_STATUS)' in src('firmware/rolo.c')
system = src('firmware/target/arm/s5l8702/system-s5l8702.c')
assert '#0xd3' in body(system, 'system_reboot')
hang = src('firmware/kernel/hang_catch.c')
assert 'panic_screen_enter();' in body(hang, 'hang_show')
assert 'panic_screen_enter();' in body(hang, 'hang_catch_fiq_show')
assert 'hang_watch_fiq_stop();' in body(hang, 'hang_catch_stop')
assert 'hang_watch_fiq_start();' in body(hang, 'hang_catch_init')
power = src('firmware/powermgmt.c')
assert power.count('hang_catch_stop();') >= 2
assert 'hang_ui_ran = current_tick' in body(src('firmware/kernel/thread.c'),
                                            'switch_thread')
# The shared timer line: E/F/H interrupts must reach their IRQ handlers.
assert 'VIC0INTSELECT &= ~TIMER32_LINE' in body(system, 'hang_fiq_timer')
assert 'default_interrupt(INT_TIMERG)' in system  # kept for non-FIQ builds
print('fu50_freeze_watch_test: ok')
