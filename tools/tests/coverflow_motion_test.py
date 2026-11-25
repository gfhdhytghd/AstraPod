"""Cover Flow's motion, driven by simulated wheel clicks without a device.

The 3D view's motion (flow_chase, flow_click, flow_turn_step, flow_update and
the placement) is copied out of carousel.c and run against click sequences
beside a floating-point model of the stock player's motion
(.specifications/coverflow-stock-motion.md, section 9.9): every frame, the
row's position and every placed cover's turn and x must match it. On top of
that: a single click's timing, the shapes at 3, 10 and 20 clicks a second,
the stop after a fast spin, a reversal and the ends. pfgeom then checks the
draw order and that neighbouring covers do not cross where the motion is slow.
Flat's timeline and the frame deadlines run through the same copies.

Usage: python3 tools/tests/coverflow_motion_test.py <scratch dir>
"""
from pathlib import Path
import re
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
    'static int slide_frame, target, center_index;', 'static int slide_frame;')
program = fixtures + '''
#include <stdbool.h>
#include <assert.h>
#include <math.h>
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
    int album_covers_center_margin, album_covers_slide_tuck;
} global_settings = { 175, 20, 32 };
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
    src, '#define PF_SLOT_X', '/* Nominal cover height.') + '''
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

/* --- The stock player's motion, in floating point (section 9.9) ---------
 * Items for position, quarter turns for the turn (+1 as a left-hand cover
 * turns), seconds for time. */
#define REF_K      5.7
#define REF_V0     0.2
#define REF_VCAP   25.0
#define REF_KY     8.0
#define REF_SLOT   104.6
#define REF_PITCH  28.2
#define REF_RING   64
static struct { double p; int index[REF_RING]; double turn[REF_RING]; } ref;

static double ref_rest(int i, double lead)
{
    double r = i - lead;
    return r < 0 ? dmin(-r, 1) : -dmin(r, 1);
}

static double ref_lead(void)
{
    return ref.p + fmax(-1, fmin(1, target - ref.p));
}

static double *ref_turn(int i)
{
    int k = i & (REF_RING - 1);
    if (ref.index[k] != i) {
        ref.index[k] = i;
        ref.turn[k] = ref_rest(i, ref_lead());
    }
    return &ref.turn[k];
}

/* At rest on `index`, the window of placed covers at its resting turns. */
static void ref_reset(int index)
{
    ref.p = index;
    for (int k = 0; k < REF_RING; k++)
        ref.index[k] = INT_MIN;
    for (int i = index - ALBUM_COVERS_NUM_SLIDES;
         i <= index + ALBUM_COVERS_NUM_SLIDES; i++)
        ref_turn(i);
}

static void ref_step(double dt)
{
    double d = target - ref.p;
    double v = dmin(REF_K * dabs(d) + REF_V0, REF_VCAP);
    double adv = dmin(dabs(d), v * dt);
    int middle;

    ref.p += d < 0 ? -adv : adv;
    middle = (int)floor(ref.p + 0.5);
    /* A cover leaving the window comes back at rest. */
    for (int k = 0; k < REF_RING; k++)
        if (abs(ref.index[k] - middle) > ALBUM_COVERS_NUM_SLIDES)
            ref.index[k] = INT_MIN;
    double lead = ref_lead();
    for (int i = middle - ALBUM_COVERS_NUM_SLIDES;
         i <= middle + ALBUM_COVERS_NUM_SLIDES; i++) {
        double *t = ref_turn(i);
        *t += (ref_rest(i, lead) - *t) * (1 - exp(-REF_KY * dt));
    }
}

static double ref_x(int i)
{
    double u = i - ref.p, a = dabs(u);
    double x = a < 1 ? REF_SLOT * a : REF_SLOT + REF_PITCH * (a - 1);
    return u < 0 ? -x : x;
}

/* The largest differences seen between the firmware's motion and the
 * model's: position in items, turn in quarter turns, x in world pixels. */
static double worst_p, worst_turn, worst_x;

static void check_slide(struct slide_data *si)
{
    int i = si->slide_index;
    double turn = *ref_turn(i);
    double angle = turn * PF_SIDE_ANGLE;

    worst_turn = dmax(worst_turn, dabs(flow_turn_of(i, flow_lead())->turn
                                       / 65536.0 - turn));
    worst_x = dmax(worst_x, dabs(si->cx / (double)PFREAL_ONE - ref_x(i)));
    assert(dabs(si->angle - angle) <= 2);
}

