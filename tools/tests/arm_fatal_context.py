#!/usr/bin/env python3
"""ARM fatal re-entry, zero/NULL Thumb unwinding and deferred LCD policy.

Executes the ELF's entry/guard/unwinder/policy code. Drawing, libc, final
wait, clocking and ADC are adapters. No boot, scheduler or hardware access.
--baseline expects FU45's unsafe behavior; original Agents fault not modeled.
"""
import argparse
import json
import struct
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import (Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE,
                     UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE)
from unicorn.arm_const import (UC_ARM_REG_CPSR, UC_ARM_REG_SPSR, UC_ARM_REG_PC,
                               UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_R0,
                               UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--elf', type=Path, required=True)
p.add_argument('--baseline', action='store_true')
a = p.parse_args()
with a.elf.open('rb') as f:
    e = ELFFile(f)
    segments = [(s['p_vaddr'], s.data()) for s in e.iter_segments()
                if s['p_type'] == 'PT_LOAD']
    syms = {s.name: s['st_value'] for s in
            e.get_section_by_name('.symtab').iter_symbols() if s.name}

STOP, TEXT, NESTED, STATE, CB, TOP = (0x10001000, 0x10002000, 0x10002200,
                                    0x10003000, 0x10004000, 0x10018000)


def machine(mode):
    u = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    for addr, size in [(0, 0x10000), (0x08000000, 0x4000000),
                       (0x10000000, 0x20000)]:
        u.mem_map(addr, size)
    for addr, data in segments:
        u.mem_write(addr, data)
    u.reg_write(UC_ARM_REG_CPSR, 0x5f)
    u.reg_write(UC_ARM_REG_SP, TOP)
    u.reg_write(UC_ARM_REG_LR, STOP)
    u.reg_write(UC_ARM_REG_CPSR, mode)
    u.reg_write(UC_ARM_REG_SP, TOP - 0x2000)
    u.reg_write(UC_ARM_REG_LR, STOP)
    if mode & 0x1f != 0x1f:
        u.reg_write(UC_ARM_REG_SPSR, 0x5f)
    u.mem_write(TEXT, b'FIRST FAULT\0')
    u.mem_write(NESTED, b'NESTED FAULT MUST NOT REPLACE FIRST\0')
    return u


def word(u, at, value):
    u.mem_write(at, struct.pack('<I', value))


def ret(u, value=0):
    u.reg_write(UC_ARM_REG_R0, value)
    u.reg_write(UC_ARM_REG_PC, u.reg_read(UC_ARM_REG_LR))


def cstr(u, at):
    return bytes(u.mem_read(at, 128)).split(b'\0', 1)[0].decode()


def thumb(kind):
    u = machine(0xd3)
    # ARM ABI: RegData[16]=128, MemData=504; cb at 632 (0x278).
    word(u, STATE + 104, TOP)
    word(u, STATE + 108, 4)
    word(u, STATE + 120, 1 if kind == 'zero' else STOP | 1)
    word(u, STATE + 124, 4)
    word(u, STATE + 632, syms['cliCallbacks'] if kind == 'zero' else CB)
    events = []

    def hook(uc, addr, size, _):
        if addr == 0:
            events.append(dict(event='NULL branch', lr=hex(uc.reg_read(UC_ARM_REG_LR))))
            uc.emu_stop()
        elif addr == syms['CliReadH']:
            events.append(dict(event='readH', address=hex(uc.reg_read(UC_ARM_REG_R0))))
            uc.emu_stop()
        elif addr == STOP:
            events.append(dict(event='return', result=uc.reg_read(UC_ARM_REG_R0)))
            uc.emu_stop()

    u.reg_write(UC_ARM_REG_R0, STATE)
    u.hook_add(UC_HOOK_CODE, hook)
    u.emu_start(syms['UnwStartThumb'], STOP + 4, count=100000)
    if a.baseline:
        assert events[0]['event'] == ('readH' if kind == 'zero' else 'NULL branch'), events
    else:
        assert events == [dict(event='return', result=7 if kind == 'zero' else 9)], events
    return dict(case='Thumb ' + kind, events=events)


def fatal(entry, mode, exception=4, message='FIRST FAULT'):
    u = machine(mode)
    u.mem_write(TEXT, message.encode() + b'\0')
    u.reg_write(UC_ARM_REG_R0, TEXT if entry == 'panicf' else 0x08123456)
    u.reg_write(UC_ARM_REG_R1, exception)
    word(u, syms['cpu_frequency'], 216000000)
    calls, formatted, labels, overflow = [], [], [], []
    injected = False
    ended = False
    adapters = {syms[n]: n for n in [
        'vsnprintf', 'strlen', 'lcd_set_viewport', 'lcd_set_backdrop',
        'lcd_set_drawinfo', 'lcd_clear_display', 'lcd_setfont', 'lcd_puts',
        'lcd_putsf', 'lcd_update', 'rb_backtrace', 'usb_log', 'usb_log_sync',
        'hang_log', 'stuck_log', 'stuck_draw', 'thread_current_name',
        'system_exception_wait', 'system_reboot', 'mutex_lock', 'i2c_write',
    ] if n in syms}

    def hook(uc, addr, size, _):
        nonlocal injected, ended
        n = adapters.get(addr)
        if not n:
            return
        calls.append(n)
        r0, r1, r2 = [uc.reg_read(r) for r in
                      [UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2]]
        if n == 'rb_backtrace' and not injected:
            # The first text must be committed before the unwinder can fail.
            if not a.baseline:
                assert 'lcd_update' in calls, calls
            injected = True
            uc.reg_write(UC_ARM_REG_R0, NESTED)
            uc.reg_write(UC_ARM_REG_PC, syms['panicf'])
        elif n in ('system_exception_wait', 'system_reboot', 'mutex_lock', 'i2c_write'):
            ended = True
            uc.emu_stop()
        elif n == 'vsnprintf':
            text = cstr(uc, r2)
            formatted.append(text)
            if a.baseline and text.startswith('NESTED'):
                ended = True
                uc.emu_stop()
            else:
                data = text.encode()[:r1 - 1]
                uc.mem_write(r0, data + b'\0')
                ret(uc, len(data))
        elif n == 'strlen':
            ret(uc, len(cstr(uc, r0)))
        elif n == 'thread_current_name':
            ret(uc, TEXT)
        else:
            if n in ('lcd_puts', 'lcd_putsf'):
                labels.append(cstr(uc, r2))
            ret(uc)

    def memory(uc, access, address, size, value, _):
        end = syms['panic_buf'] + 128
        if end <= address < end + 64:
            overflow.append(dict(address=hex(address), size=size))

    u.hook_add(UC_HOOK_CODE, hook)
    u.hook_add(UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE, memory)
    u.emu_start(syms[entry], STOP + 4, count=1000000)
    assert ended and injected, (entry, mode, calls)
    if a.baseline:
        assert formatted[-1].startswith('NESTED'), formatted
        if len(message) == 127:
            assert overflow, overflow
    else:
        assert formatted == ([message] if entry == 'panicf' else []), formatted
        assert calls.count('lcd_clear_display') == 1, calls
        assert not any(n in calls for n in ('mutex_lock', 'i2c_write', 'system_reboot')), calls
        if entry == 'panicf':
            assert cstr(u, syms['panic_buf']) == message
            assert not overflow, overflow
    return dict(case=entry, mode=hex(mode), exception=exception,
                formatted=formatted, clears=calls.count('lcd_clear_display'),
                labels=labels, halt=calls[-1], message_bytes=len(message),
                buffer_overflow=overflow)


def mirror(mode):
    u = machine(mode)
    for name in ('svid_policy_pending', 'svid_identification_pending'):
        u.mem_write(syms[name], b'\x01')
    u.mem_write(syms['svid_mode'], b'\x02')  # target uses -fshort-enums
    u.reg_write(UC_ARM_REG_R0, TEXT)
    u.reg_write(UC_ARM_REG_R1, 0)
    u.reg_write(UC_ARM_REG_R2, 0)
    u.reg_write(UC_ARM_REG_R3, 1)
    for i in range(3):
        word(u, u.reg_read(UC_ARM_REG_SP) + i * 4, 1)
    adc, returned = 0, False

    def hook(uc, addr, size, _):
        nonlocal adc, returned
        if addr == syms['pmu_accessory_present']:
            ret(uc, 1)
        elif addr == syms['adc_read_accessory_resistor']:
            adc += 1
            uc.emu_stop()
        elif addr == STOP:
            returned = True
            uc.emu_stop()

    u.hook_add(UC_HOOK_CODE, hook)
    u.emu_start(syms['videoout_mirror_rgb565'], STOP + 4, count=100000)
    normal = mode == 0x5f
    assert adc == int(a.baseline or normal), (mode, adc, returned)
    if not a.baseline and not normal:
        assert returned
        assert bytes(u.mem_read(syms['svid_policy_pending'], 1)) == b'\x01'
    return dict(case='LCD mirror', mode=hex(mode), adc=adc, returned=returned)


rows = [thumb('zero'), thumb('NULL readH')]
rows += [fatal('panicf', m) for m in (0x5f, 0xd2, 0xd3)]
rows += [fatal('UIE', m, n) for m, n in [(0xd3, 4), (0xd7, 2),
                                      (0xd7, 1), (0xdb, 0)]]
rows += [fatal('hang_show', 0x5f)]
rows += [fatal('panicf', 0x5f, message='L' * 127)]
rows += [mirror(m) for m in (0x5f, 0xdf, 0xd3)]
print(json.dumps(dict(elf=str(a.elf), baseline=a.baseline, cases=rows,
                     scope=__doc__), indent=2))
