#!/usr/bin/env python3
"""FU51 breadcrumbs, guards and bounded USB waits: host ring test and source
contract; no device.

Builds tools/tests/crumb_ring_test.c against the firmware's own
crumb_ring.h with ASan/UBSan and runs it. Then checks the wiring: the ring
is taken before anything writes and reported once the disk is up; intended
power off, reboot and ROLO mark the ring clean, panics/HANG/FREEZE record
evidence before drawing; the DesignWare register waits in the USB interrupt
are bounded; the breadcrumb status pointers target retained DRAM, and
.persist is NOLOAD and asserted clear of the bootloader.
Null page: IRAM starts at 0x1000, the main stack is in DRAM, crt0 clears
c1.V, ROLO restores low vectors, the exception screen never scans page 0,
and an unloaded skin (NULL buffer) never resolves offsets against NULL.
The ARM side (MMU guard, tick guards, boot snapshot) is arm_crumbs_guard.py.
"""
import os
import re
import subprocess
import sys
from pathlib import Path

R = Path(__file__).resolve().parents[2]
O = Path(sys.argv[1] if len(sys.argv) > 1 else R / 'build-fu51/tests').resolve()
O.mkdir(parents=True, exist_ok=True)
cc = os.environ.get('CC', 'gcc')
exe = O / 'crumb_ring_test'
subprocess.run([cc, '-std=c99', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
                '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                '-I', str(R / 'firmware/kernel/include'),
                str(R / 'tools/tests/crumb_ring_test.c'), '-o', str(exe)],
               check=True)
out = subprocess.run([str(exe)], check=True, capture_output=True, text=True)
assert 'crumb_ring_test: ok' in out.stdout, out.stdout

def src(p):
    return (R / p).read_text()

def body(text, name):
    m = re.search(r'\n(?![ \t]*/?\*)[^\n;]*\b' + name +
                  r'\s*\([^;{)]*\)\s*\{', text)
    assert m, name
    i, depth = m.end(), 1
    while depth:
        depth += {'{': 1, '}': -1}.get(text[i], 0)
        i += 1
    return text[m.end():i]

crumbs = src('firmware/kernel/crumbs.c')
crumbs_h = src('firmware/export/crumbs.h')
assert 'IRAM1_RING_ADDR' not in crumbs
assert 'crumb_status_a = RING_A->status;' in crumbs
assert 'crumb_status_extra_a = RING_A->status_extra;' in crumbs
assert 'crumb_status_b' not in crumbs_h and 'crumb_status_extra_b' not in crumbs_h
assert 'crumb_crash_latest(CRASH_A, CRASH_EARLY)' in body(crumbs, 'crash_report')

main = body(src('apps-ipod/main.c'), 'init')
assert main.index('crumbs_boot();') < main.index('system_init();')
assert main.index('kernel_init();') < main.index('crumbs_guard_init();')
assert main.index('disk_mount_all()') < main.index('crumbs_report();') < \
    main.index('pcm_init();')
hang = src('firmware/kernel/hang_catch.c')
assert 'crumbs_clean();' in body(hang, 'hang_catch_stop')
show = body(hang, 'hang_catch_fiq_show')
assert show.index('crumb(CR_FREEZE') < show.index('panic_screen_enter();')
assert 'crumb(CR_FREEZE_DRAWN' in show
hshow = hang[hang.index('static void hang_show_detail(void)\n'):
             hang.index('static void hang_tick(void)')]
assert hshow.index('crumb(CR_HANG,') < hshow.index('crumbs_crash_save(')
assert hshow.index('crumbs_crash_save(') < hshow.index('lcd_set_viewport(')
rolo = src('firmware/rolo.c')
assert rolo.index('crumbs_clean();') < rolo.index('rolo_restart(filebuf')
panic = src('firmware/panic.c')
assert panic.index('crumb(CR_PANIC,') < panic.index('    panic_screen_enter();\n\n    va_start')
assert 'crumb(CR_PANIC_DRAWN' in panic

