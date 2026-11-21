#!/usr/bin/env python3
"""ARM freeze watch on the timer FIQ (FU50).

Runs the ELF's real fiq_handler, hang_fiq_timer and hang_catch_fiq under
Unicorn, entered as the core enters an FIQ: FIQ mode with I+F set, SPSR the
interrupted CPSR, lr the interrupted pc + 4, sp the FIQ stack. The interrupted
SYS and IRQ banks hold known values. Hardware: the timer block and VIC0 are
plain memory, so the test sets TSTAT/TFCON/TFCMD and reads VIC0INTSELECT.
No scheduler, LCD or device access.

Checks: IRQs off (tick frozen) trips on the 30th period and enters
hang_catch_fiq_show on its own stack with the interrupted pc/cpsr and banked
sp/lr captured; ordinary returns restore the interrupted mode and leave its
banked registers (r8-r12, sp, lr) untouched; a running tick or a disarmed
watch never trips; a pending user timer F hands VIC line 7 back to IRQ.
Static: FIQ vector -> fiq_handler (not fiq_dummy), INT_TIMERG is not UIRQ,
switch_thread notes hang_ui_ran, init/stop start/stop the timer FIQ.
"""
import argparse
import json
import struct
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm_const import (UC_ARM_REG_CPSR, UC_ARM_REG_PC, UC_ARM_REG_SP,
                               UC_ARM_REG_LR, UC_ARM_REG_R0, UC_ARM_REG_R2,
                               UC_ARM_REG_R8,
                               UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11,
                               UC_ARM_REG_R12)

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--elf', type=Path, required=True)
p.add_argument('--out', type=Path)
a = p.parse_args()
with a.elf.open('rb') as f:
    e = ELFFile(f)
    segments = [(s['p_vaddr'], s.data(), s['p_filesz'])
                for s in e.iter_segments() if s['p_type'] == 'PT_LOAD']
    syms, sizes = {}, {}
    for s in e.get_section_by_name('.symtab').iter_symbols():
        if s.name and s['st_info']['type'] in ('STT_FUNC', 'STT_OBJECT',
                                                'STT_NOTYPE'):
            if s.name in syms and syms[s.name] != s['st_value']:
                syms[s.name] = None          # ambiguous local name
            else:
                syms[s.name] = s['st_value']
                sizes[s.name] = s['st_size']

def sym(name):
    v = syms.get(name)
    assert v is not None, f'symbol {name} missing or ambiguous'
    return v & ~1

STUB, INT_PC = 0x10000000, 0x10000100
TIMER, VIC0 = 0x3c700000, 0x38e00000
SYS_SP, SYS_LR = 0x08556a00, 0x0819460c
IRQ_SP, IRQ_LR, IRQ_SPSR = 0x0000bb80, 0x08001234, 0x6000001f
HIGH = [0x88888888, 0x99999999, 0xaaaaaaaa, 0xbbbbbbbb, 0xcccccccc]
HIGH_REGS = [UC_ARM_REG_R8, UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11,
             UC_ARM_REG_R12]

def word(u, at, value):
    u.mem_write(at, struct.pack('<I', value & 0xffffffff))

def rd(u, at):
    return struct.unpack('<I', bytes(u.mem_read(at, 4)))[0]

def image_word(at):
    for addr, data, filesz in segments:
        if addr <= at < addr + filesz:
            return struct.unpack_from('<I', data, at - addr)[0]
    raise AssertionError(hex(at))

# --- static wiring --------------------------------------------------------
static = {}
vec = image_word(0x1c)
assert vec & 0xfffff000 == 0xe59ff000, hex(vec)        # ldr pc, [pc, #imm]
static['fiq_vector'] = hex(image_word(0x1c + 8 + (vec & 0xfff)))
assert int(static['fiq_vector'], 16) == sym('fiq_handler')
assert syms.get('fiq_dummy') in (None,) or syms['fiq_dummy'] != sym('fiq_handler')
assert sym('INT_TIMERG') != sym('UIRQ')

def words(name):
    at = sym(name)
    return [(at + i, image_word(at + i)) for i in range(0, sizes[name], 4)]

def calls(name, target):
    t = sym(target)
    for at, w in words(name):
        if w & 0x0e000000 == 0x0a000000:           # b / bl (tail calls too)
            off = w & 0xffffff
            off = off - (1 << 24) if off & 0x800000 else off
            if at + 8 + 4 * off == t:
                return True
    return False

lits = {w for _, w in words('switch_thread')}
assert sym('hang_ui_thread') in lits and sym('hang_ui_ran') in lits
assert calls('hang_catch_init', 'hang_watch_fiq_start')
assert calls('hang_catch_stop', 'hang_watch_fiq_stop')
static.update(switch_thread_hook=True, init_starts=True, stop_stops=True)

