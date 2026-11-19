#!/usr/bin/env python3
"""Run the FU88 exception vectors and recovery file writer with ARM emulation."""

import argparse
from pathlib import Path
import struct

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_WRITE
from unicorn import arm_const as arm


STOP, STR, WDT = 0x10000000, 0x10000100, 0x3C800000
ALIAS = 0x40000000
IRAM1 = [(0x22020000, 0x20000), (0x62020000, 0x20000)]  # sm1 P/X/Y + parameter page
REGS = [getattr(arm, f"UC_ARM_REG_R{i}") for i in range(13)]
CRASH_PATH = "/.rockbox/crash-native.txt"
CRASH_TMP = "/.rockbox/crash-native.tmp"
CRASH_DIR = "/.rockbox/research/"


class Firmware:
    def __init__(self, elf_path):
        with elf_path.open("rb") as stream:
            elf = ELFFile(stream)
            self.segments = [(s["p_vaddr"], s.data()) for s in elf.iter_segments()
                             if s["p_type"] == "PT_LOAD" and s["p_filesz"]]
            self.symbols = {s.name: (s["st_value"], s["st_size"])
                            for s in elf.get_section_by_name(".symtab").iter_symbols()}

    def addr(self, name):
        return self.symbols[name][0]

    def machine(self, retained=()):
        return Machine(self, retained)


class Machine:
    def __init__(self, firmware, retained):
        self.firmware = firmware
        self.addr = firmware.addr
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        self.uc.ctl_set_cpu_model(arm.UC_CPU_ARM_926)
        for begin, length in [(0, 0x10000), (0x08000000, 0x00800000),
                              (0x48000000, 0x00800000), (0x10000000, 0x1000),
                              (WDT, 0x1000)] + IRAM1:
            self.uc.mem_map(begin, length)
        for begin, data in firmware.segments:
            self.uc.mem_write(begin, data)
        for begin, data in retained:
            self.uc.mem_write(begin, data)
        self.uc.mem_write(STR, b"main\0Stkov audio\0")
        self.files = {}
        self.pending = ""
        self.short_write = False
        self.reset = None
        self.hooks = {
            self.addr("clockgate_enable"): lambda: 0,
            self.addr("thread_current_name"): lambda: STR,
            self.addr("rbfs_mkdir"): lambda: 0,
            self.addr("rbfs_open"): self.open,
            self.addr("rbfs_write"): self.write,
            self.addr("rbfs_fsync"): lambda: 0,
            self.addr("rbfs_close"): lambda: 0,
            self.addr("rbfs_rename"): self.rename,
            self.addr("rbfs_remove"): self.remove,
        }
        self.uc.hook_add(UC_HOOK_CODE, self.code)
        self.uc.hook_add(UC_HOOK_MEM_WRITE, self.reset_write, begin=WDT, end=WDT + 3)
        self.iram1_writes = []
        for begin, size in IRAM1:
            self.uc.hook_add(UC_HOOK_MEM_WRITE, self.iram1_write, begin=begin,
                             end=begin + size - 1)

    def get(self, reg):
        return self.uc.reg_read(reg)

    def put(self, reg, val):
        self.uc.reg_write(reg, val)

    def word(self, address, value):
        self.uc.mem_write(address, struct.pack("<I", value))

    def cstring(self, address):
        return bytes(self.uc.mem_read(address, 128)).split(b"\0", 1)[0].decode()

    def open(self):
        self.pending = self.cstring(self.get(REGS[0]))
        self.files[self.pending] = bytearray()
        return 3

    def write(self):
        size = self.get(REGS[2])
        if self.pending == CRASH_TMP and self.short_write:
            self.short_write = False
            return 0
        self.files[self.pending].extend(self.uc.mem_read(self.get(REGS[1]), size))
        return size

    def rename(self):
        old, new = [self.cstring(self.get(reg)) for reg in REGS[:2]]
        self.files[new] = self.files.pop(old)
        return 0

    def remove(self):
        self.files.pop(self.cstring(self.get(REGS[0])), None)
        return 0

    def code(self, uc, pc, size, data):
        if pc == STOP:
            uc.emu_stop()
        elif pc in self.hooks:
            result = self.hooks[pc]()
            self.put(REGS[0], result)
            self.put(arm.UC_ARM_REG_PC, self.get(arm.UC_ARM_REG_LR))

    def iram1_write(self, uc, access, offset, size, value, data):
        self.iram1_writes.append((hex(offset), size, hex(uc.reg_read(arm.UC_ARM_REG_PC))))

    def reset_write(self, uc, access, offset, size, value, data):
        self.reset = (offset, size, value)
        uc.emu_stop()

    def call(self, symbol, *args):
        self.put(arm.UC_ARM_REG_CPSR, 0x1F)
        self.put(arm.UC_ARM_REG_SP, self.addr("stackend") - 64)
        self.put(arm.UC_ARM_REG_LR, STOP)
        for reg, value in zip(REGS, args):
            self.put(reg, value)
        self.uc.emu_start(self.addr(symbol), STOP + 4, count=500_000)

    def exception(self, vector, mode, fault_pc, offset, thumb=False, stop=STOP + 4):
        self.put(arm.UC_ARM_REG_CPSR, 0x1F)
        self.put(arm.UC_ARM_REG_SP, self.addr("stackend") - 128)
        self.put(arm.UC_ARM_REG_LR, self.addr("crumbs_boot") + 4)
        self.word(self.addr("stackend") - 128, self.addr("crumbs_report"))
        self.put(arm.UC_ARM_REG_CPSR, 0xC0 | mode)
        self.put(arm.UC_ARM_REG_SPSR, 0x3F if thumb else 0x1F)
        self.put(arm.UC_ARM_REG_SP, self.addr("_irqstackend"))
        self.put(arm.UC_ARM_REG_LR, fault_pc + offset)
        for index, reg in enumerate(REGS):
            self.put(reg, 0x12300000 + index)
        self.uc.cpr_write(15, 0, 5, 0, 0, 0, False, 5)
        self.uc.cpr_write(15, 0, 6, 0, 0, 0, False, 0x34567000)
        self.uc.emu_start(vector, stop, count=500_000)

    def retained(self):
        ranges = [(ALIAS + self.addr("_persiststart"),
                   self.addr("_persistend") - self.addr("_persiststart"))]
        return [(base, bytes(self.uc.mem_read(base, size))) for base, size in ranges]