static void check_model(void)
{
    worst_p = dmax(worst_p, dabs(slide_frame / 65536.0 - ref.p));
    check_slide(&center_slide);
    for (int i = 0; i < ALBUM_COVERS_NUM_SLIDES; i++) {
        check_slide(&left_slides[i]);
        check_slide(&right_slides[i]);
    }
}

static void seed(uint32_t clock, int index)
{
    clock_counter = clock;
    pf_flat_interval = PF_FLAT_FLIP;
    pf_flat_last_input = clock - ui_frame_clock(PF_FLAT_FLIP);
    set_current_slide(index);
    ref_reset(index);
}

static void draw_at(uint32_t clock, unsigned frame)
{
    clock_counter = clock + ui_frame_clock(frame);
    update_scroll_animation();
}

/* The turn of cover `i` in degrees, positive as a left-hand cover turns. A
 * cover outside the placed window has no entry, and is at rest. */
static double turn_deg(int i)
{
    if (abs(i - center_index) > ALBUM_COVERS_NUM_SLIDES)
        return flow_rest_turn(i, flow_lead()) * 90.0 / 65536;
    return flow_turn_of(i, flow_lead())->turn * 90.0 / 65536;
}

static int frames_run;

/* One refresh, checked against the model: the row moves straight towards
 * the target, and never past it. */
static void frame(void)
{
    int p = slide_frame, to = target << 16;
    bool moving = pf_state == pf_scrolling;

    clock_counter += ui_frame_clock(1);
    frames_run++;
    if (!moving)
        return;
    update_scroll_animation();
    ref_step(frame_s);
    assert(to >= p ? (slide_frame >= p && slide_frame <= to)
                   : (slide_frame <= p && slide_frame >= to));
    assert(center_index == (slide_frame + 0x8000) >> 16);
    if (pf_state == pf_scrolling)
        check_model();
}

static void frames(unsigned n)
{
    while (n--)
        frame();
}

static void click(int direction)
{
    if (direction > 0)
        show_next_slide();
    else
        show_previous_slide();
}

/* The covers' resting state on `index`: every turn at rest. */
static bool at_rest_on(int index)
{
    if (pf_state != pf_idle || step != 0 || center_index != index
        || slide_frame != index << 16)
        return false;
    for (int i = index - ALBUM_COVERS_NUM_SLIDES;
         i <= index + ALBUM_COVERS_NUM_SLIDES; i++)
        if (flow_turn_of(i, index << 16)->turn
            != flow_rest_turn(i, index << 16))
            return false;
    return true;
}

static void check_single_click(uint32_t epoch)
{
    seed(epoch, 3);
    click(1);
    assert(target == 4 && pf_state == pf_scrolling);
    int t50 = -1, t90 = -1, turn90 = -1, rest = -1;
    for (int f = 1; f < 200 && rest < 0; f++) {
        int p0 = slide_frame - (3 << 16);
        frame();
        int p = slide_frame - (3 << 16);
        assert(p >= p0);
        if (t50 < 0 && p >= ONE_SLIDE / 2)
            t50 = f;
        if (t90 < 0 && p >= ONE_SLIDE * 9 / 10)
            t90 = f;
        /* The arriving cover, from a quarter turn to face-on. */
        if (turn90 < 0 && dabs(turn_deg(4)) <= 9)
            turn90 = f;
        if (pf_state == pf_idle)
            rest = f;
    }
    assert(at_rest_on(4));
    printf("single click: row 50%% at %.3f s, 90%% at %.3f s; arriving cover "
           "90%% turned at %.3f s; at rest at %.3f s\n", t50 * frame_s,
           t90 * frame_s, turn90 * frame_s, rest * frame_s);
    /* The model: the row 50 % at 116 ms and 90 % at 357 ms, the turn 90 % at
     * 288 ms, every turn within 0.35 degrees of rest at 690 ms. The first
     * frame past each is the one counted. */
    assert(t50 * frame_s >= 0.10 && t50 * frame_s <= 0.14);
    assert(t90 * frame_s >= 0.34 && t90 * frame_s <= 0.38);
    assert(turn90 * frame_s >= 0.27 && turn90 * frame_s <= 0.31);
    assert(rest * frame_s >= 0.66 && rest * frame_s <= 0.73);
}

/* `count` clicks `interval_s` apart in `direction`, checking the row only
 * ever moves one way and never trails by more than FLOW_LEAD_MAX. Returns
 * the largest lag, in items. */
static double clicks(int count, double interval_s, int direction)
{
    double lag = 0, next = 0, t = 0;
    int last = slide_frame;

    for (int i = 0; i < count; ) {
        if (t >= next) {
            click(direction);
            next += interval_s;
            i++;
        }
        frame();
        t += frame_s;
        int to = target << 16;
        assert(direction * (slide_frame - last) >= 0);
        last = slide_frame;
        lag = dmax(lag, dabs((double)(to - slide_frame)) / ONE_SLIDE);
        assert(lag <= FLOW_LEAD_MAX);
    }
    return lag;
}

