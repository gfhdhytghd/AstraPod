#!/usr/bin/env python3
"""Replay a blocked UI watchdog tick while Mikey is in an ARM I2C poll.

Loads the hardware ELF, executes the real remote, I2C, tick redirect,
guard check and crash writer. Emulates the jack, I2C/clock registers,
key queue occupancy and LCD output. It does not boot the player or model
the reason the UI blocked. Requires Unicorn and pyelftools.
"""
import argparse
from pathlib import Path
import struct

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UcError, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_READ
from unicorn.arm_const import (UC_ARM_REG_CPSR, UC_ARM_REG_SP, UC_ARM_REG_LR,
                               UC_ARM_REG_PC, UC_ARM_REG_R0, UC_ARM_REG_R2)


def read_image(path):
    with path.open('rb') as src:
        elf = ELFFile(src)
        segments = [(s['p_vaddr'], s.data()) for s in elf.iter_segments()
                    if s['p_type'] == 'PT_LOAD']
        symbols = {s.name: (s['st_value'], s['st_size'])
                   for s in elf.get_section_by_name('.symtab').iter_symbols()
                   if s.name and s['st_info']['type'] != 'STT_FILE'}
    return segments, symbols


class Replay:
    STUB = 0x10000000
    # Native 6G ARM structures. The fixture checks the loaded sizes and
    # stack bounds against ELF symbols before replaying the interrupt.
    CORE_RUNNING, CORE_NEXT_TMO = 12, 52
    THREAD_LR, THREAD_STACK = 36, 44
    THREAD_WQP, THREAD_BLK_RA = 76, 100
    THREAD_STATE, THREAD_STACK_SIZE = 150, 152
    STATE_BLOCKED = 2
    GUARD = 0xdeadbeef

    def __init__(self, path, log_attached=True):
        segments, self.symbols = read_image(path)
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        for addr, size in [(0, 0x10000), (0x08000000, 0x4000000),
                           (self.STUB, 0x4000), (0x3c500000, 0x1000),
                           (0x3c600000, 0x1000), (0x3c700000, 0x1000),
                           (0x38e00000, 0x1000),
                           (0x48000000, 0x4000000)]:
            self.uc.mem_map(addr, size)
        for addr, data in segments:
            self.uc.mem_write(addr, data)
        self.uc.reg_write(UC_ARM_REG_CPSR, 0xd2)
        self.uc.reg_write(UC_ARM_REG_SP, self.addr('_irqstackend'))
        self.uc.reg_write(UC_ARM_REG_CPSR, 0x1f)
        self.main_top = self.addr('_stackend')
        self.main_min_sp = self.main_top
        self.irq_top = self.addr('_irqstackend')
        self.irq_min_sp = self.irq_top
        self.uc.reg_write(UC_ARM_REG_SP, self.main_top)
        self.min_sp = None
        self.report_min_sp = None
        self.report_top = self.word(self.addr('hang_fiq_stack_end'))
        self.now_us = self.polls = self.reads = 0
        self.inject = self.interrupted = False
        self.panicked = self.saved = False
        self.lcd = []
        self.jack = True
        self.nak = False
        self.mode = 0x2f
        self.switches = 0
        self.stkov_depth = None
        self.stop_prologue_end = self.addr('i2c_stop.part.0') + 8
        self.addresses = {self.addr(n): n for n in (
            'headphones_inserted', 'button_queue_count', 'sleep',
            'switch_thread', 'thread_stkov', 'safe_read32', 'lcd_set_viewport', 'lcd_set_backdrop',
            'lcd_set_drawinfo', 'lcd_clear_display', 'lcd_setfont', 'lcd_puts',
            'lcd_update', 'system_exception_wait', 'rb_backtrace', 'panicf', 'crumbs_crash_record')}
        self.uc.hook_add(UC_HOOK_CODE, self.step)
        self.uc.hook_add(UC_HOOK_MEM_READ, self.mmio, begin=0x3c600000,
                         end=0x3c70ffff)

        self.call('init_threads')
        self.main = self.word(self.addr('__threads'))
        self.call('hang_catch_init')
        self.call('i2c_init')
        self.put(self.addr('rec_hw_ver'), 1)
        self.call('mikey_init')
        self.call('mikey_scope_enable', 1)
        self.call('usb_log_attach', log_attached)
        self.mikey = self.word(self.addr('__threads') + 4)
        self.base = self.word(self.mikey + self.THREAD_STACK)
        self.capacity = self.word(self.mikey + self.THREAD_STACK_SIZE)
        assert self.addr('mikey_stack') <= self.base < self.addr('mikey_stack') + 8
        self.top = self.base + self.capacity
        self.put(self.addr('__cores') + self.CORE_RUNNING, self.mikey)

    def addr(self, name):
        return self.symbols[name][0]

    def word(self, addr):
        return struct.unpack('<I', self.uc.mem_read(addr, 4))[0]

    def put(self, addr, value):
        self.uc.mem_write(addr, struct.pack('<I', value))

    def ret(self, value=0):
        self.uc.reg_write(UC_ARM_REG_R0, value)
        self.uc.reg_write(UC_ARM_REG_PC, self.uc.reg_read(UC_ARM_REG_LR))

    def call(self, name, arg=0):
        self.uc.reg_write(UC_ARM_REG_R0, arg)
        self.uc.reg_write(UC_ARM_REG_LR, self.STUB)
        self.uc.emu_start(self.addr(name), self.STUB, count=300000)
        assert self.uc.reg_read(UC_ARM_REG_PC) == self.STUB, name

    def mmio(self, uc, access, addr, size, value, data):
        if addr == 0x3c7000b4:  # TECNT advances during udelay()
            self.now_us += 5
            self.put(addr, self.now_us)
        elif addr == 0x3c600020:  # IICSTA2: ACK/byte ready
            self.put(addr, 1 << 8)
        elif addr == 0x3c600004 and self.nak and self.word(addr) & 0x20:
            self.put(addr, self.word(addr) | 1)
        elif addr == 0x3c60000c:
            self.reads += 1
            self.put(addr, self.mode)

    def step(self, uc, pc, size, data):
        sp = uc.reg_read(UC_ARM_REG_SP)
        if self.min_sp is not None and self.base - 2048 < sp <= self.top:
            self.min_sp = min(self.min_sp, sp)
        if self.addr('_stackbegin') < sp <= self.main_top:
            self.main_min_sp = min(sp, self.main_min_sp)
        if self.addr('_irqstackbegin') < sp <= self.irq_top:
            self.irq_min_sp = min(sp, self.irq_min_sp)
        if self.report_top - 2048 < sp <= self.report_top:
            self.report_min_sp = min(sp, self.report_min_sp or sp)
        name = self.addresses.get(pc)
        if name == 'headphones_inserted':
            self.ret(self.jack)
        elif name == 'button_queue_count':
            self.ret(1)
        elif name == 'sleep' and not self.interrupted:
            self.polls += 1
            self.put(self.addr('current_tick'), self.polls * 2 + 5000)
            # Check the normal poll with NAK, a mode rewrite, register sweep,
            # volume edges and removal/remote reset.
            self.nak = self.polls == 1
            self.mode = (0 if self.polls == 2 else 0x2f)
            self.jack = self.polls < 5
            if self.polls == 6:
                uc.emu_stop()
            else:
                self.ret()
        elif (pc == self.stop_prologue_end and self.inject
              and not self.interrupted and self.top - sp == 280):
            self.interrupted = True
            self.interrupted_sp, self.interrupted_pc = sp, pc
            uc.emu_stop()
        elif name == 'safe_read32':
            # An unmapped read returns false via the target's abort vector.
            try:
                uc.mem_read(uc.reg_read(UC_ARM_REG_R0), 4)
            except UcError:
                self.ret(False)
        elif name == 'switch_thread':
            self.switches += 1
        elif name == 'thread_stkov':
            self.stkov_depth = self.top - sp
        elif name == 'lcd_puts':
            addr = uc.reg_read(UC_ARM_REG_R2)
            self.lcd.append(bytes(uc.mem_read(addr, 88)).split(b'\0', 1)[0])
            self.ret()
        elif name == 'rb_backtrace' or (name and name.startswith('lcd_')):
            self.ret()
        elif name == 'crumbs_crash_record':
            self.saved = True
        elif name == 'panicf':
            self.panicked = True
        elif name == 'system_exception_wait':
            uc.emu_stop()

    def run_remote(self, inject=False):
        self.inject = inject
        self.min_sp = self.top
        self.uc.reg_write(UC_ARM_REG_SP, self.top)
        self.uc.emu_start(self.addr('mikey_thread'), self.STUB, count=300000)
        if inject:
            assert self.interrupted
        else:
            assert self.polls == 6
        assert self.word(self.base) == self.GUARD

    def run_watchdog(self):
        # Use the same slot fields as the 6G ARM scheduler. The real
        # hang_tick below must observe STATE_BLOCKED and rewrite IRQ PC.
        self.uc.mem_write(self.main + self.THREAD_STATE, bytes([self.STATE_BLOCKED]))
        self.put(self.main + self.THREAD_LR, 0x08100004)
        self.put(self.main + self.THREAD_WQP, 0x08101000)
        self.put(self.main + self.THREAD_BLK_RA, 0x08102004)
        self.put(self.addr('current_tick'), 52627)
        self.put(self.addr('__cores') + self.CORE_NEXT_TMO, 52627)
        self.put(self.addr('stuck_since'), 50626)
        irq_pc = self.addr('_irqstackend') - 4
        self.put(irq_pc, self.interrupted_pc)
        self.uc.reg_write(UC_ARM_REG_CPSR, 0xd2)
        self.uc.reg_write(UC_ARM_REG_SP, irq_pc)
        self.call('hang_tick')
        assert self.word(irq_pc) == self.addr('hang_show')
        assert self.word(self.addr('hang_sp')) == self.interrupted_sp
        self.uc.reg_write(UC_ARM_REG_CPSR, 0x1f)
        self.uc.emu_start(self.word(irq_pc), self.STUB, count=500000)
        assert self.uc.reg_read(UC_ARM_REG_PC) == self.addr('system_exception_wait')

    def crash_context(self, revised):
        record = self.addr('dram_crash') + 0x40000000
        assert self.saved and self.word(record) != 0
        fields = struct.unpack('<8I', self.uc.mem_read(record + 12, 32))
        version = record + (88 if revised else 44)
        message = bytes(self.uc.mem_read(version + 64, 256)).split(b'\0', 1)[0]
        return fields, message


