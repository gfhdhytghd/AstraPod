"""Exercise the actual wheel cadence and animation equations without a device."""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[2]
out = Path(sys.argv[1]).resolve()
out.mkdir(parents=True, exist_ok=True)
src = (root / 'apps-ipod/screens/covers/carousel.c').read_text()
geom = (root / 'tools/pfgeom/pfgeom.c').read_text()

def region(text, start, end):
    a = text.index(start)
    return text[a:text.index(end, a)]

program = geom[:geom.index('static struct dim slide_dim;')] + '''
#include <stdbool.h>
#include <assert.h>
#define HZ 100
#define PF_FLAT_FLIP_MS 250
#define TIME_AFTER(a,b) ((long)((b)-(a)) < 0)
static long current_tick, pf_flat_last_input, flow_pose_tick;
static int pf_flat_interval=25, flow_mix;
static bool flow_fast, pf_random_spinning, flat;
static bool flat_view(void) { return flat; }
static int flat_ticks(int ms) { return ms * HZ / 1000; }
''' + region(src, 'static void flat_note_scroll', 'static void coverflow_idle') + region(
    geom, 'static void recalc_offsets', 'static void coverflow_animate') + region(
    src, 'static void coverflow_animate', 'static void flat_animate') + '''
int main(void)
{
    recalc_offsets();
    coverflow_idle();
    for (int i=0; i<6; i++) {
        current_tick+=20; flow_note_wheel(true); flow_update_mix();
        assert(!flow_fast && !flow_mix);
    }
    for (int i=0; i<10; i++) {
        current_tick+=3; flow_note_wheel(true); flow_update_mix();
    }
    assert(flow_fast && flow_mix==PFREAL_ONE);
    for (int s=-1; s<=1; s+=2) {
        for (int tick=0; tick<65536; tick+=2048) {
            coverflow_animate(s,tick,65536-tick,tick>>6);
            struct slide_data *incoming=s>0 ? right_slides : left_slides;
            assert(abs(center_slide.angle-s*itilt)<=1);
            assert(abs(incoming->angle+s*itilt)<=1);
            assert(s*center_slide.cx<=0 && s*incoming->cx>=0);
        }
    }
    current_tick+=9; flow_note_wheel(true); flow_update_mix();
    assert(flow_fast);
    for (int i=0; i<30; i++) {
        int before=flow_mix;
        current_tick++; flow_update_mix();
        assert(flow_mix<=before && before-flow_mix<=PFREAL_ONE/12+1);
    }
    assert(!flow_fast && !flow_mix);
    coverflow_idle();
    flow_tilt(&center_slide,1,0,0,flow_inner_slot());
    assert(!center_slide.angle && !center_slide.cx);
    for(int i=0; i<10; i++) {
        current_tick+=2; flow_note_wheel(false); flow_update_mix();
    }
    assert(!flow_fast && !flow_mix);
    flat=true;
    for(int i=0; i<10; i++) {
        current_tick+=2; flow_note_wheel(true); flow_update_mix();
    }
    assert(!flow_fast && !flow_mix);
    puts("PASS slow, fast, reverse, release, blocked boundary and flat layout");
}
'''
c = out / 'pose.c'
c.write_text(program)
binary = out / 'pose'
subprocess.run(['cc', '-O2', '-fwrapv', c, '-o', binary], check=True)
subprocess.run([binary], check=True)
