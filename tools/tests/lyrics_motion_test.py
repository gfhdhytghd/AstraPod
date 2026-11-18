"""Compile production pure functions with sanitizers and reject broken policies."""
from pathlib import Path
import os, subprocess, sys
root=Path(__file__).resolve().parents[2]
out=Path(sys.argv[1]).resolve(); out.mkdir(parents=True,exist_ok=True)
src=root/'apps-ipod/viewers/lyric_viewer'
apps=root/'apps-ipod'
flags=['cc','-std=c99','-Wall','-Wextra','-Werror','-g','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(src),'-I'+str(apps)]
def run(source, name, expect=True):
    exe=out/name
    subprocess.run(flags+[str(source),str(apps/'ui/animation.c'),
                          str(root/'tools/tests/lyrics_motion_test.c'),'-o',str(exe)],check=True)
    result=subprocess.run([str(exe)],capture_output=True,text=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})
    (out/(name+'.log')).write_text(result.stdout+result.stderr)
    assert (result.returncode==0)==expect,(name,result.stdout,result.stderr)
run(src/'lyrics_motion.c','motion')
text=(src/'lyrics_motion.c').read_text()
mutations={
 'linear-ease':('return 256 - q * q * q / 65536;','return p + q * 0;'),
 'jump-retarget':('m->from = lyrics_motion_at(m, now);','m->from = m->to;'),
 'slow-frame-clock':('ui_tween_sample(&tween, ui_clock_frame(elapsed))',
                     'ui_tween_sample(&tween, ui_clock_frame(elapsed) / 2)'),
 'no-enlargement':('return d >= 256 ? 0 : 256 - d;','return d >= 256 ? 0 : 0;'),
 'no-idle-return':('>= (uint32_t)(4 * hz)) m->manual = false;','>= (uint32_t)(40 * hz)) m->manual = false;'),
 'untimed-auto':('if (!timed) return false;','if (!timed) m->manual = false;'),
 'word-jump':('return (int)((int64_t)width * (elapsed - start) / (end - start));','return width;'),
 'no-acceleration':('return 1 + m->burst / 4;','return 1;'),
 'no-wakeup':('        ticks = ((next - elapsed) * hz + 999) / 1000;','        ticks = hz / 2;'),
}
for name,(old,new) in mutations.items():
    assert old in text,name
    p=out/(name+'.c');p.write_text(text.replace(old,new,1));run(p,name,False)
print(f'PASS production motion/policy; {len(mutations)} broken implementations rejected')
