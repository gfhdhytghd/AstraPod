#!/usr/bin/env python3
"""ARM low-IRAM trap and crt0 re-entry guard (FU47).

Executes the ELF's real vector-0 trap, its save path, null_call_report and
crt0's entry guard under Unicorn. The low-IRAM words are written exactly as
system_init does (checked against its literal pool). Adapters: the CP15 c1
read (so the test chooses MMU on/off), memory_init, thread_current_name and
panicf (arguments recorded). No scheduler, LCD or hardware access.

--baseline expects the FU46 behavior (no crt0 guard, no reset label).
"""
import argparse
import json
import struct
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm_const import (UC_ARM_REG_CPSR, UC_ARM_REG_PC, UC_ARM_REG_SP,
                               UC_ARM_REG_LR, UC_ARM_REG_R0, UC_ARM_REG_R1,
                               UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_R12)

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--elf', type=Path, required=True)
p.add_argument('--baseline', action='store_true')
a = p.parse_args()
with a.elf.open('rb') as f:
    e = ELFFile(f)
    segments = [(s['p_vaddr'], s.data(), s['p_filesz'], s['p_memsz'])
                for s in e.iter_segments() if s['p_type'] == 'PT_LOAD']
    syms = {s.name: s['st_value'] for s in
            e.get_section_by_name('.symtab').iter_symbols() if s.name}

STUB, NAME, CLK = 0x10000000, 0x10000800, 0x3c500000
THREAD_SP, THREAD_LR = 0x08556a00, 0x0819460c
BOOT_SVC_SP, BOOT_SVC_LR = 0x0000bbe0, 0x080000ac


def word(u, at, value):
    u.mem_write(at, struct.pack('<I', value))


def rd(u, at):
    return struct.unpack('<I', bytes(u.mem_read(at, 4)))[0]


def cstr(u, at):
    return bytes(u.mem_read(at, 64)).split(b'\0', 1)[0].decode()


def find_insn(start, stop, opcode):
    for addr, data, filesz, _ in segments:
        if addr <= start < addr + filesz:
            for at in range(start, stop, 4):
                if struct.unpack_from('<I', data, at - addr)[0] == opcode:
                    return at
    return None


def machine(armed):
    u = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    for addr, size in [(0, 0x10000), (0x08000000, 0x4000000),
                       (STUB, 0x1000), (CLK, 0x1000)]:
        u.mem_map(addr, size)
    for addr, data, _, _ in segments:
        u.mem_write(addr, data)
    # system_init: null_call_trap_install()
    word(u, 0, 0xe59ff000 | (0x5c - 8))
    for at in range(0x44, 0x5c, 4):
        word(u, at, 0xe59ff000 | (0x5c - (at + 8)) if at + 8 <= 0x5c
             else 0xe51ff000 | ((at + 8) - 0x5c))
    word(u, 0x5c, syms['null_call_trap'])
    if 'crt0_entry_armed' in syms:
        word(u, syms['crt0_entry_armed'], 1 if armed else 0)
    word(u, CLK + 0x54, 0x1)       # RSTSR as a hardware reset would leave it
    u.mem_write(NAME, b'podlink\0')
    return u


def bank(u, sys_sp, sys_lr, mode, sp, lr):
    u.reg_write(UC_ARM_REG_CPSR, 0xdf)
    u.reg_write(UC_ARM_REG_SP, sys_sp)
    u.reg_write(UC_ARM_REG_LR, sys_lr)
    u.reg_write(UC_ARM_REG_CPSR, mode)
    if mode & 0x1f != 0x1f:
        u.reg_write(UC_ARM_REG_SP, sp)
        u.reg_write(UC_ARM_REG_LR, lr)


def run(u, pc, mmu_on, regs=(0x7584, 0x7df4, 0, 0), stop_at=None):
    for r, v in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2,
                     UC_ARM_REG_R3), regs):
        u.reg_write(r, v)
    mrc = None
    if 'null_call_save' in syms:
        mrc = find_insn(syms['null_call_save'], syms['null_call_save'] + 64,
                        0xee110f10)
        assert mrc, 'cp15 c1 read missing from null_call_save'
    ev = dict(path=[], memory_init=0)

    def hook(uc, addr, size, _):
        if addr == mrc:                       # CP15 adapter
            uc.reg_write(UC_ARM_REG_R0, 0x00050079 if mmu_on else 0x00050078)
            uc.reg_write(UC_ARM_REG_PC, addr + 4)
        elif addr == syms.get('memory_init'):
            ev['memory_init'] += 1
            uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
        elif addr == syms['thread_current_name']:
            uc.reg_write(UC_ARM_REG_R0, NAME)
            uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
        elif addr in (syms['null_call_trap'], syms.get('crt0_reentry_trap')):
            ev['path'].append('crt0_reentry_trap'
                              if addr == syms.get('crt0_reentry_trap')
                              else 'null_call_trap')
        elif addr == syms['panicf']:
            sp = uc.reg_read(UC_ARM_REG_SP)
            fmt = cstr(uc, uc.reg_read(UC_ARM_REG_R0))
            args = [uc.reg_read(r) for r in (UC_ARM_REG_R1, UC_ARM_REG_R2,
                                             UC_ARM_REG_R3)]
            args += [rd(uc, sp + 4 * i) for i in range(12)]
            ev['format'] = fmt
            ev['args'] = args
            uc.emu_stop()
        elif addr == stop_at:
            ev['stopped'] = hex(addr)
            uc.emu_stop()

    u.hook_add(UC_HOOK_CODE, hook)
    u.emu_start(pc, 0xffffffff, count=20000)
    return ev