/* The turns, left to right, over the covers within four of the middle. */
static void profile(const char *name, int count)
{
    printf("%-11s lag %.2f:", name, target - slide_frame / 65536.0);
    for (int i = center_index - count; i <= center_index + count; i++)
        printf(" %+.2f:%+3.0f", i - slide_frame / 65536.0, turn_deg(i));
    printf("\n");
}

/* Turns fall from the left to the right, so the row never folds back. */
static void check_monotonic(void)
{
    for (int i = center_index - ALBUM_COVERS_NUM_SLIDES;
         i < center_index + ALBUM_COVERS_NUM_SLIDES; i++)
        assert(turn_deg(i) >= turn_deg(i + 1) - 0.5);
}

/* Clicks at `rate` a second for 1.2 s: the steady shape. Returns how many
 * covers on screen are turned between 15 and 75 degrees. */
static int steady(double rate, const char *name)
{
    int partly = 0;

    clicks((int)(1.2 * rate), 1 / rate, 1);
    profile(name, 4);
    check_monotonic();
    for (int i = center_index - 3; i <= center_index + 3; i++)
        if (dabs(turn_deg(i)) > 15 && dabs(turn_deg(i)) < 75)
            partly++;
    return partly;
}

static void check_shapes(uint32_t epoch)
{
    /* Slow: the row keeps within an item, one cover turning at a time on
     * each side of the middle. */
    seed(epoch, 100);
    assert(steady(3, "3 clicks/s") <= 2);
    frames(100);
    assert(at_rest_on(target));

    /* The fan: an item or two behind, the covers either side of the middle
     * part way turned. */
    seed(epoch, 100);
    assert(steady(10, "10 clicks/s") >= 2);
    double lag = target - slide_frame / 65536.0;
    assert(lag > 1 && lag < 3);
    frames(100);

    /* Fast: the arriving side stays edge-on. */
    seed(epoch, 100);
    steady(20, "20 clicks/s");
    for (int i = center_index + 2; i <= center_index + 6; i++)
        assert(turn_deg(i) <= -85);
    frames(100);
    assert(at_rest_on(target));
}

/* The wheel faster than the row can follow: clicks beyond FLOW_LEAD_MAX
 * are lost, the row runs at FLOW_CAP, and after the stop the cover the wheel
 * stopped on turns face-on before it reaches the middle. */
static void check_fast_stop(uint32_t epoch)
{
    seed(epoch, 100);
    double lag = clicks(48, 1 / 48.0, 1);
    printf("48 clicks/s for 1 s: target %d, largest lag %.2f items\n",
           target, lag);
    assert(lag > FLOW_LEAD_MAX - 1 && target < 148);
    int stop = target;
    double turn_at = 99, slot_turn = 0;
    int f = 0;
    double prev_turn = turn_deg(stop);
    while (pf_state == pf_scrolling) {
        frame();
        f++;
        double u = stop - slide_frame / 65536.0;
        /* It only ever turns towards face-on as it comes in. */
        assert(dabs(turn_deg(stop)) <= dabs(prev_turn) + 0.01);
        prev_turn = turn_deg(stop);
        if (turn_at == 99 && u <= 0.61) {
            turn_at = turn_deg(stop);
            /* The slot's own turn there, the row's at rest about p. */
            slot_turn = 90 * 0.61;
        }
    }
    printf("stop: %.2f items from the middle the arriving cover is turned "
           "%.0f degrees (its slot's %.0f); at rest %.3f s after the last "
           "click\n", 0.61, turn_at, slot_turn, f * frame_s);
    assert(dabs(turn_at) < slot_turn - 20);
    assert(at_rest_on(stop));
}

