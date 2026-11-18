"""FU53: retiring a list releases retained state; a retired list cannot redraw it."""
from pathlib import Path
import re
import subprocess
import sys

root = Path(__file__).resolve().parents[2]
out = Path(sys.argv[1]).resolve()
out.mkdir(parents=True, exist_ok=True)
list_source = (root / "apps-ipod/widgets/list.c").read_text()
renderer = (root / "apps-ipod/widgets/list_render.c").read_text()


def function(source, name):
    start = re.search(r"(?m)^(?:static )?(?:bool|void|int|unsigned) " + name + r"\([^;{}]*\)\s*\{",
                      source)
    assert start, name
    end, depth = start.end(), 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start.start():end]


fixture = r"""
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#define SCREEN_MAIN 0
#define UNLIKELY(x) (x)
#define USEC_TIMER 0u
#define HZ 100
#define UI_ANIM_FPS 60u
#define TIMEOUT_BLOCK -1

struct gui_synclist {
    const int *parent[1];
    unsigned generation;
};
struct list_motion {
    int visible;
    unsigned painted, trace;
    uint32_t started;
};
struct native_list {
    struct gui_synclist *owner;
    unsigned generation;
    bool has_marquee;
    int art_handle, backdrop;
    struct list_motion motion;
    uint32_t marquee_started, marquee_frame;
};
static struct native_list state;
static int renderer_handle = 1;
static struct { bool scrolling_enabled; } global_settings = {true};
static int drawn, laid_out, released, traces_ended;
static void ui_perf_end(unsigned token) { traces_ended += token != 0; }
static bool motion_enabled(void) { return true; }
static unsigned motion_duration(const struct list_motion *motion)
{ assert(motion->visible); return 8; }
static int ui_clock_poll_timeout(uint32_t elapsed, unsigned duration, unsigned hz)
{ assert(elapsed == 0 && duration == 9 && hz == HZ); return 1; }
static void *core_get_data_pinned(int handle)
{ assert(handle == 1); return &state; }
static void core_put_data_pinned(void *data)
{ assert(data == &state); }
static void ui_layer_free(int *layer) { *layer = 0; released++; }
static bool ui_scrolling_held(void) { return false; }
static unsigned marquee_frames(void) { return 3; }
static uint32_t ui_clock_frame(uint32_t clock) { return clock; }
static void list_render_draw(struct gui_synclist *list)
{ assert(list->parent[0]); drawn++; }
static void gui_synclist_layout(struct gui_synclist *list)
{ assert(list->parent[0]); laid_out++; }

@MOTION_OWNER@
@MOTION_STOP@
@FORGET_RENDERER@
@FORGET_LIST@
@DRAW_LIST@
@MOTION_DEADLINE@

int main(void)
{
    int vp = 1;
    struct gui_synclist a = {{&vp}, 1};
    struct gui_synclist b = {{&vp}, 2};
    struct gui_synclist no_parent = {{NULL}, 3};

    state.owner = &a;
    state.generation = a.generation;
    state.has_marquee = true;
    state.art_handle = 9;
    state.backdrop = 4;
    state.motion.visible = 6;
    state.motion.trace = 1;
    gui_synclist_forget(&b);
    assert(motion_deadline(&a, true) == 1 && motion_deadline(&b, true) == TIMEOUT_BLOCK);
    assert(released == 0 && state.backdrop == 4);

    a.generation++;
    assert(motion_deadline(&a, true) == TIMEOUT_BLOCK);
    gui_synclist_forget(&a);
    assert(!state.owner && !state.has_marquee && state.art_handle == -1);
    assert(released == 1 && state.backdrop == 0);
    assert(traces_ended == 1 && state.motion.visible == 0);

    gui_synclist_draw(&no_parent);
    assert(laid_out == 0 && drawn == 0);
    gui_synclist_draw(&b);
    assert(laid_out == 1 && drawn == 1);
    puts("Retained list owner, recycled address, and missing viewport: PASS");
}
"""
for token, source, name in (
    ("@MOTION_OWNER@", renderer, "motion_owner"),
    ("@MOTION_STOP@", renderer, "motion_stop"),
    ("@FORGET_RENDERER@", renderer, "list_render_forget"),
    ("@FORGET_LIST@", list_source, "gui_synclist_forget"),
    ("@DRAW_LIST@", list_source, "gui_synclist_draw"),
    ("@MOTION_DEADLINE@", renderer, "motion_deadline"),
):
    fixture = fixture.replace(token, function(source, name))

source_path = out / "fu53_list.c"
source_path.write_text(fixture)
binary = out / "fu53_list"
subprocess.run(["gcc", "-std=gnu11", "-Wall", "-Werror",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-fno-pie", "-no-pie", "-g", str(source_path), "-o", str(binary)],
               check=True)
subprocess.run([str(binary)], check=True)
