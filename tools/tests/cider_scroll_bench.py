"""Build and replay 1,200 cached albums with production cs_get/cache_put.
--require-zero is the acceptance gate. A failed case is kept as evidence.
No display, USB or device I/O. Disk costs are injected assumptions.
"""
from pathlib import Path
import argparse,json,subprocess
p=argparse.ArgumentParser();p.add_argument('out',type=Path);p.add_argument('--require-zero',action='store_true');p.add_argument('--model',default='1.8inch-HDD-assumed');p.add_argument('--speed',type=int,default=61);a=p.parse_args()
r=Path(__file__).resolve().parents[2];out=a.out.resolve();out.mkdir(parents=True,exist_ok=True)
subprocess.run(['cc','-std=gnu11','-O2','-Wall','-Wextra','-Werror','-I'+str(r/'apps-ipod'),'-I'+str(r/'firmware/export'),*[str(r/v) for v in ['tools/tests/cider_scroll_bench.c','apps-ipod/screens/playback/cider_model.c','apps-ipod/screens/playback/cider_store.c','firmware/usbstack/podlink_wire.c']],'-o',str(out/'bench')],check=True)
result=subprocess.run([str(out/'bench'),str(out/'scratch')],check=True,text=True,capture_output=True)
(out/'results.jsonl').write_text(result.stdout)
rows=[json.loads(line) for line in result.stdout.splitlines()]
selected=[v for v in rows if v.get('model')==a.model and v.get('speed')==a.speed]
for row in selected:print(json.dumps(row))
if a.require_zero:
    assert len(selected)==3
    assert all(v['placeholder_frames']==v['missing_frames']==0 for v in selected), 'continuous-scroll zero-placeholder/zero-missing acceptance FAILED'
