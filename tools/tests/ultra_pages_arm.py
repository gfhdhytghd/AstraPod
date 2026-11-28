#!/usr/bin/env python3
"""Count the ARM instructions of Fruit Company Ultra's page compositors.

Runs, on Unicorn's ARM926 model and from the iPod Classic ELF, one frame of
each compositor with real page pictures, and Fruit Company's own compositor
for the same pose:

  push       two full lists, the upper page's edge at P (280, 160, 40,
             and the costliest edge for Ultra of every eighth column)
  split      two split menus
  mixed      a split menu and a full list
  np         a split menu and Now Playing, whole screen
  fade       the shutdown fade at half light (Fruit Company: ui_blit_dim)
  pane       one step of the pane dissolve over the area the two panes
             differ in (Fruit Company cuts, and does nothing per frame)

The count is the compositor's alone; presenting the frame costs a further
19,527 instructions and a 153,600-byte transfer (docs/podbox/
animation-engine.md), and the pane's own rectangle its share of that. Each
frame time is estimated as the document does: instructions * 4 cycles /
clock + bytes / 8 MiB/s, in refreshes of 1/66.96 s, plus 0.24.

Arguments: --build BUILD (a configured ipod6g build), --elf ELF,
--frames DIR (the motion1-1/fruit-company directory an ultra_pages_sim run
writes). Prints a table and writes --out JSON.
"""

import argparse
import json
import shlex
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

from elftools.elf.elffile import ELFFile
from PIL import Image
try:
    import unicorn  # noqa: F401
except ModuleNotFoundError:
    root = Path.home() / ".cache/uv/archive-v0"
    sys.path.insert(0, str(next(root.glob("*/unicorn")).parent))
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm_const import (
    UC_CPU_ARM_926, UC_ARM_REG_CPSR, UC_ARM_REG_LR, UC_ARM_REG_PC,
    UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
    UC_ARM_REG_SP)

W, H = 320, 240
FB = W * H * 2
STOP, STACK = 0x10000000, 0x10020000
BUFFERS = 0x20000000
ARGS = (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)
SUBMIT_INSTRUCTIONS = 19527
REFRESH_S = 1 / 66.96
FIELDS = ["back_page", "tween", "handle", "backdrop_handle", "pan_top",
          "target_front", "slide", "back_split", "front_split",
          "full_screen", "back_still", "still_backdrop", "reversed"]


def layout(build):
    """Offsets of struct ui_scene's fields, from the build's compiler."""
    root = Path(next(line.split("=", 1)[1].strip() for line in
                     (build / "Makefile").read_text().splitlines()
                     if line.startswith("export ROOTDIR=")))
    # -W rather than -B, which would remake the generated sources too.
    plan = subprocess.check_output(
        ["make", "-C", build, "-n", "-W", root / "apps-ipod/ui/scene.c",
         build / "apps-ipod/ui/scene.o", "V=1"],
        text=True, stderr=subprocess.DEVNULL)
    line = next(item for item in plan.splitlines()
                if " -c " in item and "ui/scene.c" in item)
    cmd = shlex.split(line)
    if cmd[0].endswith("ccache"):
        cmd = cmd[1:]
    cmd = cmd[:cmd.index("-c")]
    names = ["sizeof(struct ui_scene)"] + [
        f"offsetof(struct ui_scene, {name})" for name in FIELDS]
    with tempfile.TemporaryDirectory() as folder:
        folder = Path(folder)
        source = folder / "layout.c"
        source.write_text(
            "#include <stddef.h>\n#include " +
            json.dumps(str(root / "apps-ipod/ui/scene.h")) +
            "\nconst unsigned device_layout[]={" + ",".join(names) + "};\n")
        subprocess.run(cmd + ["-c", str(source), "-o", str(folder / "l.o")],
                       check=True)
        with (folder / "l.o").open("rb") as f:
            elf = ELFFile(f)
            sym = elf.get_section_by_name(".symtab").get_symbol_by_name(
                "device_layout")[0]
            data = elf.get_section(sym["st_shndx"]).data()
            values = struct.unpack_from(f"<{len(names)}I", data,
                                        sym["st_value"])
    out = dict(zip(FIELDS, values[1:]))
    out["size"] = values[0]
    return out