def test_watchdog(firmware):
    # Recover the scheduler state offset from the compiled byte loads and
    # comparisons, as the default firmware's ARM watchdog probe does.
    code, size = firmware.symbols["hang_catch_fiq"]
    machine = firmware.machine()
    words = struct.unpack(f"<{size // 4}I", machine.uc.mem_read(code, size))
    states = {(load & 0xfff, compare & 0xff)
              for i, load in enumerate(words)
              for compare in words[i + 1:i + 3]
              if load & 0x0ff00000 == 0x05d00000
              and compare & 0x0ff00000 == 0x03500000
              and (load >> 12) & 15 == (compare >> 16) & 15
              and compare & 0xff != 0}
    assert len(states) == 1, states
    state, running = states.pop()
    audio = STR + 0x200
    machine.word(firmware.addr("hang_audio_thread"), audio)
    machine.word(firmware.addr("fiq_armed"), 1)
    pc = firmware.addr("crumbs_report")

    def tick(number):
        machine.word(firmware.addr("current_tick"), number)
        machine.call("hang_catch_fiq", pc, 0x1f)
        return machine.get(REGS[0])

    machine.uc.mem_write(audio + state, bytes([running + 1]))
    for number in range(1000, 1800, 10):
        assert tick(number) == 0, number
    machine.uc.mem_write(audio + state, bytes([running]))
    for number in range(1800, 2250, 10):
        assert tick(number) == 0, number
    machine.word(firmware.addr("hang_audio_ran"), 2249)
    for number in range(2250, 2760, 10):
        assert tick(number) == 0, number
    assert tick(2760) == 1
    assert struct.unpack("<I", machine.uc.mem_read(firmware.addr("freeze"), 4)) == (3,)
    print("PASS watchdog: idle audio, scheduler progress, then runnable starvation")

    disabled = firmware.machine()
    checked = []
    def marker():
        checked.append(disabled.cstring(disabled.get(REGS[0])))
        return 1
    disabled.hooks[firmware.addr("rbfs_file_exists")] = marker
    disabled.call("hang_catch_init")
    assert checked == [CRASH_DIR + "watchdog.off"]
    assert disabled.uc.mem_read(firmware.addr("fiq_armed"), 1) == b"\0"
    print("PASS watchdog: disable marker leaves timer disarmed")


def audit_iram1(firmware, machines):
    """Check diagnostics for IRAM1 writes and the old record addresses."""
    for machine in machines:
        assert not machine.iram1_writes, machine.iram1_writes[:8]
    machine = firmware.machine()
    machine.call("crumbs_boot")
    for _ in range(40):
        machine.call("crumb", 3, 0x1234)
    machine.call("crumbs_clean")
    machine.call("crumbs_report")
    assert not machine.iram1_writes, machine.iram1_writes[:8]
    for begin, data in firmware.segments:
        for offset in range(0, len(data) - 3, 4):
            word = struct.unpack_from("<I", data, offset)[0]
            assert word not in (0x2203F600, 0x6203F600, 0x2203F800, 0x6203F800), hex(begin + offset)
    print("PASS IRAM1 audit: boot, crumbs, clean, report, faults and watchdogs "
          "never write 0x22020000..0x2203ffff; no old IRAM1 record addresses in the image")


