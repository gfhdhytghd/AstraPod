#!/usr/bin/env python3
"""Exercise the retained UI against a small RGB565 framebuffer."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
ui = root / "apps-ipod/ui"

stubs = {
    "config.h": "",
    "system.h": """#define MAX(a,b) ((a)>(b)?(a):(b))
#define MIN(a,b) ((a)<(b)?(a):(b))
extern unsigned test_usec_timer;
#define USEC_TIMER (++test_usec_timer)
""",
    "core_alloc.h": """#include <stddef.h>
int core_alloc_free_space(size_t n);
int core_free(int handle);
void *core_get_data_pinned(int handle);
void core_put_data_pinned(const void *data);
""",
    "lcd.h": """#pragma once
#include <stdbool.h>
#include <stdint.h>
#define LCD_WIDTH 12
#define LCD_HEIGHT 10
typedef uint16_t fb_data;
extern struct test_fb { fb_data *fb_ptr; } lcd_framebuffer_default;
struct viewport;
struct viewport *lcd_set_viewport(struct viewport *);
void lcd_update_rect(int x, int y, int width, int height);
bool lcd_updates_held(void);
bool lcd_active(void);
""",
    "draw/screen_access.h": """#pragma once
#include "lcd.h"
enum screen_type { SCREEN_MAIN };
#define NB_SCREENS 1
#define FOR_NB_SCREENS(i) for (int i = 0; i < NB_SCREENS; ++i)
struct screen {
    int lcdwidth, lcdheight;
    struct viewport *(*set_viewport)(struct viewport *);
    void (*update_rect)(int, int, int, int);
};
extern struct screen screens[NB_SCREENS];
""",
}

harness = r"""
#include "retained.h"
#include "frame.h"
#include "lcd.h"
#include <assert.h>
#include <stdio.h>

static fb_data pixels[LCD_WIDTH * LCD_HEIGHT], buffer[LCD_WIDTH * LCD_HEIGHT];
struct screen screens[NB_SCREENS] = {
    { LCD_WIDTH, LCD_HEIGHT, lcd_set_viewport, lcd_update_rect },
};
struct test_fb lcd_framebuffer_default = { pixels };
unsigned test_usec_timer;
static struct ui_rect updated;
static int submissions, paints, pin_count, allocations;
static bool allocator_ready = true;
static bool hold_lcd;
static unsigned duration_scale = 100;
unsigned ui_theme_duration(unsigned frames) { return frames * duration_scale / 100; }
void ui_perf_note_render(uint32_t elapsed) { (void)elapsed; }
void ui_perf_note_submit(uint32_t elapsed) { (void)elapsed; }

int core_alloc_free_space(size_t n)
{
    assert(n <= sizeof(buffer));
    if (!allocator_ready)
        return -1;
    assert(!allocations && !pin_count);
    allocations++;
    return 1;
}
int core_free(int handle)
{
    assert(handle == 1 && allocations == 1 && !pin_count);
    allocations--;
    return 0;
}
void *core_get_data_pinned(int handle)
{
    assert(handle == 1 && allocations == 1);
    pin_count++;
    return buffer;
}
void core_put_data_pinned(const void *data)
{
    assert(data == buffer && pin_count == 1);
    pin_count--;
}
struct viewport *lcd_set_viewport(struct viewport *vp)
{
    (void)vp;
    return NULL;
}
void lcd_update_rect(int x, int y, int width, int height)
{
    updated = (struct ui_rect){ x, y, width, height };
    ++submissions;
}
bool lcd_updates_held(void) { return hold_lcd; }
bool lcd_active(void) { return true; }

static void fill(const struct ui_view *v, const struct ui_view_frame *frame)
{
    ++paints;
    struct ui_rect r = frame->clip;
    assert(r.x >= 0 && r.y >= 0 && r.x + r.width <= LCD_WIDTH && r.y + r.height <= LCD_HEIGHT);
    for (int y = r.y; y < r.y + r.height; ++y)
        for (int x = r.x; x < r.x + r.width; ++x)
            pixels[y * LCD_WIDTH + x] = *(const uint16_t *)v->data;
}

static int dispatch;
static bool child_action(struct ui_view *v, int action)
{
    (void)v;
    ++dispatch;
    return action == 7;
}
static bool root_action(struct ui_view *v, int action)
{
    (void)v;
    dispatch += 3;
    return action == 8;
}

static struct ui_page *physics_page;
static void physics(struct ui_view *view, uint32_t clock)
{
    ui_page_set(physics_page, view, UI_SELECTION, ui_clock_frame(clock));
}

static void assign_layout(struct ui_view *v, const struct ui_rect *parent)
{
    assert(parent->width == LCD_WIDTH && parent->height == LCD_HEIGHT);
    v->value[UI_X] = 2;
}

