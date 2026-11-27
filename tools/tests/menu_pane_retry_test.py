"""Run the firmware's menu-pane worker and retry path with host stubs."""

from pathlib import Path
import re
import subprocess
import tempfile


root = Path(__file__).resolve().parents[2]
source = (root / "apps-ipod/ui/menu_pane.c").read_text()


def function(name):
    match = re.search(r"^static void " + name + r"\([^;]*?\)\n\{", source, re.M)
    return source[match.start():source.index("\n}", match.end()) + 2]


stubs = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MENU_PANE_X 140
typedef uint16_t fb_data;
struct bg_task { int unused; };
struct queue_event { int unused; };
struct ui_rect { int x, y, width, height; };
static struct bg_task show_task;
static uint32_t USEC_TIMER;
static bool storage_pending;
static int posts, pins, loads;
static fb_data buffer;
static int core_free(int handle) { assert(handle == 1 && !pins); return 0; }
static int tagcache_album_count(void) { return 1; }
static fb_data *core_get_data_pinned(int handle)
{
    assert(handle == 1 && !pins);
    pins++;
    return &buffer;
}
static void core_put_data_pinned(fb_data *pixels)
{
    assert(pixels == &buffer && pins == 1);
    pins--;
}
static bool bg_task_should_stop(const struct bg_task *task)
{
    assert(task == &show_task);
    return storage_pending;
}
static int load_cover(fb_data *pixels, int album)
{
    assert(pixels == &buffer && pins == 1 && album == 0 && !storage_pending);
    loads++;
    return 1;
}
static bool bg_task_post(struct bg_task *task, long id)
{
    assert(task == &show_task && id == SHOW_EVENT);
    posts++;
    return true;
}
static int core_alloc_free_space(size_t bytes) { assert(bytes > 0); return 1; }
static size_t core_available(void) { return 0; }
static size_t core_allocatable(void) { return 0; }

/* Rendering is outside the interrupted-load and retry paths. */
static struct ui_rect menu_pane_rect(void) { return (struct ui_rect){0}; }
static void show_begin(uint32_t now) { (void)now; abort(); }
static void show_window(uint32_t now, int *x, int *y)
{ (void)now; (void)x; (void)y; abort(); }
static void blend_cover(struct ui_rect r, int x, int y, unsigned weight)
{ (void)r; (void)x; (void)y; (void)weight; abort(); }
static void paint_cover(struct ui_rect r, int x, int y, int left)
{ (void)r; (void)x; (void)y; (void)left; abort(); }
static void shade(struct ui_rect r) { (void)r; abort(); }
static bool storage_disk_is_active(void) { abort(); }
"""

test = r"""
int main(void)
{
    struct ui_rect damage = {0};
    USEC_TIMER = 123456;
    show_request();
    assert(posts == 1 && show.state == SHOW_REQUESTED);

    /* USB remains pending while the worker returns to its queue. */
    storage_pending = true;
    show_work(NULL);
    assert(posts == 1 && "interrupted worker must not enqueue itself");
    assert(!pins && !loads && probe.yielded == 1);
    assert(show.state == SHOW_NONE && show.none_at == USEC_TIMER);

    storage_pending = false;
    USEC_TIMER += SHOW_LIFE_US - 1;
    cover_step(USEC_TIMER, &damage);
    assert(posts == 1);
    USEC_TIMER++;
    cover_step(USEC_TIMER, &damage);
    assert(posts == 2 && show.state == SHOW_REQUESTED);
    show_work(NULL);
    assert(loads == 1 && !pins && show.state == SHOW_FRESH);

    storage_pending = true;
    show_request();
    show.release = true;
    show_work(NULL);
    assert(posts == 3 && !pins && loads == 1);
    assert(show.state == SHOW_EMPTY && !show.handle && !show.release);
    return 0;
}
"""

constants = "\n".join(
    line for line in source.splitlines()
    if line.startswith(("#define SHOW_", "#define FADE_US", "#define IDLE_DWELL_US"))
)
state = (
    source[source.index("enum show_state"):source.index("static struct {", source.index("enum show_state"))]
    + source[source.index("static struct {", source.index("/* The slideshow.")):
             source.index("static struct menu_pane_content shown")]
    + source[source.index("static struct {\n    unsigned requests"):
             source.index("/* A still pane")]
)

with tempfile.TemporaryDirectory() as directory:
    test_source = Path(directory) / "menu_pane_retry.c"
    binary = Path(directory) / "menu_pane_retry"
    test_source.write_text(
        constants + "\n" + stubs + state
        + function("show_work") + "\n" + function("show_request")
        + "\n" + function("cover_step") + "\n" + test
    )
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    str(test_source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print("menu pane: interrupted load returns without reposting; delayed retry and release PASS")
