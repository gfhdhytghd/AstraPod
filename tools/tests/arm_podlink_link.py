#!/usr/bin/env python3
"""Check that the Rust PodLink object cannot replace firmware/libgcc symbols.

FU49 round 2: core/compiler_builtins define weak memset/memmove/memcmp/strlen
and EABI helpers. Linked as a plain archive ahead of libfirmware, those weak
definitions satisfied every later reference, so the whole firmware ran the
slow Rust byte loops from DRAM. The build now prelinks one object whose only
global definitions are podlink_*. Reads the linked ELF and map; no device.
"""
import argparse
import re
import subprocess
from pathlib import Path
from elftools.elf.elffile import ELFFile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--elf', type=Path, required=True)
p.add_argument('--nm', default='arm-elf-eabi-nm')
a = p.parse_args()
build = a.elf.parent
obj = build / 'podlink-rust-target/libpodlink_fw.o'
mapfile = a.elf.with_suffix('.map')

# 1. The Rust object exports only the C API (pfu_* and helpers stay imports).
if obj.exists():
    exported = [line.split()[-1] for line in subprocess.run(
        [a.nm, '-g', '--defined-only', str(obj)], check=True,
        capture_output=True, text=True).stdout.splitlines()]
    leaked = [n for n in exported if not n.startswith('podlink_')]
    assert exported and not leaked, ('Rust object exports non-PodLink symbols', leaked[:20])
    print(f'PASS Rust object globals: {len(exported)} podlink_* only')
else:
    print(f'SKIP {obj}: not a prelinked build')

# 2. Every address of the shared helpers lies in a firmware/libgcc input section.
sections = []
text = mapfile.read_text(errors='replace')
# The memory map starts at the first output section (column 0); the discarded
# input sections listed before it all sit at address 0. Locale independent.
for line in text[re.search(r'^\.\w', text, re.M).start():].splitlines():
    m = re.match(r'^ (\S+)?\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S+\.(?:a\(.*\)|o))$', line)
    if m:
        sections.append((int(m.group(2), 16), int(m.group(3), 16), m.group(4)))
symbols = {}
with a.elf.open('rb') as f:
    for s in ELFFile(f).get_section_by_name('.symtab').iter_symbols():
        if s['st_info']['bind'] in ('STB_GLOBAL', 'STB_WEAK') and s['st_shndx'] != 'SHN_UNDEF':
            symbols[s.name] = s['st_value'] & ~1


def owner(name):
    addr = symbols[name]
    hits = [f for start, size, f in sections if size and start <= addr < start + size]
    assert hits, (name, hex(addr), 'not in any map input section')
    return hits[0]


expect = {'memset': 'libfirmware.a', 'memmove': 'libfirmware.a', 'memcmp': 'libfirmware.a',
          'strlen': 'libfirmware.a', 'memcpy': 'libfirmware.a',
          '__aeabi_uldivmod': 'libgcc.a', '__aeabi_ldivmod': 'libgcc.a',
          '__aeabi_uidiv': 'libarm_support.a', '__aeabi_idiv': 'libarm_support.a',
          '__aeabi_dmul': 'libgcc.a'}
for name, library in expect.items():
    if name not in symbols:
        print(f'SKIP {name}: not linked')
        continue
    source = owner(name)
    assert library in source and 'podlink' not in source, (name, source)
    print(f'PASS {name}: {Path(source.split("(")[0]).name}({source.split("(")[-1]}')
print('PASS arm podlink link isolation')
