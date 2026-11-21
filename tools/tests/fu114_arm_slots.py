#!/usr/bin/env python3
"""Replay the iPod 6G thread budget in the linked ARM kernel with USB/serial fixtures."""
import argparse
import json
from pathlib import Path
import struct
import sys

from elftools.elf.elffile import ELFFile
try:
    import unicorn
except ModuleNotFoundError:
    root = Path.home() / ".cache/uv/archive-v0"
    sys.path.insert(0, str(next(root.glob("*/unicorn")).parent))
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm_const import (
    UC_CPU_ARM_926, UC_ARM_REG_CPSR, UC_ARM_REG_PC, UC_ARM_REG_SP,
    UC_ARM_REG_LR, UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
)

BOOT = (
    ("PMU", "pmu_thread_stack"),
    ("scroll", "scroll_stack"),
    ("mikey", "mikey_stack"),
    ("podlink", None),
    ("usb", "usb_stack"),
    ("cider-cache", "disk_stack"),
    ("backlight", "backlight_stack"),
    ("power", "power_stack"),
    ("ata", "storage_thread_stack"),
    ("tagcache", "tagcache_stack"),
    ("bgtask", "bg_stack"),
    ("playlist cachectrl", "playlist_stack"),
    ("codec", "codec_stack"),
    ("audio", "audio_stack"),
    ("buffering", "buffering_stack"),
    ("voice", "voice_stack"),
    ("usb-log", "writer_stack"),
)
STOP, STACK, SCRATCH = 0x10000000, 0x10020000, 0x10030000
ARGS = (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)
CREATE_FROZEN = 1


class Firmware:
    def __init__(self, elf_path):
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        self.uc.ctl_set_cpu_model(UC_CPU_ARM_926)
        for address, size in ((0, 0x40000), (0x08000000, 0x04000000),
                              (0x10000000, 0x400000)):
            self.uc.mem_map(address, size)
        self.syms, self.sizes = {}, {}
        with elf_path.open("rb") as f:
            elf = ELFFile(f)
            for segment in elf.iter_segments():
                if segment["p_type"] == "PT_LOAD" and segment["p_filesz"]:
                    self.uc.mem_write(segment["p_vaddr"], segment.data())
            for sym in elf.get_section_by_name(".symtab").iter_symbols():
                if sym["st_shndx"] != "SHN_UNDEF" and sym["st_value"]:
                    self.syms[sym.name] = sym["st_value"]
                    self.sizes[sym.name] = sym["st_size"]
        self.capacity = self.sizes["__threads"] // 4
        self.live = {}
        self.attempts = []
        self.panics = []
        self.usb_inserted = 0
        self.hook("serial_bitrate", self.ignore)
        self.hook("usb_detect", self.detect_usb)
        self.hook("panicf", self.panic)
        self.hook("create_thread", self.thread_attempt)
        self.call("init_threads")
        self.call("init_queues")
        self.call("queue_init", self.syms["usb_queue"], 1)

    def put(self, address, value):
        self.uc.mem_write(address, struct.pack("<I", value & 0xffffffff))

    def get(self, address):
        return struct.unpack("<I", self.uc.mem_read(address, 4))[0]

    def chars(self, at):
        return bytes(self.uc.mem_read(at, 128)).split(b"\0", 1)[0].decode(errors="replace")

    def hook(self, name, fn):
        address = self.syms[name] & ~1
        self.uc.hook_add(UC_HOOK_CODE, fn, begin=address, end=address)

    def ignore(self, machine, *_):
        machine.reg_write(UC_ARM_REG_R0, 0)
        machine.reg_write(UC_ARM_REG_PC, machine.reg_read(UC_ARM_REG_LR))

    def detect_usb(self, machine, *_):
        machine.reg_write(UC_ARM_REG_R0, self.usb_inserted)
        machine.reg_write(UC_ARM_REG_PC, machine.reg_read(UC_ARM_REG_LR))

    def panic(self, machine, *_):
        self.panics.append(self.chars(machine.reg_read(UC_ARM_REG_R0)))
        machine.reg_write(UC_ARM_REG_PC, STOP)

    def thread_attempt(self, machine, *_):
        sp = machine.reg_read(UC_ARM_REG_SP)
        destination = self.chars(self.get(sp))
        self.attempts.append(dict(name=destination, stack_bytes=machine.reg_read(UC_ARM_REG_R2),
                                  mode=machine.reg_read(UC_ARM_REG_CPSR) & 31))

    def call(self, name, *args, mode=0x13):
        self.uc.reg_write(UC_ARM_REG_CPSR, mode)
        self.uc.reg_write(UC_ARM_REG_SP, STACK)
        self.uc.reg_write(UC_ARM_REG_LR, STOP)
        for reg, val in zip(ARGS, args):
            self.uc.reg_write(reg, val & 0xffffffff)
        for i, val in enumerate(args[4:]):
            self.put(STACK + i * 4, val)
        self.uc.emu_start(self.syms[name], STOP, count=1_000_000)
        assert self.uc.reg_read(UC_ARM_REG_PC) == STOP, name
        return self.uc.reg_read(UC_ARM_REG_R0)

    def reserve(self, name, stack=None):
        # Boot service bodies are dormant. Only real kernel allocation and
        # the iAP interrupt path execute; actual UI is exercised in SDL.
        index = len(self.attempts)
        address = SCRATCH + 0x1000 + 0x5000 * index
        size = self.sizes.get(stack, 0) or (12288 if name == "main" else 8192)
        tag = SCRATCH + index * 96
        self.uc.mem_write(tag, name.encode() + b"\0")
        tid = self.call("create_thread", self.syms["yield"], address, size, CREATE_FROZEN, tag, 20)
        if tid:
            self.live[name] = tid
        return tid

    def release(self, name):
        tid = self.live.pop(name)
        slot = self.get(self.syms["__threads"] + 4 * (tid & 255))
        self.call("thread_free", slot)

    def table(self):
        entries = {}
        for slot in range(self.capacity):
            at = SCRATCH - 4096
            self.uc.mem_write(at, bytes(64))
            if self.call("thread_get_debug_info", slot, at) == 1:
                entries[slot] = self.chars(at + 4)
        return dict(threads=entries, used=len(entries), spare=self.capacity-len(entries),
                    maxthreads=self.capacity)

    def boot(self, with_dircache=False, setup_iap=True):
        for name, stack in BOOT:
            if name == "usb-log" and setup_iap:
                self.call("iap_setup", 0)
            assert self.reserve(name, stack), (name, self.table())
        if with_dircache:
            assert self.reserve("dircache", "dircache_stack")
        return self.table()

    def rx_sync(self):
        self.call("iap_getc", 0xff, mode=0x12)
        return self.table()


