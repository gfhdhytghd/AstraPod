"""Cover Flow's motion, driven by simulated wheel clicks without a device.

The 3D view's chase (flow_chase, flow_click, flow_update) is copied out of
carousel.c and run against click sequences -- a single click, slow clicks, a
fast burst, the stop after it, and a reversal -- checking the speed law, that
nothing passes its target, and that the turn leads the row. Flat's timeline
and the frame deadlines run through the same copies.

Usage: python3 tools/tests/coverflow_motion_test.py <scratch dir>
"""
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
#define BUTTON_SCROLL_FWD 2
static uint32_t clock_counter, pf_flat_last_input;
/* The Classic's panel: one frame a refresh at 66.93 Hz. */
uint32_t ui_frame_period_q8(void) { return (uint32_t)(256e6 / 66.93); }
uint32_t ui_frame_period(void) { return (ui_frame_period_q8() + 128) >> 8; }
uint32_t ui_clock_start(void) { return clock_counter; }
/* Without a precise wake: whole ticks, rounded down. */
int ui_clock_wait_until(uint32_t deadline)
{
    int32_t left = deadline - clock_counter;
    return left <= 0 ? TIMEOUT_NOBLOCK : left / (1000000 / HZ);
}
static int pf_flat_interval = 25;
static bool pf_random_spinning, flat, pf_flat_snap;
static uint32_t anim_clock;
static unsigned anim_sampled_frame;
static int anim_pose_step, anim_frac;
static int center_index, target, step, pf_state;
static int number_of_slides = 200;
static struct slide_data center_slide;
enum {pf_idle, pf_scrolling};
static struct {
    int album_covers_scroll_speed;
} global_settings = { 175 };
static bool flat_view(void) { return flat; }
static void wake_pf_thread(void) {}
static int fbound(int lo, int x, int hi) { return x < lo ? lo : x > hi ? hi : x; }
static void keyclick_click(bool force, int button) { (void)force; (void)button; }
static void flat_animate(int direction, PFreal frame) { (void)direction; (void)frame; }
static void flat_idle(void) {}
#define UI_THEME_MOTION_ONE 100
static void start_animation(void);
''' + region(src, 'static unsigned pf_motion_scale', '\n}') + '\n}\n' + region(
    src, '#define PF_SPEED_FRAMES', 'static struct ui_pace') + region(
    src, '#define PF_FLAT_FLIP ', '/* The deal:') + region(
    src, '#define PF_FLAT_SETTLE ', '/* The cover geometry') + region(
    src, 'static uint32_t motion_clock', "/* --- The 3D view's motion") + region(
    src, '#define FLOW_FLOOR', '/* Every card on its resting place') + region(
    geom, '#define PF_SIDE_TILT', 'static PFreal flow_cx') + '''
static void reset_slides(void) { if (flat) flat_idle(); else coverflow_idle(); }
static void set_current_slide(int slide)
{
    center_index = target = slide;
    slide_frame = slide << 16;
    step = 0;
    pf_state = pf_idle;
    reset_slides();
}
''' + region(src, 'static void start_animation', 'static bool random_was_recent') + region(
    src, 'static int scroll_speed', 'static void cleanup') + region(
    src, '#define PF_DEADLINE_AHEAD_US', '#define ALBUM_PANEL_WIDTH') + r'''
#define ONE_SLIDE 65536
static double frame_s;
static double dabs(double v) { return v < 0 ? -v : v; }
static double dmin(double a, double b) { return a < b ? a : b; }
static double dmax(double a, double b) { return a > b ? a : b; }

static void seed(uint32_t clock, int index)
{
    clock_counter = clock;
    pf_flat_interval = PF_FLAT_FLIP;
    pf_flat_last_input = clock - ui_frame_clock(PF_FLAT_FLIP);
    set_current_slide(index);
}

static void draw_at(uint32_t clock, unsigned frame)
{
    clock_counter = clock + ui_frame_clock(frame);
    update_scroll_animation();
}

/* One 3D frame, checked against the law. Each chase moves straight towards
 * the target by min(d, (FLOOR + gain * min(d, REACH)) * dt), so it can never
 * pass it, and an advance that matches the law to a unit is the law. */
static void check_step(int from, int to, int now, int gain)
{
    double d = dabs((double)to - from);
    double law = (FLOW_FLOOR / 1000.0 * ONE_SLIDE
                  + gain / 1000.0 * dmin(d, FLOW_REACH)) * frame_s;
    double want = dmin(d, law);
    double got = dabs((double)now - from);

    assert(dabs(got - want) <= 1.5);
    assert(to >= from ? (now >= from && now <= to) : (now <= from && now >= to));
}

static int frames_run;

