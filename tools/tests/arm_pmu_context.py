#!/usr/bin/env python3
"""Run real ARM fatal/PMU paths in SYS, IRQ and SVC modes, without MMIO.

Rendering, raw I2C, mutex entry and the terminal wait are adapters. CPU
frequency, PMU wrappers and i2c_write execute from the ELF. --baseline expects
the old unsafe PMU calls; it does not reconstruct the user's initial fault.
"""
import argparse
import json
import struct
from pathlib import Path

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm_const import (UC_ARM_REG_CPSR, UC_ARM_REG_PC, UC_ARM_REG_SP,
                               UC_ARM_REG_LR, UC_ARM_REG_R0, UC_ARM_REG_R1,
                               UC_ARM_REG_R2, UC_ARM_REG_R3)

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--elf', type=Path, required=True)
p.add_argument('--baseline', action='store_true')
a = p.parse_args()
with a.elf.open('rb') as f:
    elf = ELFFile(f)
    segments = [(s['p_vaddr'], s.data()) for s in elf.iter_segments()
                if s['p_type'] == 'PT_LOAD']
    symbols = {s.name: s['st_value'] for s in
               elf.get_section_by_name('.symtab').iter_symbols() if s.name}

STOP, TEXT, TOP = 0x10001000, 0x10002000, 0x10010000


def run(entry, mode):
    u = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    u.mem_map(0, 0x10000)
    u.mem_map(0x08000000, 0x4000000)
    u.mem_map(0x10000000, 0x20000)
    u.mem_map(0x48000000, 0x4000000)  # FU51 crumb rings, uncached
    u.mem_map(0x62000000, 0x40000)
    for addr, data in segments:
        u.mem_write(addr, data)
    u.reg_write(UC_ARM_REG_CPSR, mode | 0x40)  # IRQ initially enabled
    u.reg_write(UC_ARM_REG_SP, TOP)
    u.reg_write(UC_ARM_REG_LR, STOP)
    u.mem_write(TEXT, b'FU46 original fault\0')
    u.reg_write(UC_ARM_REG_R0, TEXT if entry == 'panicf' else 0x28)
    u.reg_write(UC_ARM_REG_R1, 7)
    if entry == 'pmu_write_multiple':
        u.reg_write(UC_ARM_REG_R1, 1)
        u.reg_write(UC_ARM_REG_R2, TEXT)
    u.mem_write(symbols['cpu_frequency'], struct.pack('<I', 216000000))
    calls, formats, diagnostics = [], [], []
    ended = False

    def ret(value=0):
        u.reg_write(UC_ARM_REG_R0, value)
        u.reg_write(UC_ARM_REG_PC, u.reg_read(UC_ARM_REG_LR))

    def cstr(addr):
        return bytes(u.mem_read(addr, 128)).split(b'\0', 1)[0].decode()

    adapters = {symbols[n]: n for n in [
        'vsnprintf', 'strlen', 'lcd_set_viewport', 'lcd_set_backdrop',
        'lcd_set_drawinfo', 'lcd_clear_display', 'lcd_setfont', 'lcd_puts',
        'lcd_putsf', 'lcd_update', 'rb_backtrace', 'hang_log', 'stuck_log',
        'stuck_draw', 'system_exception_wait', 'system_reboot', 'i2c_write',
        'mutex_lock', 'ide_power_enable',
        'set_clocking_level',
        'usb_log', 'usb_log_sync',
        'i2c_wr', 'mutex_unlock',
    ] if n in symbols}

    def hook(uc, address, size, _):
        nonlocal ended
        if address == STOP:
            ended = True
            uc.emu_stop()
            return
        if address == symbols['panicf']:
            fmt = cstr(uc.reg_read(UC_ARM_REG_R0))
            formats.append(fmt)
            if fmt.startswith('PMU write'):
                diagnostics.append(dict(reg=uc.reg_read(UC_ARM_REG_R1),
                                        count=uc.reg_read(UC_ARM_REG_R2),
                                        mode=uc.reg_read(UC_ARM_REG_R3),
                                        caller=struct.unpack('<I', uc.mem_read(
                                            uc.reg_read(UC_ARM_REG_SP), 4))[0]))
        n = adapters.get(address)
        if not n:
            return
        calls.append(n)
        r0, r1, r2 = [uc.reg_read(r) for r in
                      [UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2]]
        if n == 'mutex_lock' and entry.startswith('pmu_') and mode == 0x1f:
            ret()
        elif n in ('system_exception_wait', 'system_reboot', 'mutex_lock',
                 'ide_power_enable'):
            ended = True
            uc.emu_stop()
        elif n == 'i2c_write':
            # Observe this boundary, then execute its real mutex call.
            pass
        elif n == 'vsnprintf':
            data = cstr(r2).encode()[:r1 - 1]
            uc.mem_write(r0, data + b'\0')
            ret(len(data))
        elif n == 'strlen':
            ret(len(cstr(r0)))
        else:
            ret()

    u.hook_add(UC_HOOK_CODE, hook)
    try:
        u.emu_start(symbols[entry], STOP + 4, count=1000000)
    except Exception as error:
        raise RuntimeError((entry, hex(mode), hex(u.reg_read(UC_ARM_REG_PC)),
                            calls, formats)) from error
    assert ended, (entry, mode, hex(u.reg_read(UC_ARM_REG_PC)))
    writes = calls.count('i2c_write')
    if entry in ('panicf', 'hang_show'):
        assert writes == int(a.baseline), (entry, mode, calls)
        assert calls.count('mutex_lock') == int(a.baseline), calls
        if entry == 'panicf':
            assert formats == ['FU46 original fault'], formats
    elif mode == 0x1f:
        assert writes == 1 and not formats, (entry, mode, calls, formats)
        assert calls.count('mutex_lock') == 1, calls
    else:
        assert writes == int(a.baseline), (entry, mode, calls)
        assert calls.count('mutex_lock') == int(a.baseline), calls
        if not a.baseline:
            assert diagnostics == [dict(reg=0x28, count=1, mode=mode,
                                        caller=STOP)], diagnostics
    return dict(entry=entry, mode=hex(mode), i2c_writes=writes,
                mutex_entries=calls.count('mutex_lock'),
                diagnostics=diagnostics, formats=formats)


rows = [run(entry, mode) for entry in ('panicf', 'pmu_write', 'pmu_write_multiple')
        for mode in (0x1f, 0x12, 0x13)]
rows.append(run('hang_show', 0x1f))
print(json.dumps(dict(elf=str(a.elf), baseline=a.baseline, cases=rows,
                     scope='ARM instructions with rendering/raw-I2C/mutex-entry adapters; '
                           'no original-trigger or physical-device reproduction'), indent=2))
