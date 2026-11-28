#!/usr/bin/env python3
"""Exercise the production list wait and glyph repair with a host clock."""
import subprocess
import tempfile
from pathlib import Path

source = (Path(__file__).resolve().parents[2] /
          "apps-ipod/widgets/list_render.c").read_text()


def body(name):
    start = source.index(name + "(")
    start = source.rfind("\n", 0, start)
    return source[start:source.index("\n}", start) + 2]


fixture = r"""
#include <assert.h>
#include <stdbool.h>

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define SHOWN_ROWS 2
#define REPAIR_ROWS 68
#define TIMEOUT_BLOCK -1
#define TIMEOUT_NOBLOCK 0

struct gui_synclist { int unused; };
struct list_geometry { int unused; };
struct ui_rect { int slot, height; };
struct page { struct ui_rect damage; };
struct native_list {
    struct { int slots, skipped[SHOWN_ROWS]; } shown;
    int skipping[SHOWN_ROWS];
    struct page page;
};
static struct native_list render;
static int deadline, switches, paints;
static void yield(void) { ++switches; }
static int motion_deadline(const struct gui_synclist *list, bool poll)
{
    (void)list;
    assert(poll);
    return deadline;
}
static struct ui_rect slot_row(const struct native_list *list,
                              const struct list_geometry *geometry, int slot)
{
    (void)list;
    (void)geometry;
    return (struct ui_rect){slot, 80};
}
static void ui_page_paint(struct page *page)
{
    ++paints;
    render.skipping[page->damage.slot] = 0;
}
"""
cases = r"""
int main(void)
{
    struct gui_synclist list = {0};

    /* A slow marquee paint makes the next frame overdue on every pass. */
    for (int frame = 0; frame < 1000; ++frame)
        assert(list_render_timeout(&list, 10) == 0);
    assert(switches == 1000);

    deadline = 2;
    assert(list_render_timeout(&list, 10) == 2);
    assert(list_render_timeout(&list, TIMEOUT_NOBLOCK) == TIMEOUT_NOBLOCK);
    deadline = TIMEOUT_BLOCK;
    assert(list_render_timeout(&list, 10) == 10);
    assert(switches == 1000);

    /* Two skipped glyph rows, each taller than the per-frame budget. */
    render.shown.slots = 2;
    render.shown.skipped[0] = render.shown.skipped[1] = 1;
    assert(repaint_waiting(&render, NULL));
    assert(paints == 1 && render.skipping[0] == 0);
    render.shown.skipped[0] = 0;
    assert(!repaint_waiting(&render, NULL));
    assert(paints == 2 && render.skipping[1] == 0);
}
"""
with tempfile.TemporaryDirectory() as tmp:
    path = Path(tmp)
    program = path / "probe.c"
    program.write_text("#include <stddef.h>\n" + fixture +
                       body("repaint_waiting") + "\n" +
                       body("list_render_timeout") + cases)
    binary = path / "probe"
    subprocess.run(["cc", "-std=c99", str(program), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print("list redraw liveness: PASS")