def shell_on_target(firmware):
    """The Thumb shell core of the real image answers requests."""
    machine = firmware.machine()
    base = firmware.addr("audiobuffer")
    req, out, flag = base, base + 0x1000, base + 0x6000
    for line, want in [(b"ping", b"RS\t1\tq\tOK\tpong"),
                       (b"poke 4 0x3c300000 1 --force",
                        b"RS\t1\tq\tERR\tguard: refused (flash/bootloader path)"),
                       (b"poke 4 0x38300000 1", b"RS\t1\tq\tERR\tguard: protected range, add --force"),
                       (b"sm1 nope", b"RS\t1\tq\tERR\tusage: sm1 on|off|status|regs|ack|wait|feed|load|param|slots|release|pcm")]:
        data = b"RS\t1\tq\t" + line
        machine.uc.mem_write(req, data)
        machine.call("rs_exec", req, len(data), out, flag)
        n = machine.get(REGS[0])
        got = bytes(machine.uc.mem_read(out, n))
        assert got == want, (line, got)
    print("PASS ARM shell core: ping, NOR refusal, LCD soft guard, sm1 usage")


def verify(firmware):
    shell_on_target(firmware)
    machines = []
    for name, kind, vector, mode, padding, thumb in [
        ("undefined ARM", 2, 0x04, 0x1b, 4, False),
        ("undefined Thumb", 2, 0x04, 0x1b, 2, True),
        ("prefetch abort", 3, 0x0c, 0x17, 4, False),
        ("data abort", 4, 0x10, 0x17, 8, False),
        ("stack overflow panic", 1, None, None, 0, False),
    ]:
        before = firmware.machine()
        machines.append(before)
        before.call("crumbs_boot")
        before.word(firmware.addr("current_tick"), 1234)
        before.call("pfu_research_ping")
        fault_pc = firmware.addr("crumbs_report")
        if vector is None:
            # The shell's last command goes into the record
            before.uc.mem_write(firmware.addr("rs_last_command"), b"sm1 wait 50\0")
            before.call("panicf", STR + 5)
        else:
            before.exception(vector, mode, fault_pc, padding, thumb=thumb)
        assert before.reset == (WDT, 4, 0x100000), (name, before.reset)

        after = firmware.machine(before.retained())
        machines.append(after)
        after.call("crumbs_boot")
        after.files[CRASH_PATH] = b"previous report"
        after.short_write = True
        after.call("crumbs_report")
        assert after.files[CRASH_PATH] == b"previous report", name
        assert CRASH_TMP not in after.files

        # Another fault before successful export must keep the first record.
        after.call("panicf", STR + 5)
        assert after.reset == (WDT, 4, 0x100000)
        after.call("crumbs_report")
        native = after.files[CRASH_PATH].decode()
        assert native.startswith("PodBox native crash\nFirmware: ")
        heading = "Panic" if vector is None else "ARM exception"
        assert f"Boot: 1\n{heading}: " in native
        text = after.files[CRASH_DIR + "crash-1.txt"].decode()
        assert native.endswith(text), native
        assert f"boot=1 kind={kind} ticks=1234 hz=100" in text, text
        assert "thread=main\n" in text and "UNCLEAN copies: DRAM\n" in text, text
        command = "sm1 wait 50" if vector is None else "ping"
        assert f"command={command}\n" in text and "rs-ping" in text, text
        if vector is None:
            assert "message=Stkov audio\n" in text, text
        else:
            assert f"pc={fault_pc:08x}" in text, text
            assert f"cpsr={0x3f if thumb else 0x1f:08x}" in text, text
            assert "candidate=" in text, text
            assert all(f"r{i}=123000{i:02x}" in text for i in range(13)), text
            if kind == 4:
                assert "far=34567000 fsr=00000005" in text, text
        assert (CRASH_DIR + "log.txt") in after.files, after.files.keys()
        after.call("crumbs_report")
        assert after.files[CRASH_PATH].decode() == native
        print(f"PASS {name}: reset, native export, numbered export, short-write retry")

    for bits in [8, 16, 32]:
        machine = firmware.machine()
        pc = firmware.addr(f"safe_read{bits}_faulty_addr")
        machine.exception(0x10, 0x17, pc, 8, stop=pc + 4)
        assert machine.get(arm.UC_ARM_REG_PC) == pc + 4
        assert machine.get(arm.UC_ARM_REG_CPSR) & 0xff == 0x1f
        assert machine.reset is None
        mark = machine.uc.mem_read(firmware.addr("was_aborted"), 4)
        assert struct.unpack("<I", mark) == (1,)
    print("PASS recoverable safe_read faults resume without reset")

    for reason, message in enumerate(("Tick stalled", "UI starved", "Audio starved"), 1):
        before = firmware.machine()
        before.call("crumbs_boot")
        pc = firmware.addr("crumbs_report")
        entry = firmware.addr("freeze")
        registers = [reason, pc, 0x1f, firmware.addr("stackend") - 128, pc + 4]
        before.uc.mem_write(entry, struct.pack("<5I", *registers))
        before.call("hang_catch_fiq_show")
        assert before.reset == (WDT, 4, 0x100000)
        after = firmware.machine(before.retained())
        after.call("crumbs_boot")
        after.call("crumbs_report")
        text = after.files[CRASH_PATH].decode()
        assert f"Freeze: {message}\n" in text and "kind=6" in text, text
        machines += [before, after]
        print(f"PASS watchdog capture: {message}")
    audit_iram1(firmware, machines)

    test_watchdog(firmware)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--elf", required=True, type=Path)
    verify(Firmware(parser.parse_args().elf))