static void frame(void)
{
    int p = slide_frame, q = pose_frame, to = target << 16;
    bool moving = pf_state == pf_scrolling;

    clock_counter += ui_frame_clock(1);
    if (moving)
        update_scroll_animation();
    frames_run++;
    if (!moving)
        return;
    check_step(p, to, slide_frame, FLOW_GAIN);
    check_step(q, to, pose_frame, FLOW_POSE_GAIN);
    assert(center_index == (slide_frame + 0x8000) >> 16);
}

static void frames(unsigned n)
{
    while (n--)
        frame();
}

/* The covers' resting state on `index`. */
static bool at_rest_on(int index)
{
    return pf_state == pf_idle && step == 0 && center_index == index
        && slide_frame == index << 16 && pose_frame == index << 16;
}

static void check_single_click(uint32_t epoch)
{
    seed(epoch, 3);
    show_next_slide();
    assert(target == 4 && pf_state == pf_scrolling);
    int t90 = -1, rest = -1, pose_rest = -1;
    for (int f = 1; f < 200 && rest < 0; f++) {
        int p0 = slide_frame - (3 << 16), q0 = pose_frame - (3 << 16);
        frame();
        int p = slide_frame - (3 << 16), q = pose_frame - (3 << 16);
        /* The turn leads the row at every frame of the step. */
        assert(q >= p && q >= q0 && p >= p0);
        if (t90 < 0 && p >= ONE_SLIDE * 9 / 10)
            t90 = f;
        if (pose_rest < 0 && pose_frame == 4 << 16)
            pose_rest = f;
        if (pf_state == pf_idle)
            rest = f;
    }
    assert(at_rest_on(4));
    double t90_s = t90 * frame_s, rest_s = rest * frame_s;
    printf("single click: 90%% at %.3f s, at rest at %.3f s, turned at %.3f s\n",
           t90_s, rest_s, pose_rest * frame_s);
    /* The stock player: 90 % at 0.35-0.38 s, at rest at 0.48-0.51 s. */
    assert(t90_s >= 0.33 && t90_s <= 0.40);
    assert(rest_s >= 0.46 && rest_s <= 0.53);
    assert(pose_rest < rest);
}

/* `count` clicks `interval_s` apart in `direction`, checking the turn leads
 * the row and the row only ever moves one way. Returns the largest lag. */
static double clicks(int count, double interval_s, int direction)
{
    double lag = 0, next = 0, t = 0;
    int last = slide_frame;

    for (int i = 0; i < count; ) {
        if (t >= next) {
            if (direction > 0)
                show_next_slide();
            else
                show_previous_slide();
            next += interval_s;
            i++;
        }
        frame();
        t += frame_s;
        int to = target << 16;
        assert(direction * (slide_frame - last) >= 0);
        assert(abs(to - pose_frame) <= abs(to - slide_frame));
        last = slide_frame;
        lag = dmax(lag, dabs((double)(to - slide_frame)) / ONE_SLIDE);
    }
    return lag;
}

static void check_slow_clicks(uint32_t epoch)
{
    seed(epoch, 10);
    /* Four slides a second: the row never comes to rest between clicks. */
    clicks(1, 0, 1);
    for (int i = 0; i < 7; i++) {
        clicks(1, 0, 1);
        for (int f = 0; f < (int)(0.25 / frame_s); f++) {
            frame();
            assert(pf_state == pf_scrolling);
        }
    }
    assert(target == 18);
    frames(100);
    assert(at_rest_on(18));
}

static void check_burst_and_stop(uint32_t epoch)
{
    seed(epoch, 10);
    /* Fifteen clicks a second for two seconds: every click counts, and the
     * row keeps up within the distance its speed stops growing at. */
    double lag = clicks(30, 1 / 15.0, 1);
    assert(target == 40);
    printf("burst at 15 slides/s: largest lag %.2f slides\n", lag);
    assert(lag > 1.5 && lag < FLOW_REACH / (double)ONE_SLIDE);
    /* The stop: the row slows into the target with no snap. */
    int prev = slide_frame, prev_adv = 1 << 30, f = 0;
    while (pf_state == pf_scrolling) {
        frame();
        int adv = slide_frame - prev;
        assert(adv >= 0 && adv <= prev_adv);
        prev = slide_frame;
        prev_adv = adv;
        f++;
    }
    printf("stop: at rest %.3f s after the last click\n", f * frame_s);
    assert(at_rest_on(40) && f * frame_s < 1.0);
}

static void check_reversal(uint32_t epoch)
{
    seed(epoch, 50);
    clicks(10, 0.1, 1);
    int turns = 0, last_dir = 1, prev = slide_frame;
    double t = 0, next = 0;
    for (int i = 0; i < 10 || pf_state == pf_scrolling; ) {
        if (i < 10 && t >= next) {
            show_previous_slide();
            next += 0.1;
            i++;
        }
        frame();
        t += frame_s;
        int dir = slide_frame > prev ? 1 : slide_frame < prev ? -1 : 0;
        if (dir && dir != last_dir) {
            turns++;
            last_dir = dir;
        }
        prev = slide_frame;
    }
    /* One smooth turn, back to where it started. */
    assert(turns == 1 && at_rest_on(50));
}

