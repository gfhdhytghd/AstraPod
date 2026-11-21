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

fixtures = geom[:geom.index('static struct dim slide_dim;')].replace(
    'struct slide_data { int angle;', 'struct slide_data { int slide_index, angle;')
program = fixtures + '''
#include <stdbool.h>
#include <assert.h>
#include "ui/animation.h"
#define HZ 100
#define USEC_TIMER clock_counter
#define TIMEOUT_NOBLOCK 0
#define EV_WAKEUP 1
#define BUTTON_SCROLL_FWD 2
#define CAROUSEL_NOMINAL_FPS 30
static uint32_t clock_counter, current_tick, pf_flat_last_input, flow_pose_clock;
static unsigned flow_sampled_frame;
static int pf_flat_interval=25, flow_mix;
static int flow_mix_frames;
static bool flow_fast, pf_random_spinning, flat, pf_native_motion, pf_flat_snap;
static uint32_t anim_clock;
static unsigned anim_sampled_frame;
static int anim_pose_step, anim_frac, anim_avg4_ms = 80, anim_owed_ms;
static bool anim_started;
static int center_index, target, slide_frame, pf_state, flow_landed_direction;
static int number_of_slides = 10, thread_q;
enum {pf_idle, pf_scrolling};
static struct {
    int album_covers_scroll_speed;
    int album_covers_transition_speed;
} global_settings = { 200, 325 };
static bool flat_view(void) { return flat; }
static void wake_pf_thread(void) {}
static void keyclick_click(bool force, int button) {}
static void flat_animate(int direction, PFreal frame) {}
#define UI_THEME_MOTION_ONE 100
static void set_current_slide(int slide) { center_index = target = slide; slide_frame = slide << 16; }
''' + region(src, 'static unsigned pf_motion_scale', '\n}') + '\n}\n' + region(src, '#define PF_FLAT_FLIP ', '/* The deal:') + region(
    src, '#define PF_FLAT_SETTLE ', '/* The cover geometry') + region(
    src, 'static unsigned motion_rate', 'static void coverflow_idle') + region(
    geom, 'static void recalc_offsets', 'static void coverflow_animate') + region(
    src, 'static void coverflow_animate', 'static void flat_animate') + region(
    src, 'static int flow_brightness', 'static void coverflow_render(') + '''
static void reset_slides(void) { coverflow_idle(); }
''' + region(src, 'static void start_animation', 'static bool random_was_recent') + region(
    src, 'static int scroll_speed', 'static void cleanup') + region(
    src, 'static int native_frame_wait', '#define ALBUM_PANEL_WIDTH') + '''
static void seed(uint32_t clock)
{
    center_slide.slide_index = center_index = target = 3;
    slide_frame = 3 << 16;
    step = 0;
    pf_state = pf_idle;
    flow_mix = 0;
    clock_counter = clock;
    pf_flat_interval = PF_FLAT_FLIP;
    pf_flat_last_input = clock - ui_frame_clock(PF_FLAT_FLIP);
    reset_slides();
}

static void draw_at(uint32_t clock, unsigned frame)
{
    clock_counter = clock + ui_frame_clock(frame);
    update_scroll_animation();
}

static void check_timeline(uint32_t epoch)
{
    seed(epoch);
    show_next_slide();
    for (unsigned f = 1; f <= 7; f++)
        draw_at(epoch, f);
    int on_time = slide_frame;
    int on_time_index = center_index;
    seed(epoch);
    show_next_slide();
    draw_at(epoch, 7);
    assert(slide_frame == on_time && center_index == on_time_index);
    draw_at(epoch, 90);
    assert(step == 0 && pf_state == pf_idle);
    assert(center_index == 4 && slide_frame == (4 << 16));

    seed(epoch);
    show_next_slide();
    draw_at(epoch, 3);
    int forward = slide_frame;
    int angle = center_slide.angle;
    assert(forward > (3 << 16) && forward < (4 << 16));
    uint32_t reverse_clock = clock_counter;
    show_previous_slide();
    draw_at(reverse_clock, 0);
    assert(slide_frame == forward && center_slide.angle == angle && center_index == 3);
    draw_at(reverse_clock, 1);
    assert(slide_frame < forward && center_index == 3);
    draw_at(reverse_clock, 30);
    assert(center_index == 3 && slide_frame == (3 << 16) && pf_state == pf_idle);
}

static void begin_flat_deal(uint32_t epoch)
{
    seed(epoch);
    pf_flat_last_input = epoch;
    pf_flat_interval = 2;
    target = 6;
    start_animation();
    assert(pf_flat_snap);
}

static void check_flat_deal_timeline(uint32_t epoch)
{
    begin_flat_deal(epoch);
    for (unsigned f = 1; f <= 5; f++)
        draw_at(epoch, f);
    int position = slide_frame;
    assert(!pf_flat_snap && center_index == 5);
    begin_flat_deal(epoch);
    draw_at(epoch, 5);
    assert(!pf_flat_snap && slide_frame == position);
}

int main(void)
{
    for(int alpha=0;alpha<=256;alpha++) {
        assert(flow_brightness(alpha,0)==alpha);
        assert(flow_brightness(alpha,PFREAL_ONE)==256);
        assert(flow_brightness(alpha,PFREAL_HALF)==(alpha+256)/2);
    }
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
            assert(abs(center_slide.angle-s*(itilt+FLOW_INNER_TILT))<=1);
            assert(abs(incoming->angle+s*(itilt+FLOW_INNER_TILT))<=1);
            assert(left_slides[2].angle==itilt && right_slides[2].angle==-itilt);
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
    pf_native_motion = true;
    flat = false;
    uint32_t epoch = UINT32_MAX - ui_frame_clock(10);
    pf_flat_last_input = flow_pose_clock = epoch;
    pf_flat_interval = PF_FLAT_FLIP;
    for (unsigned frame = 1; frame <= 12; frame++)
    {
        clock_counter = epoch + ui_frame_clock(frame);
        flow_note_wheel(true);
        flow_update_mix();
    }
    assert(flow_fast && flow_mix == PFREAL_ONE);
    uint32_t release = pf_flat_last_input + ui_frame_clock(PF_FLOW_SLOW);
    clock_counter = release;
    flow_update_mix();
    assert(flow_fast && flow_mix == PFREAL_ONE);
    for (unsigned f = 1; f <= PF_FLOW_RAMP; f++)
    {
        clock_counter = release + ui_frame_clock(f);
        flow_update_mix();
        assert(!flow_fast && flow_mix == (PF_FLOW_RAMP - f) * PFREAL_ONE / PF_FLOW_RAMP);
    }
    for (int layout = 0; layout < 2; layout++)
    {
        flat = !!layout;
        check_timeline(epoch);
    }
    check_flat_deal_timeline(epoch);
    clock_counter = epoch + ui_frame_clock(12);
    assert(native_frame_wait(epoch, 12) > TIMEOUT_NOBLOCK);
    assert(native_frame_wait(epoch, 11) == TIMEOUT_NOBLOCK);
    clock_counter = epoch + ui_frame_clock(30);
    assert(native_frame_wait(epoch, 12) == TIMEOUT_NOBLOCK);
    puts("PASS slow, fast, reverse, release, blocked boundary and flat layout");
    puts("PASS 60 Hz deadlines, late samples, timer wrap, and in-gap reversal");
}
'''
c = out / 'pose.c'
c.write_text(program)
binary = out / 'pose'
subprocess.run(['cc', '-O2', '-fwrapv', '-I', root / 'apps-ipod', c,
                root / 'apps-ipod/ui/animation.c', '-o', binary], check=True)
subprocess.run([binary], check=True)
