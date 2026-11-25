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
#define UI_PERF_ART 1u
#define UI_PERF_CAPTION 2u
/* Covers on rows 20-170 (or 20-240 for a whole-viewport repaint), the
 * caption box on rows 210-230. */
#define ART_TOP 20
#define CAPTION_TOP 210
#define CAPTION_END 230
static int fb[240], panel[240], calls;
static bool spanning_allowed;
static unsigned render_notes, submit_notes, noted_parts, noted_art, noted_caption;
static struct { bool capturing; } pf_scene;
static void ui_scene_start(void *scene)
{
    assert(scene == &pf_scene && calls == 1);
    pf_scene.capturing = false;
}
static uint32_t clock_value;
#define USEC_TIMER (++clock_value)
static void ui_perf_note_rects(unsigned parts, unsigned art, unsigned caption)
{
    noted_parts = parts;
    noted_art = art;
    noted_caption = caption;
}
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
static int last_h;
static void lcd_update_rect(int x,int y,int w,int h) {
    assert(x==0 && w==320 && y>=0 && y+h<=240 && h>0);
    /* Never covers and caption in one transfer, unless the caption lies
     * inside rows that have to go anyway. */
    assert(spanning_allowed || y >= CAPTION_TOP || y + h <= CAPTION_TOP);
    memcpy(panel+y,fb+y,h*sizeof(int));
    last_h = h;
    ++calls;
}
int main(void) {
    for (int mask=1; mask<128; ++mask) {
        bool drew_art=mask&1, drew_caption=mask&2, joined=mask&4;
        bool full_flush_owed=mask&8, partial=mask&32;
        bool caption_flush_owed=mask&64;
        int dirty_top=240, dirty_end=0, pf_vp_y=ART_TOP, art_y=0, art_h=joined?220:150;
        int pf_text_h=20;
        int caption_rows_y = partial ? 192 : 190, caption_rows_h = partial ? 8 : 20;
        int caption_flush_top = CAPTION_TOP, caption_flush_end = CAPTION_END;
        if(joined && !drew_art) continue;
        memcpy(panel, fb, sizeof fb);
        spanning_allowed = joined || full_flush_owed;
        calls=0;
        bool first_frame = true;
        pf_scene.capturing = drew_art;
        unsigned token = mask & 1, *trace = &token;
        uint32_t render = 1;
        render_notes = submit_notes = 0;
        noted_parts = noted_art = noted_caption = 0;
        for (int y=0;y<240;y++) {
            if(full_flush_owed || ((mask&16) && y<10) ||
                (drew_art && y>=ART_TOP && y<ART_TOP+art_h) ||
                (drew_caption && y>=ART_TOP+caption_rows_y &&
                 y<ART_TOP+caption_rows_y+caption_rows_h) ||
                (caption_flush_owed && y>=CAPTION_TOP && y<CAPTION_END))
                fb[y]=mask;
        }
        if(mask&16) {dirty_top=0;dirty_end=10;}
""" + frame + """
        assert(calls<=1 && !first_frame);
        assert(!pf_scene.capturing);
        assert(render_notes == (unsigned)calls * token);
        assert(submit_notes == render_notes);
        if (render_notes)
        {
            if (noted_parts & UI_PERF_ART)
                assert(noted_art == (unsigned)last_h);
            else
                assert(noted_parts == UI_PERF_CAPTION &&
                       noted_caption == (unsigned)last_h);
        }
        /* The caption the frame held back goes on the next deadline, alone. */
        if (caption_flush_owed)
        {
            assert(calls == 1 && !spanning_allowed);
            spanning_allowed = false;
            lcd_update_rect(0, caption_flush_top, LCD_WIDTH,
                            caption_flush_end - caption_flush_top);
            assert(last_h <= CAPTION_END - CAPTION_TOP);
        }
        assert(!memcmp(panel,fb,sizeof fb));
    }
}
"""
c = out / 'submit.c'
c.write_text(program)
exe = out / 'submit'
subprocess.run(['cc', '-Wall', '-Werror', str(c), '-o', str(exe)], check=True)
subprocess.run([str(exe.resolve())], check=True)
print('PASS covers and caption go in separate transfers, the held caption follows alone, and each frame records at most one sample')
