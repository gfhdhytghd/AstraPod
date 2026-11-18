"""Replay the carousel's actual frame submission with independent band stamps."""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[2]
out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)
source = (root / 'apps-ipod/screens/covers/carousel.c').read_text()
frame = source.split('uint32_t submitting = USEC_TIMER;', 1)[1].split('uint32_t submitted', 1)[0]
frame = frame[frame.index('if (drew_art)'):frame.index('skin_top = LCD_HEIGHT;')]
program = """
#include <assert.h>
#include <stdbool.h>
#include <string.h>
#define LCD_HEIGHT 240
#define LCD_WIDTH 320
#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))
static int fb[240], panel[240], calls;
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
        int skin_top=240, skin_end=0, pf_vp_y=20, art_y=0, art_h=joined?220:150;
        int pf_text_y=190,pf_text_h=20;
        if(joined && !drew_art) continue;
        calls=0;
        for (int y=0;y<240;y++) {
            if(full_flush_owed || ((mask&16) && y<10) ||
                (drew_art && y>=20 && y<20+art_h) ||
                (drew_caption && y>=210 && y<230)) fb[y]=mask;
        }
        if(mask&16) {skin_top=0;skin_end=10;}
""" + frame + """
        assert(calls<=1 && !memcmp(panel,fb,sizeof fb));
    }
}
"""
c = out / 'submit.c'
c.write_text(program)
exe = out / 'submit'
subprocess.run(['cc', '-Wall', '-Werror', str(c), '-o', str(exe)], check=True)
subprocess.run([str(exe.resolve())], check=True)
print('PASS every submitted snapshot contains all dirty band stamps')