# --- execution ------------------------------------------------------------
STUB_CODE = [
    0xe321f0df,  # 00 msr cpsr_c, #0xdf (SYS)
    0xe1a00000,  # 04 [set SYS sp/lr/r8-r12]
    0xe321f0d2,  # 08 msr cpsr_c, #0xd2 (IRQ)
    0xe1a00000,  # 0c [set IRQ sp/lr, r0]
    0xe16ff000,  # 10 msr spsr_cxsf, r0
    0xe321f0d1,  # 14 msr cpsr_c, #0xd1 (FIQ)
    0xe1a00000,  # 18 [set FIQ sp/lr, r0]
    0xe16ff000,  # 1c msr spsr_cxsf, r0
    0xe1a00000,  # 20
]

def machine(armed=True):
    u = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    for addr, size in [(0, 0x10000), (0x08000000, 0x4000000),
                       (STUB, 0x1000), (TIMER, 0x1000), (VIC0, 0x1000),
                       (0x48000000, 0x4000000), (0x62000000, 0x40000)]:
        u.mem_map(addr, size)
    for addr, data, _ in segments:
        u.mem_write(addr, data)
    for i, w in enumerate(STUB_CODE):
        word(u, STUB + 4 * i, w)
    word(u, INT_PC, 0xe1a00000)
    word(u, sym('fiq_armed'), 1 if armed else 0)
    word(u, sym('current_tick'), 5000)
    word(u, sym('fiq_watch'), 5000)                  # as hang_catch_init
    word(u, VIC0 + 0x0c, 1 << 7)                     # line 7 routed to FIQ
    u.emu_start(STUB, STUB + 4)
    u.reg_write(UC_ARM_REG_SP, SYS_SP)
    u.reg_write(UC_ARM_REG_LR, SYS_LR)
    for r, v in zip(HIGH_REGS, HIGH):
        u.reg_write(r, v)
    u.emu_start(STUB + 4, STUB + 0xc)
    u.reg_write(UC_ARM_REG_SP, IRQ_SP)
    u.reg_write(UC_ARM_REG_LR, IRQ_LR)
    u.reg_write(UC_ARM_REG_R0, IRQ_SPSR)
    u.emu_start(STUB + 0xc, STUB + 0x14)
    return u

def fiq(u, cpsr=0x9f):
    """One FIQ taken from SYS mode with the given CPSR. Returns 'return' or
    'show' and checks the state each leaves."""
    u.reg_write(UC_ARM_REG_CPSR, (u.reg_read(UC_ARM_REG_CPSR) & ~0xff) | 0xdf)
    u.emu_start(STUB + 0x14, STUB + 0x18)
    u.reg_write(UC_ARM_REG_SP, sym('_fiqstackend'))
    u.reg_write(UC_ARM_REG_LR, INT_PC + 4)
    u.reg_write(UC_ARM_REG_R0, cpsr)
    u.emu_start(STUB + 0x18, STUB + 0x20)
    show = sym('hang_catch_fiq_show')
    hit = []
    def hook(uc, address, size, user):
        if address == show:
            hit.append(address)
            uc.emu_stop()
    h = u.hook_add(UC_HOOK_CODE, hook)
    u.emu_start(sym('fiq_handler'), INT_PC, count=20000)
    u.hook_del(h)
    pc = u.reg_read(UC_ARM_REG_PC)
    if hit:
        c = u.reg_read(UC_ARM_REG_CPSR)
        assert c & 0x1f == 0x11 and c & 0xc0 == 0xc0, hex(c)
        assert u.reg_read(UC_ARM_REG_SP) == rd(u, sym('hang_fiq_stack_end'))
        assert u.reg_read(UC_ARM_REG_SP) == sym('fiq_stack') + 2048
        return 'show'
    assert pc == INT_PC, hex(pc)
    c = u.reg_read(UC_ARM_REG_CPSR)
    assert c & 0xff == cpsr, hex(c)                 # interrupted mode back
    assert u.reg_read(UC_ARM_REG_SP) == SYS_SP
    assert u.reg_read(UC_ARM_REG_LR) == SYS_LR
    assert [u.reg_read(r) for r in HIGH_REGS] == HIGH
    return 'return'

F = sym('freeze')
cases = []

# A: IRQs off. The tick never moves; the 30th period trips.
u = machine()
results = [fiq(u) for _ in range(30)]
assert results[:29] == ['return'] * 29 and results[29] == 'show', results
fz = [rd(u, F + 4 * i) for i in range(11)]
assert fz[0] == 1                                    # HANG_WATCH_IRQS_OFF
assert fz[1] == INT_PC and fz[2] == 0x9f
assert fz[3] == SYS_SP and fz[4] == SYS_LR
assert fz[5] == IRQ_SP and fz[6] == IRQ_LR and fz[7] == IRQ_SPSR
assert fz[9] == 5000
# The screen: run hang_catch_fiq_show from where the FIQ left it, with the
# drawing calls as recording stubs, up to system_exception_wait.
STUBS = ['panic_screen_enter', 'lcd_set_viewport', 'lcd_set_backdrop',
         'lcd_set_drawinfo', 'lcd_clear_display', 'lcd_setfont', 'lcd_puts',
         'lcd_putsf', 'lcd_update']
