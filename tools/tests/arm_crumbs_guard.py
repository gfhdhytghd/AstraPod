#!/usr/bin/env python3
"""ARM breadcrumbs and memory guards (FU51), on the ELF's own instructions.

Unicorn as an ARM926 with its MMU: the firmware's real memory_init builds
the page tables, then crumbs_guard_init / guard_tick / crumbs_boot run.
Memory is plain RAM; the clock, DMA and USB blocks are zero-filled pages.

Checks:
 - layout: .persist after the loaded image and below the bootloader's .bss
   (DRAM + 8 MiB); the ring fits; VA page 0 holds only the vectors, IRAM
   code starts at 0x1000, the read-only IRAM pages hold code only, the main
   stack is in DRAM.
 - MMU guard: after crumbs_guard_init c1.V is set and the vectors are read
   at 0xffff0000 (read-only, same words). Page 0 (FU53) is the old low
   vector page, zeroed and read-only: a NULL / near-NULL read returns 0 (the
   last two words: the trap), a store data-aborts (Unicorn does not enter
   guest vectors: the vector used is c1.V plus the high page's words), and a
   call anywhere into it runs to null_call_trap with lr and r0-r3 intact
   (ARMv5 has no execute-never); a store
   to the IRAM code pages aborts, reads work; stores past them, to the
   stacks and to DRAM do not. crumbs_guard_off (ROLO) restores low vectors
   and the VA 0 section, with the vector words back at PA 0.
 - guard_tick: quiet on an intact machine (and writes the tick status word
   to the retained DRAM ring); a changed live vector word or a changed word
   anywhere in the PA 0 page (written past the MMU, as a DMA would), a
   changed SVC bank, a
   changed IRAM code block and a changed page-table block each end in panicf
   naming it, after a crumb.
 - crumbs_boot: a ring left by a previous boot (built here with the host
   formulas) is taken whole, and a new ring with the next boot number starts.
No device access.
"""
import argparse
import json
import struct
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import (Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE,
                     UC_HOOK_INTR, UcError)
from unicorn.arm_const import (UC_CPU_ARM_926, UC_ARM_REG_PC, UC_ARM_REG_SP,
                               UC_ARM_REG_LR, UC_ARM_REG_R0, UC_ARM_REG_R1,
                               UC_ARM_REG_CPSR)

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--elf', type=Path, required=True)
p.add_argument('--out', type=Path)
a = p.parse_args()
with a.elf.open('rb') as f:
    e = ELFFile(f)
    segments = [(s['p_paddr'], s['p_vaddr'], s.data(), s['p_filesz'])
                for s in e.iter_segments() if s['p_type'] == 'PT_LOAD']
    syms, kinds = {}, {}
    for s in e.get_section_by_name('.symtab').iter_symbols():
        if not s.name:
            continue
        if s.name in syms and syms[s.name] != s['st_value']:
            syms[s.name] = None
        else:
            syms[s.name] = s['st_value']
            kinds[s.name] = s['st_info']['type']
    objects = [(s['st_value'], s.name) for s in
               e.get_section_by_name('.symtab').iter_symbols()
               if s['st_info']['type'] == 'STT_OBJECT']

def sym(name):
    v = syms.get(name)
    assert v is not None, f'symbol {name} missing or ambiguous'
    return v & ~1

DRAM, DRAM_SIZE = 0x08000000, 64 << 20
TTB = DRAM + DRAM_SIZE - 0x4000
IRAM1_RING = 0x2203f800
STUB = 0x10000000
RET = STUB + 0x800
CLK, DMA0, OTG = 0x3c500000, 0x38200000, 0x38400000
res = {}