def verify(path, revised):
    remote = Replay(path)
    remote.run_remote()
    hang = Replay(path)
    hang.run_remote(inject=True)
    hang.run_watchdog()
    fields, message = hang.crash_context(revised)
    if revised:
        assert not hang.panicked and hang.switches == 0
        assert hang.word(hang.base) == hang.GUARD
        assert fields[:4] == (3, hang.interrupted_pc, hang.interrupted_sp,
                              hang.word(hang.addr('hang_lr')))
        assert message == b'main blocked 20s, keys unread'
        record = hang.addr('dram_crash') + 0x40000000
        assert hang.uc.mem_read(record + 72, 6) == b'mikey\0'
        assert struct.unpack('<3I', hang.uc.mem_read(record + 44, 12)) == (
            0x08100004, 0x08101000, 0x08102004)
        assert hang.report_top - hang.report_min_sp < 1536
        assert (b'*HANG* (' in hang.lcd[0] and
                any(b'mikey' in line for line in hang.lcd))
    else:
        assert hang.panicked and hang.switches == 1
        assert hang.word(hang.base) != hang.GUARD
        assert fields[0] == 1 and fields[1] == fields[2] and fields[3] == 0
        assert message == b'Stkov mikey'
    fault = 'HANG' if revised else 'Stkov mikey'
    print(f'{path}: {fault}')
    print(f'  Mikey: normal {remote.top - remote.min_sp}/{remote.capacity} B; '
          f'watchdog {hang.top - hang.min_sp}/{hang.capacity} B; '
          f'at guard failure {hang.stkov_depth} B')
    print(f'  Main fixture setup: {hang.main_top - hang.main_min_sp} B; '
          f'tick IRQ: {hang.irq_top - hang.irq_min_sp} B; '
          f'report: {hang.report_top - (hang.report_min_sp or hang.report_top)}/2048 B')
    return hang


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--candidate', type=Path)
    args = parser.parse_args()
    verify(args.baseline, revised=False)
    if args.candidate:
        verify(args.candidate, revised=True)
        panic = Replay(args.candidate)
        panic.uc.reg_write(UC_ARM_REG_R0, panic.addr('__main_thread_name_str'))
        panic.uc.reg_write(UC_ARM_REG_LR, panic.STUB)
        panic.uc.emu_start(panic.addr('panicf'), panic.STUB, count=100000)
        fields, message = panic.crash_context(revised=True)
        assert fields[:4] == (1, panic.STUB, panic.main_top, panic.STUB)
        assert message == b'main'
        print('  Panic entry SP/LR and retained record agree')


if __name__ == '__main__':
    main()