class Machine:
    def __init__(self, elf_path):
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        self.uc.ctl_set_cpu_model(UC_CPU_ARM_926)
        for address, size in ((0, 0x40000), (0x08000000, 0x04000000),
                              (0x10000000, 0x400000),
                              (BUFFERS, 0x200000)):
            self.uc.mem_map(address, size)
        self.syms = {}
        with elf_path.open("rb") as f:
            elf = ELFFile(f)
            for segment in elf.iter_segments():
                if segment["p_type"] == "PT_LOAD" and segment["p_filesz"]:
                    self.uc.mem_write(segment["p_vaddr"], segment.data())
            source = None
            for sym in elf.get_section_by_name(".symtab").iter_symbols():
                if sym["st_info"]["type"] == "STT_FILE":
                    source = sym.name
                elif sym["st_value"]:
                    self.syms.setdefault(sym.name, sym["st_value"])
                    self.syms[f"{source}:{sym.name}"] = sym["st_value"]
        self.count = 0
        self.uc.hook_add(UC_HOOK_CODE, self.tick)
        self.returns("ui_theme_ultra", 0)
        self.returns("menu_pane_shadow", 0)

    def tick(self, *_):
        self.count += 1

    def returns(self, name, value):
        def stub(machine, *_):
            machine.reg_write(UC_ARM_REG_R0, value)
            machine.reg_write(UC_ARM_REG_PC, machine.reg_read(UC_ARM_REG_LR))
        address = self.syms[name] & ~1
        self.uc.hook_add(UC_HOOK_CODE, stub, begin=address, end=address)

    def put(self, address, fmt, *values):
        self.uc.mem_write(address, struct.pack(fmt, *values))

    def call(self, name, *args):
        self.uc.reg_write(UC_ARM_REG_CPSR, 0x13)
        self.uc.reg_write(UC_ARM_REG_SP, STACK)
        self.uc.reg_write(UC_ARM_REG_LR, STOP)
        for reg, value in zip(ARGS, args):
            self.uc.reg_write(reg, value & 0xffffffff)
        for i, value in enumerate(args[4:]):
            self.put(STACK + 4 * i, "<I", value & 0xffffffff)
        self.count = 0
        self.uc.emu_start(self.syms[name], STOP, count=50_000_000)
        assert self.uc.reg_read(UC_ARM_REG_PC) == STOP, name
        return self.count - 1


def rgb565(image):
    pixels = image.convert("RGB").load()
    out = bytearray()
    for y in range(H):
        for x in range(W):
            r, g, b = pixels[x, y]
            out += struct.pack("<H", (r >> 3) << 11 | (g >> 2) << 5 | b >> 3)
    return bytes(out)


def picture(frames, name, last):
    folder = frames / name
    files = sorted(folder.glob("f*.png"))
    return rgb565(Image.open(files[-1] if last else files[0]))


def save(data, path):
    """RGB565 DATA as a PNG, to check the compositor drew a page."""
    rgb = bytearray()
    for (v,) in struct.iter_unpack("<H", bytes(data)):
        rgb += bytes(((v >> 11) << 3, ((v >> 5) & 63) << 2, (v & 31) << 3))
    Image.frombytes("RGB", (W, H), bytes(rgb)).save(path)


