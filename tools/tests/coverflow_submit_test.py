"""Replay the carousel's actual frame submission with independent band stamps."""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[2]
out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)
source = (root / 'apps-ipod/screens/covers/carousel.c').read_text()
frame = source.split('/* Copy offscreen buffer to LCD', 1)[1]
frame = frame[frame.index('if (drew_art)'):frame.index('dirty_top = LCD_HEIGHT;')]
program = """
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#define LCD_HEIGHT 240
#define LCD_WIDTH 320
#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))
static int fb[240], panel[240], calls;
static unsigned render_notes, submit_notes;
static uint32_t clock_value;
#define USEC_TIMER (++clock_value)
static void ui_perf_note_render(uint32_t elapsed)
{
    assert(elapsed == 1);
    ++render_notes;
}
static void ui_perf_note_submit(uint32_t elapsed)
{
    assert(elapsed == 1);
    ++submit_notes;
}
static void lcd_update_rect(int x,int y,int w,int h) {
    assert(x==0 && w==320 && y>=0 && y+h<=240);
    memcpy(panel+y,fb+y,h*sizeof(int));
    ++calls;
    assert(!memcmp(panel,fb,sizeof fb));
}
int main(void) {
    for (int mask=1; mask<32; ++mask) {
        bool drew_art=mask&1, drew_caption=mask&2, joined=mask&4;
        bool full_flush_owed=mask&8;
        int dirty_top=240, dirty_end=0, pf_vp_y=20, art_y=0, art_h=joined?220:150;
        int pf_text_y=190,pf_text_h=20;
        if(joined && !drew_art) continue;
        calls=0;
        unsigned token = mask & 1, *trace = &token;
        render_notes = submit_notes = 0;
        uint32_t frame_start = clock_value;
        for (int y=0;y<240;y++) {
            if(full_flush_owed || ((mask&16) && y<10) ||
                (drew_art && y>=20 && y<20+art_h) ||
                (drew_caption && y>=210 && y<230)) fb[y]=mask;
        }
        if(mask&16) {dirty_top=0;dirty_end=10;}
""" + frame + """
        assert(calls<=1 && !memcmp(panel,fb,sizeof fb));
        assert(render_notes == (unsigned)calls * token);
        assert(submit_notes == render_notes);
    }
}
"""
c = out / 'submit.c'
c.write_text(program)
exe = out / 'submit'
subprocess.run(['cc', '-Wall', '-Werror', str(c), '-o', str(exe)], check=True)
subprocess.run([str(exe.resolve())], check=True)
print('PASS each snapshot contains every dirty band and records at most one sample')