def run(elf, legacy):
    boot = Firmware(elf)
    eager_iap = not legacy and "iap_retry_at" not in boot.syms
    boot.usb_inserted = 1
    boot.boot(with_dircache=True)
    boot.call("usb_start_monitoring")
    # The byte received before the gate opens cannot create a thread.
    early = Firmware(elf)
    assert early.rx_sync()["used"] == 1 and not early.attempts
    boot_before = boot.table()
    if eager_iap:
        assert not boot.uc.mem_read(boot.syms["iap_started"], 1)[0]
    boot_after = boot.rx_sync()

    runtime = Firmware(elf)
    normal_boot = runtime.boot()
    runtime.call("usb_start_monitoring")
    if legacy:
        assert runtime.reserve("glyph read", "worker_stack")
    peak_ui = runtime.table()
    carousel = runtime.reserve("Picture load thread")
    operational = runtime.table()
    runtime.usb_inserted = 1
    runtime.call("usb_status_event", 1, mode=0x12)
    usb_only = runtime.table()
    assert usb_only == operational
    assert ("iap" in usb_only["threads"].values()) == eager_iap
    if eager_iap:
        assert not runtime.uc.mem_read(runtime.syms["iap_started"], 1)[0]
    runtime_after = runtime.rx_sync()

    retry = None
    if legacy:
        assert boot_before["spare"] == 0 and boot.panics == ["Could not create iap thread"]
        assert not carousel and runtime.panics == ["Could not create iap thread"]
    else:
        assert carousel and not boot.panics and not runtime.panics
        for table in (boot_after, runtime_after):
            assert "iap" in table["threads"].values() and table["spare"] >= 2, table
        assert "glyph read" not in operational["threads"].values()
        full = Firmware(elf)
        full.boot(with_dircache=True, setup_iap=not eager_iap)
        for n in range(full.table()["spare"]):
            assert full.reserve(f"optional {n}")
        if eager_iap:
            full.call("iap_setup", 0)
        full.rx_sync()
        assert not full.panics
        assert "iap" not in full.table()["threads"].values()
        full.release("optional 0")
        if eager_iap:
            attempts = [attempt for attempt in boot.attempts if attempt["name"] == "iap"]
            assert len(attempts) == 1 and attempts[0]["mode"] == 0x13, attempts
            assert boot_after == boot_before and runtime_after == usb_only
            for firmware in (boot, runtime):
                assert firmware.uc.mem_read(firmware.syms["iap_started"], 1)[0]
            full.call("iap_setup", 0)
        else:
            full.put(full.syms["current_tick"], 101)
        retry = full.rx_sync()
        assert "iap" in retry["threads"].values() and not full.panics, retry
    return dict(boot_without_cable=normal_boot, cable_already_present_before_rx=boot_before,
                cable_already_present_after_rx=boot_after, after_glyphs_and_blur=peak_ui,
                cover_flow_with_loader=operational, runtime_usb_without_serial_sync=usb_only,
                runtime_usb_and_serial_sync=runtime_after,
                iap_attempt=next(attempt for attempt in boot.attempts if attempt["name"] == "iap"),
                boot_panics=boot.panics, runtime_panics=runtime.panics,
                optional_slot_failure_retry=retry)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path)
    parser.add_argument("--legacy", action="store_true")
    args = parser.parse_args()
    print(json.dumps(run(args.elf, args.legacy), indent=2))
