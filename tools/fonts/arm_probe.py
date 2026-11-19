#!/usr/bin/env python3
"""Count actual iPod 6G ARM text instructions with an in-memory file adapter."""
import argparse
import json
from pathlib import Path
from zipfile import ZipFile

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_BLOCK, UC_HOOK_CODE
from unicorn.arm_const import UC_CPU_ARM_1176, UC_ARM_REG_CPSR, UC_ARM_REG_LR, UC_ARM_REG_PC
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_SP

SAMPLES = {
    "home-list": ["专辑封面", "音乐", "艺术家像", "播放列表", "播放报告", "文件浏览", "设置", "正在播放"],
    "status": ["PodBox", "20:26"],
    "cover-flow": ["Welcome to New York", "Taylor Swift", "1989 (Taylor’s Version)"],
    "dialog": ["将播放切换到另一设备？", "目前的播放进度会保留。", "取消", "继续"],
    "agents-usage": ["Agents", "使用情况", "Workspace 1 · Active", "Token usage", "0123456789",
                     "AVATAR Toyota WAVE"],
    "lyrics": ["The lights are so bright", "But they never blind me, me",
               "Welcome to New York (New York)", "It’s been waiting for you",
               "Welcome to New York", "封肖师字目（你好，世界。）"],
}


def arm_probe(elf_path, font_data):
    uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    uc.ctl_set_cpu_model(UC_CPU_ARM_1176)
    uc.mem_map(0, 0x10000)
    uc.mem_map(0x08000000, 0x04000000)
    uc.mem_map(0x10000000, 0x00400000)
    uc.reg_write(UC_ARM_REG_CPSR, 0x13)
    with elf_path.open("rb") as file:
        elf = ELFFile(file)
        for segment in elf.iter_segments():
            if segment["p_type"] == "PT_LOAD" and segment["p_filesz"]:
                uc.mem_write(segment["p_vaddr"], segment.data())
        syms = {s.name: s["st_value"] for s in elf.get_section_by_name(".symtab").iter_symbols()
                if s["st_shndx"] != "SHN_UNDEF"}

    arg_regs = (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)
    stop, str_address = 0x10000000, 0x10001000
    stack, pool = 0x100ff000, 0x10100000
    counters = {"instructions": 0, "reads": 0, "read_bytes": 0}
    descriptors = {}
    opened = 0

    def count(_uc, _address, n, _context):
        counters["instructions"] += n // 4

    uc.hook_add(UC_HOOK_BLOCK, count)

    def host_file(machine, address, _size, kind):
        nonlocal opened
        a, b, c = (machine.reg_read(r) for r in arg_regs[:3])
        if kind == "open":
            path = bytes(machine.mem_read(a, 256)).split(b"\0", 1)[0]
            opened += 1
            data = font_data.get(path.decode().rsplit("/", 1)[-1])
            if data is None:
                result = -1
            else:
                descriptors[opened] = (data, 0)
                result = opened
        elif kind == "close":
            descriptors.pop(a, None)
            result = 0
        else:
            data, offset = descriptors[a]
            if kind == "read":
                raw = data[offset:offset + c]
                machine.mem_write(b, raw)
                descriptors[a] = (data, offset + len(raw))
                counters["reads"] += 1
                counters["read_bytes"] += len(raw)
                result = len(raw)
            elif kind == "lseek":
                delta = b if b < 2 ** 31 else b - 2 ** 32
                result = (0, offset, len(data))[c] + delta
                descriptors[a] = (data, result)
            else:
                result = len(data)
        machine.reg_write(UC_ARM_REG_R0, result & 0xffffffff)
        machine.reg_write(UC_ARM_REG_PC, machine.reg_read(UC_ARM_REG_LR))

    for kind in ("open", "close", "read", "lseek", "ffilesize"):
        entry = syms["rbfs_" + kind]
        uc.hook_add(UC_HOOK_CODE, host_file, kind, begin=entry, end=entry)

    def call(name, *args):
        uc.reg_write(UC_ARM_REG_SP, stack)
        uc.reg_write(UC_ARM_REG_LR, stop)
        for reg, val in zip(arg_regs, args):
            uc.reg_write(reg, val & 0xffffffff)
        uc.emu_start(syms[name], stop, count=10000000)
        assert uc.reg_read(UC_ARM_REG_PC) == stop, name
        ret = uc.reg_read(UC_ARM_REG_R0)
        return ret if ret < 2 ** 31 else ret - 2 ** 32

    call("buflib_init", syms["core_ctx"], pool, 2 * 1024 * 1024)
    call("font_init")
    call("lcd_set_viewport", 0)
    call("lcd_set_foreground", 0xffff)
    call("lcd_set_background", 0)
    call("lcd_set_drawmode", 2)  # DRMODE_FG
    total_ram = call("core_available")
    ids = {}
    for name in font_data:
        uc.mem_write(str_address, ("/.rockbox/fonts/" + name).encode() + b"\0")
        index = call("font_load_ex", str_address, 0, 250 if name.startswith("18-") else 64)
        assert index >= 0, name
        ids[name] = index
    first = next(name for name in ids if name.startswith("18-"))
    small = next(name for name in ids if name.startswith("12-"))
    large = next((name for name in ids if name.startswith("20-")), first)
    index = next((name for name in ids if name.startswith("56-")), large)
    ram = total_ram - call("core_available")

    def draw(lines, fonts):
        for i, (line, name) in enumerate(zip(lines, fonts, strict=True)):
            call("lcd_setfont", ids[name])
            uc.mem_write(str_address, line.encode() + b"\0")
            call("lcd_putsxy", 4, (i * 24) % 200, str_address)

    def measure(lines, fonts):
        start = counters.copy()
        draw(lines, fonts)
        return {k: v - start[k] for k, v in counters.items()}

    result = {}
    for name, lines in {**SAMPLES, "index-box": [*"北A9"]}.items():
        if name == "status":
            faces = [small] * len(lines)
        elif name == "lyrics":
            # Charge both native sizes during the cross-fade.
            lines = lines + lines
            faces = [first] * (len(lines) // 2) + [large] * (len(lines) // 2)
        elif name == "index-box":
            faces = [index] * len(lines)
        else:
            faces = [first] * len(lines)
        cold = measure(lines, faces)
        warm = measure(lines, faces)
        result[name] = {"glyphs_requested": sum(map(len, lines)), "first": cold, "warm": warm}
        for sample in (cold, warm):
            # A stated CPI assumption is an estimate, not an instruction
            # emulator's timing. Disk latency and the rest of the UI are absent.
            sample["cpu_ms_at_3_cpi_54mhz"] = sample["instructions"] / 18000
            sample["cpu_ms_at_3_cpi_216mhz"] = sample["instructions"] / 72000
    return {"font_ram_delta": ram, "scenes": result}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("elf", type=Path)
    p.add_argument("fonts", type=Path)
    p.add_argument("--legacy", action="store_true", help="Use the baseline ZIP as the font source")
    args = p.parse_args()
    if args.legacy:
        names = ["18-noto-sans.fnt", "12-noto-sans.fnt"]
        with ZipFile(args.fonts) as archive:
            files = {name: archive.read(".rockbox/fonts/" + name) for name in names}
    else:
        names = ["18-noto-sans.pbf", "12-noto-sans.pbf", "20-noto-sans.pbf", "56-podbox-index.pbf"]
        files = {name: (args.fonts / name).read_bytes() for name in names}
    print(json.dumps(arm_probe(args.elf, files), ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