def frame_time(instructions, transfer, mhz):
    seconds = instructions * 4 / (mhz * 1e6) + transfer / (8 * 1024 * 1024)
    return round(seconds / REFRESH_S + 0.24, 2)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--elf", type=Path, required=True)
    parser.add_argument("--frames", type=Path, required=True)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    offsets = layout(args.build.resolve())
    m = Machine(args.elf)
    back, front, chrome, out, scene = (BUFFERS + i * 0x40000
                                       for i in range(5))

    pictures = {
        "push": (picture(args.frames, "artists-albums", False),
                 picture(args.frames, "artists-albums", True)),
        "split": (picture(args.frames, "home-music", False),
                  picture(args.frames, "home-music", True)),
        "mixed": (picture(args.frames, "music-artists", False),
                  picture(args.frames, "music-artists", True)),
        "np": (picture(args.frames, "menu-np", False),
               picture(args.frames, "menu-np", True)),
    }
    flags = {"push": {}, "split": {"back_split": 1, "front_split": 1},
             "mixed": {"back_split": 1},
             "np": {"back_split": 1, "full_screen": 1}}
    rows = []
    for name, (a, b) in pictures.items():
        m.uc.mem_write(back, a)
        m.uc.mem_write(front, b)
        m.uc.mem_write(chrome, b)
        m.uc.mem_write(scene, bytes(offsets["size"]))
        top = 0 if name == "np" else 20
        m.put(scene + offsets["pan_top"], "<i", top)
        for field in ("target_front", "slide"):
            m.put(scene + offsets[field], "<B", 1)
        for field, value in flags[name].items():
            m.put(scene + offsets[field], "<B", value)
        # The three poses, then Ultra's costliest edge from a sweep.
        costliest = max(range(8, W, 8), key=lambda p: m.call(
            "ultra_scene_compose", scene, out, back, front, chrome, p))
        for p in (280, 160, 40, costliest):
            fruit = m.call("scene.c:compose_pages", scene, out, back, front,
                           p, 0)
            ultra = m.call("ultra_scene_compose", scene, out, back, front,
                           chrome, p)
            rows.append({"case": name, "edge": p, "fruit": fruit,
                         "ultra": ultra, "transfer": FB})
            if name == "push" and p == 160 and args.out:
                save(m.uc.mem_read(out, FB), args.out.with_suffix(".png"))

    m.uc.mem_write(back, pictures["split"][0])
    fruit = m.call("ui_blit_dim", out, back, W * H, 128)
    ultra = m.call("ultra_scene_fade", out, back, 128)
    rows.append({"case": "fade", "edge": 128, "fruit": fruit, "ultra": ultra,
                 "transfer": FB})

    # The pane dissolve: both panes in one buffer behind handle 1, the area
    # they differ in, and the step at half way.
    was = picture(args.frames, "pane-row", False)
    now = picture(args.frames, "pane-row", True)
    pane = bytearray()
    for source in (was, now):
        for y in range(H):
            pane += source[(y * W + 160) * 2:(y * W + 320) * 2]
    table = BUFFERS + 0x1f0000
    data = BUFFERS + 0x100000
    m.uc.mem_write(data, bytes(pane))
    m.put(data - 4, "<I", 0)
    m.put(table - 4, "<I", data)
    # The handle table's end is core_ctx's first word.
    m.put(m.syms["core_ctx"], "<I", table)
    m.put(m.syms["lcd_framebuffer_default"], "<I", out)
    m.uc.mem_write(out, was)
    x0, x1, y0, y1 = W, 0, H, 0
    for y in range(H):
        for x in range(160, W):
            i = (y * W + x) * 2
            if was[i:i + 2] != now[i:i + 2]:
                x0, x1 = min(x0, x), max(x1, x)
                y0, y1 = min(y0, y), max(y1, y)
    x0 &= ~1
    x1 |= 1
    area = (x0, y0, x1 - x0 + 1, y1 - y0 + 1)
    fade = m.syms["ultra_pane.c:fade"]
    m.put(fade, "<i4i4i", 1, 160, 0, 160, H, *area)
    ultra = m.call("ultra_pane.c:blend", *area, 16)
    rows.append({"case": "pane", "edge": 16, "fruit": 0, "ultra": ultra,
                 "transfer": area[2] * area[3] * 2, "area": area})

    print(f"{'case':6} {'edge':>4} {'fruit':>9} {'ultra':>9} "
          f"{'54 MHz f':>9} {'216 MHz f':>9}")
    for row in rows:
        submit = 0 if row["case"] == "pane" else SUBMIT_INSTRUCTIONS
        for theme in ("fruit", "ultra"):
            total = row[theme] + submit
            row[f"{theme}_f54"] = frame_time(total, row["transfer"], 54)
            row[f"{theme}_f216"] = frame_time(total, row["transfer"], 216)
        print(f"{row['case']:6} {row['edge']:>4} {row['fruit']:>9} "
              f"{row['ultra']:>9} "
              f"{row['fruit_f54']:>4}/{row['ultra_f54']:<4} "
              f"{row['fruit_f216']:>4}/{row['ultra_f216']:<4}")
    if args.out:
        args.out.write_text(json.dumps(rows, indent=1) + "\n")


if __name__ == "__main__":
    main()