# USB interrupt: no unbounded register wait left
dw = src('firmware/drivers/usb-designware.c')
assert 'while (!(DWC_GRSTCTL & AHBIDL));' not in dw
assert not re.search(r'while \(DWC_GRSTCTL & fflsh\);', dw)
assert 'USB_STAGE_AHB_IDLE' in body(dw, 'usb_dw_wait_for_ahb_idle')
assert 'usb_dw_wait_bits(&DWC_GRSTCTL, fflsh, 0' in body(dw, 'usb_dw_flush_fifo')
waits = body(dw, 'usb_dw_wait_bits')
assert 'USB_DW_WAIT_US' in waits and 'crumb(CR_USB_TIMEOUT' in waits
assert 'crumb(CR_USB_SETUP' in body(dw, 'usb_dw_handle_setup_received')
assert 'usb_dw_check_dma(' in body(dw, 'usb_dw_epstart')
irq = body(dw, 'usb_dw_irq')
assert 'CS_USB_GINTSTS' in irq and 'crumb(CR_USB_RESET' in irq

system = src('firmware/target/arm/s5l8702/system-s5l8702.c')
assert '!defined(HAVE_CRUMBS)' in body(system, 'system_init')
assert 'IRQ_CRUMB(current_irq, 0);' in body(system, 'irq_handler')
assert 'crumb(CR_RESTART' in body(system, 'null_call_report')
lds = src('firmware/target/arm/s5l8702/app.lds')
assert re.search(r'\.persist \(NOLOAD\)', lds)
assert 'ASSERT(_persistend <= DRAMORIG + 0x800000' in lds
assert '_icodestart = .;' in lds and '_icodeend = .;' in lds
assert '.iram 0x1000 :' in lds and '.mainstack (NOLOAD)' in lds
stack = lds[lds.index('.stack (NOLOAD)'):lds.index('.bss (NOLOAD)')]
assert 'stackbegin = .;' not in stack.replace('_irqstackbegin', '').replace('_fiqstackbegin', '')
crt0 = src('firmware/target/arm/s5l8702/crt0.S')
assert 'bic     r0, r0, #0x2000' in crt0
assert rolo.index('crumbs_guard_off();') < rolo.index('rolo_restart(filebuf')
uie = src('firmware/target/arm/system-arm-classic.c')
assert 'saved_sp >= 0x1000u' in uie
boot = src('firmware/target/arm/s5l8702/boot.lds')
assert '#define MAX_LOADSIZE 8M' in boot and 'ORIGIN = DRAM_ORIG + MAX_LOADSIZE' in boot

assert 'crumb_snapshot_take(RING_A, RING_A, &crumbs_prev)' in body(crumbs, 'crumbs_boot')
assert 'CLOCKGATE_SM1' not in body(crumbs, 'crumbs_boot')
assert 'tick_add_task(guard_tick);' in body(crumbs, 'crumbs_guard_init')
gm = body(crumbs, 'guard_mmu')
assert 'c1 |= 1u << 13;' in gm and 'i == 0 ? 0 :' not in gm
# FU53: page 0 is written (zero page) only after c1.V, and before the remap
assert gm.index('c1 |= 1u << 13;') < gm.index('page0[i] = low_word(i);') < \
    gm.index('l1[0] = (uint32_t)l2tab[0]')
off = body(crumbs, 'crumbs_guard_off')
assert 'c1 &= ~(1u << 13);' in off
assert off.index('l1[0] = low_sect;') < off.index('page0[i] = low_ref[i] = vec_ref[i];') < \
    off.index('c1 &= ~(1u << 13);')
assert crumbs.count('mov %0, #0') == 2      # those two only; nothing reads VA 0
plat = src('firmware/usbstack/podlink_rust_platform.c')
assert 'crumb(CR_RUST_PANIC' in body(plat, 'pfu_panic')
assert 'CS_PL_LOCK' in body(plat, 'pfu_lock') and 'CS_PL_LOCK' in body(plat, 'pfu_unlock')
dbg = src('apps-ipod/screens/system/debug_menu.c')
for t in ['Freeze test 1: IRQ spin', 'Freeze test 2: IRQs off',
          'Freeze test 3: busy thread', 'Freeze test 4: all masked',
          'Last boot crumbs']:
    assert '"' + t + '"' in dbg, t
print('fu51_crumbs_test: ok')