def report(u, ev):
    f, x = ev['format'], ev['args']
    if f.startswith('%s lr:'):
        return dict(what=cstr(u, x[0]), lr=x[1], sp=x[2], mode=x[3],
                    thr=cstr(u, x[4]), r=x[5:9], usp=x[9], ulr=x[10],
                    rs=x[11])
    # FU46 format: "NULL call lr:%08lx sp:%08lx mode:%02lx thr:%.16s r0..r3"
    return dict(what='NULL call', lr=x[0], sp=x[1], mode=x[2],
                thr=cstr(u, x[3]), r=x[4:8])


cases = []


def case(name, result):
    cases.append(dict(case=name, **{k: (hex(v) if isinstance(v, int) else
                                        [hex(i) for i in v] if isinstance(v, list)
                                        else v) for k, v in result.items()}))


# 1. A branch through NULL from a SYS-mode thread names its caller.
u = machine(armed=True)
bank(u, THREAD_SP, 0x0815dead, 0x1f, 0, 0)
word(u, STUB, 0xe12fff33)                        # blx r3 (r3 = 0)
ev = run(u, STUB, mmu_on=True, regs=(0x11, 0x22, 0x33, 0))
r = report(u, ev)
assert ev['path'] == ['null_call_trap'], ev
assert r['what'] == 'NULL call' and r['lr'] == STUB + 4 and r['mode'] == 0x1f, r
assert r['sp'] == THREAD_SP and r['r'] == [0x11, 0x22, 0x33, 0], r
assert ev['memory_init'] == 0
if not a.baseline:
    assert r['usp'] == THREAD_SP and r['ulr'] == STUB + 4, r
case('SYS blx NULL', dict(r, memory_init=ev['memory_init']))

# 2. The field capture: reset entry (SVC, MMU off, boot-time SVC bank,
#    interrupted thread's registers). Must be labelled and must restore the
#    MMU before drawing.
u = machine(armed=True)
bank(u, THREAD_SP, THREAD_LR, 0xd3, BOOT_SVC_SP, BOOT_SVC_LR)
ev = run(u, 0, mmu_on=False)
r = report(u, ev)
assert r['lr'] == BOOT_SVC_LR and r['sp'] == BOOT_SVC_SP and r['mode'] == 0x13, r
assert r['r'] == [0x7584, 0x7df4, 0, 0] and r['thr'] == 'podlink', r
if a.baseline:
    assert r['what'] == 'NULL call' and ev['memory_init'] == 0, (r, ev)
else:
    assert r['what'] == 'RESET' and ev['memory_init'] == 1, (r, ev)
    assert r['usp'] == THREAD_SP and r['ulr'] == THREAD_LR and r['rs'] == 1, r
case('reset entry at 0 (field capture shape)', dict(r, memory_init=ev['memory_init']))

# 3. Stray branches into the running DRAM image no longer re-run crt0.
for entry in (0x08000000, 0x0800003c, syms['_loadaddress']):
    u = machine(armed=True)
    bank(u, THREAD_SP, 0x0815dead, 0x1f, 0, 0)
    word(u, STUB, 0xe12fff32)                    # blx r2
    msr = find_insn(syms['_loadaddress'], syms['_loadaddress'] + 32, 0xe321f0d3)
    ev = run(u, STUB, mmu_on=True, regs=(0x11, 0x22, entry, 0x44),
             stop_at=msr)
    if a.baseline:
        assert ev.get('stopped') == hex(msr), ev
        case('branch to %#x' % entry, dict(reached='crt0 msr cpsr_c,#0xd3'))
        continue
    r = report(u, ev)
    assert ev['path'] == ['crt0_reentry_trap'] and 'stopped' not in ev, ev
    assert r['what'] == 'CRT0 entry' and r['lr'] == STUB + 4, r
    assert r['mode'] == 0x1f and r['sp'] == THREAD_SP, r
    assert r['r'] == [0x11, 0x22, entry, 0x44], r
    case('branch to %#x' % entry, r)

# 4. Real boot is unchanged: unarmed image runs into crt0's mode switch.
if not a.baseline:
    for addr, data, filesz, memsz in segments:
        at = syms['crt0_entry_armed']
        if addr <= at < addr + memsz:
            assert at + 4 <= addr + filesz, 'flag must be in the loaded image'
            assert struct.unpack_from('<I', data, at - addr)[0] == 0
            break
    else:
        raise AssertionError('crt0_entry_armed not in a PT_LOAD segment')
    for mode in (0xd3, 0x1f):
        u = machine(armed=False)
        bank(u, 0x0bf00000, 0, mode, 0x0bf00000, 0)
        msr = find_insn(syms['_loadaddress'], syms['_loadaddress'] + 32,
                        0xe321f0d3)
        ev = run(u, 0x08000000, mmu_on=False, stop_at=msr)
        assert ev.get('stopped') == hex(msr) and not ev['path'], ev
        case('boot entry mode %#x' % mode, dict(reached='crt0 msr cpsr_c,#0xd3'))

print(json.dumps(dict(elf=str(a.elf), baseline=a.baseline, cases=cases),
                 indent=2))
