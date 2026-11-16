#!/usr/bin/env python3
"""Measure the actual ARM ELF, Cider modules and Rust PodLink RAM domains."""
import argparse,json
from pathlib import Path
from elftools.elf.elffile import ELFFile
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--build',type=Path,required=True)
p.add_argument('--baseline',type=Path,required=True)
a=p.parse_args();b=a.build

def measure(path):
    writable=readonly=text=0
    with path.open('rb') as f:
        e=ELFFile(f)
        for s in e.iter_sections():
            flags=s['sh_flags']
            if not flags&2:continue
            if flags&4:text+=s['sh_size']
            elif flags&1:writable+=s['sh_size']
            else:readonly+=s['sh_size']
    return dict(writable=writable,readonly=readonly,text=text,total=writable+readonly+text)
names=['screens/playback/'+n for n in ['cider','cider_model','cider_store','cider_library','cider_browser','cider_controls','cider_wps']]+['screens/covers/cider_covers']
rows=[dict(module=n,**measure(b/'apps-ipod'/(n+'.o'))) for n in names]
# Match the existing measure_ram.py convention: allocated non-executable bytes.
footprint=sum(r['writable']+r['readonly'] for r in rows)
base=measure(a.baseline);rust=measure(b/'rockbox.elf')
with (b/'rockbox.elf').open('rb') as f:
    syms=ELFFile(f).get_section_by_name('.symtab')
    rust_static=sum(s['st_size'] for s in syms.iter_symbols() if 'podlink_rust' in s.name and any(n in s.name for n in ['CTX','DELIVERY','CLOCK']))
result=dict(cider_modules=rows,cider_static=footprint,cider_cap=1152*1024,cider_headroom=1152*1024-footprint,baseline=base,rust=rust,delta={k:rust[k]-base[k] for k in base},rust_context_delivery_clock=rust_static,platform=measure(b/'firmware/usbstack/podlink_rust_platform.o'))
assert result['cider_headroom']>=0
# Preserve the observed headroom even when charging all additional image bytes
# to the Cider domain (which itself has no source changes).
result['headroom_after_total_growth']=result['cider_headroom']-max(0,result['delta']['total'])
assert result['headroom_after_total_growth']>=0
print(json.dumps(result,indent=2))