stub_at = {sym(n): n for n in STUBS if syms.get(n) is not None}
drawn = []
def cstr(at):
    return bytes(u.mem_read(at, 80)).split(b'\0', 1)[0].decode('latin-1')
def draw_hook(uc, address, size, user):
    name = stub_at.get(address)
    if name:
        if name in ('lcd_puts', 'lcd_putsf'):
            drawn.append(cstr(uc.reg_read(UC_ARM_REG_R2)))
        else:
            drawn.append(name)
        uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
h = u.hook_add(UC_HOOK_CODE, draw_hook)
u.emu_start(sym('hang_catch_fiq_show'), sym('system_exception_wait'),
            count=200000)
u.hook_del(h)
assert u.reg_read(UC_ARM_REG_PC) == sym('system_exception_wait')
assert drawn[0] == 'panic_screen_enter', drawn[:3]
texts = [d for d in drawn if not d.startswith(('lcd_', 'panic_'))]
assert texts[0].startswith('*FREEZE* (') and texts[1] == 'IRQs off 3s: tick 5000 stopped'
assert any(t.startswith('thread  s pr lr') for t in texts)
assert drawn.count('lcd_update') == 2
cases.append({'case': 'irqs off', 'trip_period': 30, 'pc': hex(fz[1]),
              'cpsr': hex(fz[2]), 'sp': hex(fz[3]), 'lr': hex(fz[4]),
              'irq_sp': hex(fz[5]), 'irq_lr': hex(fz[6]),
              'irq_spsr': hex(fz[7]), 'screen': texts[:6],
              'screen_lines': len(texts)})

# B: the tick moves every period: never trips
u = machine()
for i in range(1000):
    word(u, sym('current_tick'), 5000 + 10 * (i + 1))
    assert fiq(u, 0x1f) == 'return'
cases.append({'case': 'tick running', 'periods': 1000, 'tripped': False})

# C: disarmed (before hang_catch_init, after hang_catch_stop)
u = machine(armed=False)
for _ in range(100):
    assert fiq(u) == 'return'
assert rd(u, F) == 0
cases.append({'case': 'disarmed', 'periods': 100, 'tripped': False})

# D: a user timer F interrupt pending on the shared line: back to IRQ
u = machine()
word(u, TIMER + 0xc0, 1 << 12)                       # TFCON: INT0 enabled
word(u, TIMER + 0xc4, 1)                             # TFCMD: running
word(u, TIMER + 0x118, 1 << 16)                      # TSTAT: F INT0
assert fiq(u) == 'return'
assert rd(u, VIC0 + 0x0c) & (1 << 7) == 0
assert rd(u, sym('fiq_watch') + 4) == 0              # not counted
cases.append({'case': 'timer F pending', 'line7_to_irq': True})

# E: UI runnable, never switched in, while the tick runs: trips just past
# 5 s. The state byte's offset and STATE_RUNNING's value are read from the
# compiled check itself (its only ldrb-immediate, then cmp #imm), so a wrong
# reading cannot pass: the watch would then never trip.
ws = [w for _, w in words('hang_catch_fiq')]
pairs = [(ws[i] & 0xfff, ws[j] & 0xff) for i in range(len(ws))
         for j in (i + 1, i + 2) if j < len(ws)
         and ws[i] & 0x0ff00000 == 0x05d00000           # ldrb rd, [rn, #imm]
         and ws[j] & 0x0ff00000 == 0x03500000           # cmp rd, #imm
         and (ws[i] >> 12) & 15 == (ws[j] >> 16) & 15
         and ws[j] & 0xff != 0]                         # not a flag test
assert len(pairs) == 1, pairs
state_off, running = pairs[0]
u = machine()
UI = 0x0bf00000
u.mem_write(UI, bytes(256))
u.mem_write(UI + state_off, bytes([running]))
word(u, sym('hang_ui_thread'), UI)
word(u, sym('hang_ui_ran'), 5000)
trip = None
for i in range(200):
    word(u, sym('current_tick'), 5000 + 10 * (i + 1))
    if fiq(u, 0x1f) == 'show':
        trip = i + 1
        break
assert trip == 51, trip                              # tick 5510 > 5000 + 500
assert rd(u, F) == 2                                 # HANG_WATCH_UI_STARVED
cases.append({'case': 'ui starved', 'trip_period': trip,
              'state_offset': state_off, 'state_running': running})

# F: the same UI thread blocked (any other state) is not starved
u = machine()
u.mem_write(UI, bytes(256))
u.mem_write(UI + state_off, bytes([running + 1]))   # STATE_BLOCKED
word(u, sym('hang_ui_thread'), UI)
word(u, sym('hang_ui_ran'), 5000)
for i in range(600):
    word(u, sym('current_tick'), 5000 + 10 * (i + 1))
    assert fiq(u, 0x1f) == 'return'
cases.append({'case': 'ui blocked', 'periods': 600, 'tripped': False})

result = {'elf': str(a.elf), 'static': static, 'cases': cases}
text = json.dumps(result, indent=2)
if a.out:
    a.out.write_text(text + '\n')
print(text)
print('arm_freeze_watch: ok')