static void check_ends(uint32_t epoch)
{
    seed(epoch, 1);
    clicks(5, 0.05, -1);
    assert(target == 0);
    frames(100);
    assert(at_rest_on(0));
    show_previous_slide();
    assert(pf_state == pf_idle && target == 0);
    seed(epoch, number_of_slides - 2);
    clicks(5, 0.05, 1);
    assert(target == number_of_slides - 1);
    frames(100);
    assert(at_rest_on(number_of_slides - 1));
}

/* A late frame lands where the same refreshes drawn one by one would have. */
static void check_late_frames(uint32_t epoch)
{
    seed(epoch, 3);
    clicks(3, 0.03, 1);
    uint32_t from = clock_counter;
    int saved_p = slide_frame, saved_q = pose_frame, saved_t = target;
    unsigned saved_sample = anim_sampled_frame;
    for (unsigned f = 1; f <= 9; f++)
        draw_at(from, f);
    int p = slide_frame, q = pose_frame;
    slide_frame = saved_p;
    pose_frame = saved_q;
    target = saved_t;
    anim_sampled_frame = saved_sample;
    pf_state = pf_scrolling;
    step = 1;
    draw_at(from, 9);
    assert(slide_frame == p && pose_frame == q);
}

/* The geometry at the moment of a step: the turn is ahead of the row, so the
 * leaving cover is further round than it is across. */
static void check_pose(uint32_t epoch)
{
    recalc_offsets();
    seed(epoch, 3);
    show_next_slide();
    frames(4);
    int pos = center_slide.cx < 0 ? -center_slide.cx : center_slide.cx;
    int across = (int)((long long)pos * 65536 / offsetX);
    int round = (int)((long long)center_slide.angle * 65536 / itilt);
    assert(round > across && across > 0);
    printf("after 4 frames: leaving cover %.2f across, %.2f round\n",
           across / 65536.0, round / 65536.0);
}

static void check_timeline(uint32_t epoch)
{
    seed(epoch, 3);
    show_next_slide();
    for (unsigned f = 1; f <= 7; f++)
        draw_at(epoch, f);
    int on_time = slide_frame;
    int on_time_index = center_index;
    seed(epoch, 3);
    show_next_slide();
    draw_at(epoch, 7);
    assert(slide_frame == on_time && center_index == on_time_index);
    draw_at(epoch, 90);
    assert(step == 0 && pf_state == pf_idle);
    assert(center_index == 4 && slide_frame == (4 << 16));

    seed(epoch, 3);
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
    seed(epoch, 3);
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
    frame_s = ui_frame_period() / 1e6;
    recalc_offsets();
    /* Across the timer's wrap as well as clear of it. */
    uint32_t epochs[] = { 1000, UINT32_MAX - ui_frame_clock(10) };
    for (unsigned e = 0; e < 2; e++) {
        uint32_t epoch = epochs[e];
        flat = false;
        check_single_click(epoch);
        check_slow_clicks(epoch);
        check_burst_and_stop(epoch);
        check_reversal(epoch);
        check_ends(epoch);
        check_late_frames(epoch);
        check_pose(epoch);
        for (int layout = 0; layout < 2; layout++) {
            flat = !!layout;
            check_timeline(epoch);
        }
        check_flat_deal_timeline(epoch);
    }
    flat = false;

    uint32_t epoch = epochs[1];
    uint32_t deadline = epoch + ui_frame_clock(13);
    clock_counter = epoch + ui_frame_clock(12);
    assert(!frame_due(deadline) && frame_wait(deadline) >= 1);
    clock_counter = deadline - 5000;
    assert(!frame_due(deadline) && frame_wait(deadline) == 1);
    clock_counter = deadline;
    assert(frame_due(deadline) && frame_wait(deadline) == TIMEOUT_NOBLOCK);
    clock_counter = deadline + ui_frame_clock(30);
    assert(frame_due(deadline) && frame_wait(deadline) == TIMEOUT_NOBLOCK);
    clock_counter = deadline + 0x90000000u;
    assert(frame_due(deadline));
    puts("PASS single click, slow clicks, burst, stop, reversal, ends, "
         "late frames and the leading turn");
    puts("PASS flat timeline, frame deadlines and timer wrap");
    return 0;
}
'''
c = out / 'motion.c'
c.write_text(program)
binary = out / 'motion'
subprocess.run(['cc', '-O2', '-fwrapv', '-I', root / 'apps-ipod', c,
                root / 'apps-ipod/ui/animation.c', '-o', binary],
               check=True)
subprocess.run([binary], check=True)