# --- layout ---------------------------------------------------------------
ps, pe = sym('_persiststart'), sym('_persistend')
assert sym('_edata') <= ps and pe <= DRAM + 0x800000, (hex(ps), hex(pe))
assert ps <= sym('dram_ring') and sym('dram_ring') + 2008 <= pe
assert sym('audiobuffer') >= pe
ro_end = sym('_icodeend') & ~0xfff
assert sym('_icodestart') == 0x1000 and ro_end > 0x1000
assert sym('_intvectend') + 8 <= 0x60
assert DRAM <= sym('stackbegin') and sym('stackend') - sym('stackbegin') == 0x3000
assert sym('_fiqstackend') <= 0xc000                 # codec IRAM from 48 KiB
assert sym('hivec') & 0xfff == 0 and sym('l2tab') & 0x3ff == 0
inside = [(hex(v), n) for v, n in objects if v < ro_end]
assert not inside, inside                  # no data in the read-only pages
res['layout'] = dict(persist=[hex(ps), hex(pe)], ro_end=hex(ro_end),
                     iram_end=hex(sym('_fiqstackend')),
                     main_stack=[hex(sym('stackbegin')), hex(sym('stackend'))],
                     bootloader_bss=hex(DRAM + 0x800000))

# --- machine --------------------------------------------------------------
def word(u, at, v):
    u.mem_write(at, struct.pack('<I', v & 0xffffffff))

def rd(u, at):
    return struct.unpack('<I', bytes(u.mem_read(at, 4)))[0]

def cstr(u, at):
    b = bytes(u.mem_read(at, 160))
    return b.split(b'\0')[0].decode('latin1')

class Stop(Exception):
    pass

def machine():
    u = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    u.ctl_set_cpu_model(UC_CPU_ARM_926)
    for addr, size in [(0, 0x40000), (DRAM, DRAM_SIZE), (0x22000000, 0x40000),
                       (STUB, 0x1000), (CLK, 0x1000), (DMA0, 0x1000),
                       (OTG, 0x1000)]:
        u.mem_map(addr, size)
    for paddr, vaddr, data, _ in segments:
        u.mem_write(vaddr, data)
    word(u, RET, 0xeafffffe)               # b .
    u.state = dict(panic=None, aborts=[], stubs={})
    def code(u, addr, size, st):
        if addr == sym('panicf'):
            st['panic'] = cstr(u, u.reg_read(UC_ARM_REG_R0))
            st['panic_r1'] = u.reg_read(UC_ARM_REG_R1)
            u.emu_stop()
        elif addr in st['stubs']:
            st['stubs'][addr] += 1
            u.reg_write(UC_ARM_REG_PC, u.reg_read(UC_ARM_REG_LR))
    def intr(u, intno, st):
        st['aborts'].append(intno)
        u.emu_stop()
    u.hook_add(UC_HOOK_CODE, code, u.state)
    u.hook_add(UC_HOOK_INTR, intr, u.state)
    return u

def call(u, fn, *args, mode=0xdf, sp=None):
    """Run fn(args) in the given mode until it returns to RET."""
    word(u, STUB, 0xe321f000 | mode)       # msr cpsr_c, #mode
    word(u, STUB + 4, 0xe12fff10)          # bx r0
    u.emu_start(STUB, STUB + 4)
    if sp is not None:
        u.reg_write(UC_ARM_REG_SP, sp)
    for i, v in enumerate(args):
        u.reg_write(UC_ARM_REG_R0 + i, v)
    u.reg_write(UC_ARM_REG_LR, RET)
    u.state['panic'] = None
    u.emu_start(sym(fn), RET, count=20_000_000)
    return u.reg_read(UC_ARM_REG_PC)

def set_bank(u, mode, sp, lr=None):
    word(u, STUB, 0xe321f000 | mode | 0xc0)
    word(u, STUB + 4, 0xe1a00000)
    u.emu_start(STUB, STUB + 4)
    u.reg_write(UC_ARM_REG_SP, sp)
    if lr is not None:
        u.reg_write(UC_ARM_REG_LR, lr)