int main(void)
{
    struct ui_view page_view, chip, overlay_view;
    struct ui_page page, overlay;
    struct ui_nav nav = {0};
    uint16_t background = 0x001f, chip_color = 0x07e0;
    const struct ui_view_ops outer = { .paint = fill, .action = root_action };
    const struct ui_view_ops inner = { .paint = fill, .action = child_action,
      .step = physics, .layout = assign_layout };
    const struct ui_view_ops cached = { .paint = ui_layer_paint };

    ui_view_init(&page_view, (struct ui_rect){0, 0, LCD_WIDTH, LCD_HEIGHT}, &outer, &background);
    ui_view_init(&chip, (struct ui_rect){ 1, 2, 3, 2 }, &inner, &chip_color);
    ui_view_append(&page_view, &chip);
    ui_page_init(&page, &page_view);
    physics_page = &page;
    page.focus = &chip;
    ui_page_layout(&page);
    assert(ui_page_dispatch(&page, 7) && dispatch == 1);
    assert(ui_page_dispatch(&page, 8) && dispatch == 5);

    struct ui_rect damage = ui_page_paint(&page);
    assert(damage.width == LCD_WIDTH && damage.height == LCD_HEIGHT);
    assert(!submissions && paints == 2 && pixels[2 * LCD_WIDTH + 2] == chip_color);
    ui_page_draw(&page);
    assert(!submissions && paints == 2);

    ui_page_set(&page, &chip, UI_OFFSET_X, 2);
    ui_page_draw(&page);
    assert(submissions == 1 && updated.x == 2 && updated.y == 2 &&
           updated.width == 5 && updated.height == 2);
    assert(pixels[2 * LCD_WIDTH + 2] == background);
    assert(pixels[2 * LCD_WIDTH + 4] == chip_color);

    struct ui_track track, reversal;
    uint32_t start = UINT32_MAX - ui_frame_clock(2);
    ui_page_tween(&page, &track, &chip, UI_OFFSET_X, 7, ANIM_F(8), false, start);
    int pose = ui_tween_sample(&track.tween, 3);
    uint32_t reverse_at = start + ui_frame_clock(3);
    ui_page_tween(&page, &reversal, &chip, UI_OFFSET_X, 0, ANIM_F(10), false, reverse_at);
    assert(page.tracks == &reversal && chip.value[UI_OFFSET_X] == pose);
    assert(ui_page_dispatch(&page, 7));
    ui_page_step(&page, reverse_at + ui_frame_clock(18));
    assert(chip.value[UI_OFFSET_X] == 0 && !page.tracks && chip.value[UI_SELECTION] > 10);
    ui_page_draw(&page);

    duration_scale = 0;
    ui_page_tween(&page, &track, &chip, UI_OFFSET_X, 1, ANIM_F(8), false, start);
    assert(chip.value[UI_OFFSET_X] == 1 && !page.tracks);
    duration_scale = 100;

    struct ui_layer layer = {0};
    allocator_ready = false;
    assert(!ui_layer_alloc(&layer, 2, 2) && !layer.handle);
    allocator_ready = true;
    assert(ui_layer_alloc(&layer, 2, 2));
    for (int i = 0; i < 4; ++i)
        buffer[i] = 0xf800;
    ui_view_init(&overlay_view, (struct ui_rect){8, 3, 3, 2}, &cached, &layer);
    ui_page_init(&overlay, &overlay_view);
    ui_page_set(&overlay, &overlay_view, UI_OPACITY, UI_FULL_OPACITY / 2);
    ui_page_set(&overlay, &overlay_view, UI_BRIGHTNESS, UI_FULL_OPACITY / 2);
    ui_page_set(&overlay, &overlay_view, UI_CLIP_WIDTH, 2);
    assert(ui_nav_push(&nav, &page, UI_PAGE));
    assert(ui_nav_push(&nav, &overlay, UI_SHEET));
    assert(!ui_nav_dispatch(&nav, 7));
    ui_nav_draw(&nav);
    assert(pixels[3 * LCD_WIDTH + 8] == ((7 << 11) | 15));
    assert(pixels[3 * LCD_WIDTH + 10] == background);
    assert(!pin_count);
    assert(ui_nav_pop(&nav) == &overlay);
    ui_layer_free(&layer);
    assert(!allocations);
    ui_nav_draw(&nav);
    assert(pixels[3 * LCD_WIDTH + 8] == background);
    assert(ui_nav_dispatch(&nav, 7));

    unsigned count = ui_flush_count();
    ui_inhibit_flush(true);
    ui_mark_dirty_rect(SCREEN_MAIN, -1, 0, 4, 1);
    ui_page_invalidate(&page, (struct ui_rect){1, 5, 3, 2});
    ui_page_draw(&page);
    assert(ui_flush_count() == count);
    ui_inhibit_flush(false);
    hold_lcd = true;
    ui_flush_dirty();
    assert(ui_flush_count() == count);
    hold_lcd = false;
    ui_flush_dirty();
    assert(ui_flush_count() == count + 1 &&
           updated.x == 0 && updated.y == 0 &&
           updated.width == 4 && updated.height == 7);
    puts("PASS retained layout, focus, damage, retarget, layers and navigation");
}
"""

with tempfile.TemporaryDirectory(prefix="podbox-retained-test-") as temp:
    port = Path(temp)
    for name, text in stubs.items():
        path = port / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
    (port / "test.c").write_text(harness)
    binary = port / "test"
    subprocess.run(["cc", "-std=c11", "-Werror", "-Wall", "-Wextra",
                    "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                    "-I", str(port), "-I", str(ui), str(port / "test.c"),
                    str(ui / "retained.c"), str(ui / "frame.c"),
                    str(ui / "animation.c"), "-o", str(binary)],
                   check=True)
    subprocess.run([str(binary)], check=True)