static void check_reversal(uint32_t epoch)
{
    seed(epoch, 50);
    clicks(10, 0.1, 1);
    int turns = 0, last_dir = 1, prev = slide_frame;
    double t = 0, next = 0;
    double before[2 * ALBUM_COVERS_NUM_SLIDES + 1];
    for (int i = 0; i < 10 || pf_state == pf_scrolling; ) {
        if (i < 10 && t >= next) {
            click(-1);
            next += 0.1;
            i++;
        }
        for (int k = 0; k <= 2 * ALBUM_COVERS_NUM_SLIDES; k++)
            before[k] = turn_deg(center_index - ALBUM_COVERS_NUM_SLIDES + k);
        int c = center_index;
        frame();
        t += frame_s;
        /* No cover jumps: a refresh moves a turn by its share of the way. */
        for (int k = 0; k <= 2 * ALBUM_COVERS_NUM_SLIDES; k++) {
            int i_ = c - ALBUM_COVERS_NUM_SLIDES + k;
            if (abs(i_ - center_index) <= ALBUM_COVERS_NUM_SLIDES)
                assert(dabs(turn_deg(i_) - before[k]) <= 180 * 0.12);
        }
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

/* Random clicks, both ways, at every pace: the motion stays the model's. */
static void check_random(uint32_t epoch)
{
    unsigned seed_ = 99;

    seed(epoch, 100);
    for (int f = 0; f < 3000; f++) {
        seed_ = seed_ * 1103515245u + 12345u;
        if ((seed_ >> 16) % 100 < 25)
            click(((seed_ >> 9) & 3) ? 1 : -1);
        frame();
    }
}

/* A late frame lands where the same refreshes drawn one by one would have,
 * turns and all. */
static void check_late_frames(uint32_t epoch)
{
    seed(epoch, 3);
    clicks(3, 0.03, 1);
    uint32_t from = clock_counter;
    int saved_p = slide_frame, saved_t = target;
    unsigned saved_sample = anim_sampled_frame;
    struct flow_turn saved_turns[FLOW_RING], turns[FLOW_RING];
    memcpy(saved_turns, flow_turns, sizeof flow_turns);
    for (unsigned f = 1; f <= 9; f++)
        draw_at(from, f);
    int p = slide_frame;
    memcpy(turns, flow_turns, sizeof flow_turns);
    slide_frame = saved_p;
    target = saved_t;
    anim_sampled_frame = saved_sample;
    memcpy(flow_turns, saved_turns, sizeof flow_turns);
    pf_state = pf_scrolling;
    step = 1;
    draw_at(from, 9);
    assert(slide_frame == p && !memcmp(turns, flow_turns, sizeof turns));
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
    draw_at(reverse_clock, 90);
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
    setvbuf(stdout, NULL, _IONBF, 0);
    frame_s = ui_frame_period() / 1e6;
    recalc_offsets();
    /* Across the timer's wrap as well as clear of it. */
    uint32_t epochs[] = { 1000, UINT32_MAX - ui_frame_clock(10) };
    for (unsigned e = 0; e < 2; e++) {
        uint32_t epoch = epochs[e];
        flat = false;
        check_single_click(epoch);
        check_shapes(epoch);
        check_fast_stop(epoch);
        check_reversal(epoch);
        check_ends(epoch);
        check_random(epoch);
        check_late_frames(epoch);
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
    printf("largest difference from the model: position %.4f items, turn "
           "%.2f degrees, x %.2f px\n", worst_p, worst_turn * 90, worst_x);
    assert(worst_p < 0.01 && worst_turn * 90 < 1 && worst_x < 1);
    puts("PASS single click, 3/10/20 clicks/s, fast stop, reversal, ends, "
         "random clicks and late frames, against the model");
    puts("PASS flat timeline, frame deadlines and timer wrap");
    return 0;
}
'''
c = out / 'motion.c'
c.write_text(program)
binary = out / 'motion'
subprocess.run(['cc', '-O2', '-std=gnu11', '-fwrapv', '-I',
                root / 'apps-ipod', c, root / 'apps-ipod/ui/animation.c',
                '-o', binary, '-lm'], check=True)
subprocess.run([binary], check=True)

# The scene: pfgeom replays the same motion through the render.
pfgeom = out / 'pfgeom'
subprocess.run(['cc', '-O2', '-std=gnu11', root / 'tools/pfgeom/pfgeom.c',
                '-o', pfgeom, '-lm'], check=True)
report = subprocess.run([pfgeom], check=True, capture_output=True,
                        text=True).stdout
scene = {m.group(1).strip(): (int(m.group(2)), int(m.group(3)))
         for m in re.finditer(r'^  (.+?)\s+\d+ refreshes  crossing\s+(\d+)'
                              r'  painter wrong\s+\d+, worst (\d+) columns',
                              report, re.M)}
table = report[report.index('the scene'):]
print(table[:table.index('\n\n')])
# Slow motion keeps neighbours apart and the painter right to a column.
for name in ('one click', '3 clicks/s'):
    crossing, worst = scene[name]
    assert crossing == 0 and worst <= 1, name
assert 'stale pixels: 0  (pass)' in report
assert 'divisions by zero: 0  (pass)' in report
print('PASS pfgeom: no stale pixels, no division by zero, slow motion '
      'crossing-free')