def mmu(u):
    """crt0's banks and memory_init (entered with the MMU off)."""
    irq_top, boot_lr = sym('_irqstackend'), sym('crt0_boot_lr')
    for m in (0x13, 0x17, 0x1b):
        set_bank(u, m, irq_top, boot_lr if m == 0x13 else None)
    set_bank(u, 0x12, irq_top)
    set_bank(u, 0x1f, sym('stackend'))
    call(u, 'memory_init', mode=0xd3, sp=irq_top - 0x100)
    assert rd(u, TTB) & 3 == 2                # section VA 0 -> PA 0
    set_bank(u, 0x13, irq_top, boot_lr)       # memory_init used SVC lr
    return u

def boot(u):
    """mmu(), then crumbs_boot and crumbs_guard_init, in main.c's order."""
    mmu(u)
    assert call(u, 'crumbs_boot') == RET
    u.state['stubs'] = {sym('tick_add_task'): 0}
    pc = call(u, 'crumbs_guard_init')
    assert pc == RET and u.state['stubs'][sym('tick_add_task')] == 1
    assert rd(u, sym('crumbs_ro_end')) == ro_end, hex(rd(u, sym('crumbs_ro_end')))
    return u

def store(u, at, value=0x12345678, load=False):
    word(u, STUB + 0x100, 0xe5901000 if load else 0xe5801000)  # ldr/str r1,[r0]
    word(u, STUB + 0x104, 0xeafffffe)
    word(u, STUB, 0xe321f0df)
    word(u, STUB + 4, 0xe1a00000)
    u.emu_start(STUB, STUB + 4)
    u.reg_write(UC_ARM_REG_R0, at)
    u.reg_write(UC_ARM_REG_R1, value)
    u.state['aborts'] = []
    u.emu_start(STUB + 0x100, STUB + 0x104, count=10)
    return 4 in u.state['aborts']              # EXCP_DATA_ABORT

def cp15_c1(u):
    word(u, STUB + 0x200, 0xee110f10)          # mrc p15, 0, r0, c1, c0, 0
    word(u, STUB + 0x204, 0xe1a00000)
    word(u, STUB, 0xe321f0d3)
    word(u, STUB + 4, 0xe1a00000)
    u.emu_start(STUB, STUB + 4)
    u.emu_start(STUB + 0x200, STUB + 0x204)
    return u.reg_read(UC_ARM_REG_R0)

def vread(u, at):
    """A CPU load through the MMU; None if it aborts."""
    if store(u, at, load=True):
        return None
    return u.reg_read(UC_ARM_REG_R1)

# --- MMU guard -----------------------------------------------------------
u = machine()
low = [rd(u, 4 * i) for i in range(24)]       # .intvect, as crt0 copies it
boot(u)
assert cp15_c1(u) & (1 << 13), hex(cp15_c1(u))
high = [vread(u, 0xffff0000 + 4 * i) for i in range(24)]
assert high == low, (high, low)
assert high[6] & 0xfffff000 == 0xe59ff000      # ldr pc, [pc, #..] (IRQ)
ro = {}
null = {}
trap = sym('null_call_trap')
def low_word(i):
    return 0xe51ff004 if i == 1022 else trap if i == 1023 else 0
assert [rd(u, 4 * i) for i in range(1024)] == [low_word(i) for i in range(1024)]
for at in [0x0, 0xc, 0x10, 0x18, 0x40, 0x5c, 0x60, 0x800, 0xff4, 0xff8, 0xffc]:
    null[hex(at)] = dict(read=vread(u, at), write_aborts=store(u, at))
    assert null[hex(at)] == dict(read=low_word(at // 4), write_aborts=True), \
        (hex(at), null)
    null[hex(at)]['read'] = hex(null[hex(at)]['read'])
assert [rd(u, 4 * i) for i in range(1024)] == [low_word(i) for i in range(1024)]
# A call into page 0: zero words are "andeq r0, r0, r0", then the trap jump
calls = {}
for at in [0x0, 0x4, 0x5c, 0x800, 0xff8]:
    word(u, STUB, 0xe321f0df)
    word(u, STUB + 4, 0xe1a00000)
    u.emu_start(STUB, STUB + 4)
    regs = [0x1111, 0x2222, 0x3333, 0x4444]
    for i, v in enumerate(regs):
        u.reg_write(UC_ARM_REG_R0 + i, v)
    u.reg_write(UC_ARM_REG_LR, RET)
    u.state['aborts'] = []
    u.emu_start(at, trap, count=2000)
    pc = u.reg_read(UC_ARM_REG_PC)
    got = [u.reg_read(UC_ARM_REG_R0 + i) for i in range(4)]
    assert pc == trap and not u.state['aborts'], (hex(at), hex(pc), u.state)
    assert u.reg_read(UC_ARM_REG_LR) == RET and got == regs, (hex(at), got)
    calls[hex(at)] = 'null_call_trap, lr kept'
# Unicorn reports the abort but does not enter the guest's vector; the
# vector taken is c1.V (checked above) plus the words at 0xffff0010.
dabt = vread(u, 0xffff0010)
assert dabt == low[4] and dabt & 0xfffff000 == 0xe59ff000, hex(dabt or 0)
for at in [0xffff0000, 0xffff0018, sym('_icodestart'),
           sym('_icodestart') + 0x100, ro_end - 4]:
    ro[hex(at)] = store(u, at)
    assert ro[hex(at)], f'store to {at:#x} did not abort'
    assert not store(u, at, load=True), f'read of {at:#x} aborted'
for at in [ro_end, sym('_irqstackend') - 8, sym('stackend') - 8,
           DRAM + 0x100000, sym('dram_ring')]:
    ro[hex(at)] = store(u, at)
    assert not ro[hex(at)], f'store to {at:#x} aborted'
res['null_page'] = null
res['null_calls'] = calls
res['high_vectors'] = dict(c1=hex(cp15_c1(u)), dabt_vector=hex(dabt))
res['readonly'] = ro

# crumbs_guard_off (ROLO): low vectors, VA 0 a section again
u = boot(machine())
assert call(u, 'crumbs_guard_off', mode=0xd3,
            sp=sym('_irqstackend') - 0x40) == RET
assert not cp15_c1(u) & (1 << 13)
assert rd(u, TTB) & 3 == 2 and rd(u, TTB + 0xfff * 4) == 0
assert [rd(u, 4 * i) for i in range(24)] == low
assert vread(u, 0x18) == low[6] and not store(u, 0x40)
assert rd(u, sym('crumbs_ro_end')) == 0
res['guard_off'] = 'low vectors, VA 0 section, VA 0xfff00000 unmapped'

# --- guard_tick ----------------------------------------------------------
def tick(u, n=1):
    for _ in range(n):
        word(u, sym('current_tick'), rd(u, sym('current_tick')) + 1)
        pc = call(u, 'guard_tick', mode=0xd2, sp=sym('_irqstackend') - 0x40)
        if u.state['panic']:
            return u.state['panic']
        assert pc == RET, hex(pc)
    return None

u = boot(machine())          # the stores above wrote past ro_end: start over
blocks = rd(u, sym('icode_blocks'))
assert blocks >= (ro_end - sym('_icodestart')) // 256
_q = tick(u, blocks + 70); assert _q is None, _q
status = 6 * 4                                  # header words before status
assert rd(u, sym('dram_ring') + status) == rd(u, sym('current_tick'))
assert rd(u, IRAM1_RING + status) == 0
guard = {}

def newest_id(u, ring):
    seq = rd(u, ring + 20)
    best = None
    for i in range(96):
        at = ring + 88 + 20 * i
        s = rd(u, at)
        if s and (best is None or s > best[0]):
            best = (s, rd(u, at + 8) >> 16)
    return best[1] if best else None

ids = {n: i for i, n in enumerate([
    'CR_NONE', 'CR_BOOT', 'CR_PANIC', 'CR_PANIC_DRAWN', 'CR_HANG',
    'CR_HANG_DRAWN', 'CR_FREEZE', 'CR_FREEZE_DRAWN', 'CR_RESTART',
    'CR_RUST_PANIC', 'CR_AGENTS_IN', 'CR_AGENTS_STEP', 'CR_AGENTS_OUT',
    'CR_CIDER_IN', 'CR_CIDER_OUT', 'CR_PL_API', 'CR_PL_STATE',
    'CR_PL_WORKER_START', 'CR_USB_EVENT', 'CR_USB_RESET', 'CR_USB_ENUM',
    'CR_USB_SETUP', 'CR_USB_TIMEOUT', 'CR_USB_DMA_BAD', 'CR_DMA_STOP',
    'CR_DMA_BAD', 'CR_VECTOR_BAD', 'CR_ICODE_BAD', 'CR_BANK_BAD',
    'CR_SELFTEST', 'CR_SCREEN', 'CR_TTB_BAD'])}

def fresh():
    return boot(machine())

u = fresh()
at = sym('hivec') + 0x38                       # IRQ literal, past the MMU
word(u, at, rd(u, at) ^ 0x10)
guard['vector'] = tick(u)
assert guard['vector'] and guard['vector'].startswith('VECTOR +%02x'), guard
assert newest_id(u, sym('dram_ring')) == ids['CR_VECTOR_BAD']
assert newest_id(u, IRAM1_RING) is None

u = fresh()
word(u, 0x34, rd(u, 0x34) ^ 0x10)              # DMA aimed at PA 0
guard['vector_pa0'] = tick(u)
assert guard['vector_pa0'] and guard['vector_pa0'].startswith('VECTOR PA0 +'), guard
assert u.state['panic_r1'] == 0x34, hex(u.state['panic_r1'])
assert newest_id(u, sym('dram_ring')) == ids['CR_VECTOR_BAD']

for at in [0x800, 0xffc]:                      # the rest of the zero page
    u = fresh()
    word(u, at, rd(u, at) ^ 0x10)
    guard['pa0_%x' % at] = tick(u, 70)
    assert guard['pa0_%x' % at] and \
        guard['pa0_%x' % at].startswith('VECTOR PA0 +'), guard
    assert u.state['panic_r1'] == at, (hex(at), hex(u.state['panic_r1']))
    assert newest_id(u, sym('dram_ring')) == ids['CR_VECTOR_BAD']

u = fresh()
set_bank(u, 0x13, 0xe24ee004, sym('crt0_boot_lr'))
guard['bank'] = tick(u)
assert guard['bank'] and guard['bank'].startswith('BANK'), guard
assert newest_id(u, sym('dram_ring')) == ids['CR_BANK_BAD']

u = fresh()
at = sym('_icodestart') + 3 * 256 + 8
word(u, at, rd(u, at) ^ 1)
guard['icode'] = tick(u, blocks + 2)
assert guard['icode'] and guard['icode'].startswith('ICODE'), guard
assert newest_id(u, sym('dram_ring')) == ids['CR_ICODE_BAD']

u = fresh()
word(u, TTB + 0x300 * 4, rd(u, TTB + 0x300 * 4) ^ (1 << 20))  # VA 0x30000000
guard['ttb'] = tick(u, 70)
assert guard['ttb'] and guard['ttb'].startswith('TTB VA'), guard
assert newest_id(u, sym('dram_ring')) == ids['CR_TTB_BAD']
res['guard_tick'] = guard

# --- crumbs_boot takes the previous ring ------------------------------
M = 0xffffffff
SALT = 0x9e3779b9
def head_check(boot):
    return ((0x43524d42 ^ SALT) + (1 << 8) + boot * 0x01000193) & M
def entry_check(seq, tick_, idr, val):
    h = SALT ^ seq
    h = ((h ^ tick_) * 0x01000193) & M
    h = ((h ^ idr) * 0x01000193) & M
    h = ((h ^ val) * 0x01000193) & M
    return h ^ (h >> 15)

def write_ring(u, at, boot, n):
    for i in range(22 + 96 * 5):
        word(u, at + 4 * i, 0)
    for s in range(1, n + 1):
        idr = (ids['CR_AGENTS_STEP'] << 16)
        e_at = at + 88 + 20 * (s % 96)
        for k, v in enumerate([s, 500 + s, idr, s * 3,
                               entry_check(s, 500 + s, idr, s * 3)]):
            word(u, e_at + 4 * k, v)
    for k, v in enumerate([0x43524d42, 1, boot, 0, head_check(boot), n + 1]):
        word(u, at + 4 * k, v)
    word(u, at + 24 + 4 * 4, 0x55aa)               # status CS_USB_STAGE

u = mmu(machine())
write_ring(u, sym('dram_ring'), 41, 100)
write_ring(u, IRAM1_RING, 41, 100)
pc = call(u, 'crumbs_boot', mode=0xdf)       # after crt0's memory_init
assert pc == RET
prev = sym('crumbs_prev')
valid, clean = u.mem_read(prev, 2)
boot_no, = struct.unpack('<I', bytes(u.mem_read(prev + 4, 4)))
n = rd(u, prev + 8 + 16 * 4)
first_seq = rd(u, prev + 8 + 16 * 4 + 4)
assert valid == 1 and clean == 0 and boot_no == 41 and n == 32, \
    (valid, clean, boot_no, n)
assert first_seq == 69 and rd(u, prev + 8 + 4 * 4) == 0x55aa
assert rd(u, sym('crumbs_prev_copies')) == 1
assert rd(u, sym('dram_ring') + 8) == 42 and rd(u, IRAM1_RING + 8) == 41
assert newest_id(u, sym('dram_ring')) == ids['CR_BOOT']
res['crumbs_boot'] = dict(previous_boot=boot_no, entries=n, first_seq=first_seq,
                          new_boot=42)

# --- the native crash payload survives another crumbs_boot ------------
if 'crumbs_crash_record' in syms:
    context_addr, message_addr = STUB + 0x400, STUB + 0x440
    message = b'Stack overflow in viewportmanager'
    u.mem_write(message_addr, message + b'\0')
    context = (1, 0x08123456, 0x08500000, 0, 0, 0x1f, 0, 0)
    u.mem_write(context_addr, struct.pack('<8I', *context))
    if 'recovery_started' in syms:
        u.state['stubs'][sym('system_reboot')] = 0
    assert call(u, 'crumbs_crash_record', message_addr, context_addr) == RET
    if 'recovery_started' in syms:
        assert u.state['stubs'][sym('system_reboot')] == 1
    crash_addrs = (sym('dram_crash'), IRAM1_RING - 512)
    slots = [bytes(u.mem_read(addr, 364)) for addr in crash_addrs]
    assert slots[1] == bytes(364)
    check, magic, boot_no, *saved_context = struct.unpack_from('<11I', slots[0])
    assert magic == 0x43525331 and boot_no == 42 and tuple(saved_context) == context
    checksum = SALT
    for byte in slots[0][4:]:
        checksum = ((checksum ^ byte) * 0x01000193) & M
    assert check == (checksum or 1)
    assert slots[0][108:].split(b'\0', 1)[0] == message

    assert call(u, 'crumbs_boot') == RET
    assert rd(u, sym('dram_ring') + 8) == 43
    assert [bytes(u.mem_read(addr, 364)) for addr in crash_addrs] == slots
    res['native_crash'] = dict(message=message.decode(), copies=1,
                               retained_from=42, new_boot=43,
                               persist=[hex(addr) for addr in crash_addrs])

out = a.out or a.elf.parent / 'arm_crumbs_guard.json'
out.write_text(json.dumps(res, indent=2) + '\n')
print(json.dumps(res, indent=2))
print('arm_crumbs_guard: ok')
