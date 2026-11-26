/* The hardware-scroll frame arithmetic
 * (firmware/target/arm/s5l8702/ipod6g/lcd-hwscroll-plan.c), on the host.
 * From the repository root:
 *   gcc -std=gnu11 -Wall -Wextra -I firmware/target/arm/s5l8702/ipod6g \
 *       -o /tmp/lcd_hwscroll_plan_test tools/tests/lcd_hwscroll_plan_test.c \
 *       firmware/target/arm/s5l8702/ipod6g/lcd-hwscroll-plan.c &&
 *   /tmp/lcd_hwscroll_plan_test
 * GPL version 2 or later. */
#include <assert.h>
#include <stdio.h>
#include "lcd-hwscroll-plan.h"

#define TOP 35
#define H (HWSCROLL_ROWS - TOP)

static const struct hwscroll_timing timing = {
    .period = 14940, .high = 1100, .open = 300, .guard = 150,
    .command = 40, .setup = 50, .full = 25500,
};

static int wrap(int offset)
{
    return ((offset % H) + H) % H;
}

/* The ring is a rotation of the area: every screen row maps to a distinct
 * panel row, and runs end exactly where the map jumps. */
static void check_ring(int offset)
{
    struct hwscroll_ring ring = { TOP, H, offset };
    unsigned char seen[HWSCROLL_ROWS] = {0};
    for (int y = 0; y < HWSCROLL_ROWS; y++)
    {
        int p = hwscroll_ring_map(&ring, y);
        assert(p >= 0 && p < HWSCROLL_ROWS);
        assert(y < TOP ? p == y : p >= TOP);
        assert(!seen[p]);
        seen[p] = 1;
        if (y >= TOP)
            assert(p == TOP + (y - TOP + offset) % H);
    }
    for (int y = 0; y < HWSCROLL_ROWS; y++)
    {
        int left = HWSCROLL_ROWS - y;
        int run = hwscroll_ring_run(&ring, y, left);
        assert(run >= 1 && run <= left);
        for (int i = 1; i < run; i++)
            assert(hwscroll_ring_map(&ring, y + i) ==
                   hwscroll_ring_map(&ring, y) + i);
        if (run < left)
            assert(hwscroll_ring_map(&ring, y + run) !=
                   hwscroll_ring_map(&ring, y) + run);
    }
}

static void test_ring(void)
{
    struct hwscroll_ring identity = { TOP, H, 0 };
    assert(hwscroll_ring_run(&identity, 0, HWSCROLL_ROWS) == HWSCROLL_ROWS);
    for (int y = 0; y < HWSCROLL_ROWS; y++)
        assert(hwscroll_ring_map(&identity, y) == y);

    /* Start lines reached by positive and negative shifts, near both ends. */
    int shifts[] = { 1, -1, 30, -30, H - 1, -(H - 1), 7 * H + 3, -5 * H - 2 };
    int offset = 0;
    for (unsigned int i = 0; i < sizeof shifts / sizeof shifts[0]; i++)
    {
        offset = wrap(offset + shifts[i]);
        assert(offset >= 0 && offset < H);
        check_ring(offset);
    }
    check_ring(H - 1);
    check_ring(1);

    /* Offset H - 1: the area's first screen row is the last panel row. */
    struct hwscroll_ring last = { TOP, H, H - 1 };
    assert(hwscroll_ring_map(&last, TOP) == HWSCROLL_ROWS - 1);
    assert(hwscroll_ring_run(&last, TOP, H) == 1);
    assert(hwscroll_ring_map(&last, TOP + 1) == TOP);
    assert(hwscroll_ring_run(&last, TOP + 1, H - 1) == H - 1);
    /* A transfer from above the area stops at its top. */
    assert(hwscroll_ring_run(&last, 0, HWSCROLL_ROWS) == TOP);
}

static void test_segments(void)
{
    struct hwscroll_ring ring = { TOP, H, 100 };
    struct lcd_hwscroll_band band[] = {
        { 0, 210, 320, 30 },    /* exposed strip */
        { 314, TOP, 6, H },     /* scrollbar */
        { 0, 100, 320, 60 },    /* marker, crosses the wrap at 140 */
        { 0, TOP, 320, 10 },    /* same first row, wider: after the bar */
    };
    struct hwscroll_segment seg[HWSCROLL_SEGMENTS];
    int n = hwscroll_plan_segments(&ring, band, 4, seg);
    assert(n == 6);
    /* Wrap row: map(y) == 239 at y - TOP + 100 == H - 1 -> y = 139. */
    int wrap_y = TOP + H - 100;
    const int expect[][4] = {
        { 314, TOP, 6, wrap_y - TOP },
        { 314, wrap_y, 6, HWSCROLL_ROWS - wrap_y },
        { 0, TOP, 320, 10 },
        { 0, 100, 320, wrap_y - 100 },
        { 0, wrap_y, 320, 160 - wrap_y },
        { 0, 210, 320, 30 },
    };
    unsigned int pixel = 0;
    for (int i = 0; i < n; i++)
    {
        assert(seg[i].x == expect[i][0] && seg[i].y == expect[i][1]);
        assert(seg[i].width == expect[i][2] && seg[i].rows == expect[i][3]);
        assert(seg[i].panel_y == hwscroll_ring_map(&ring, seg[i].y));
        assert(seg[i].panel_y + seg[i].rows <= HWSCROLL_ROWS);
        assert(seg[i].pixel == pixel);
        pixel += seg[i].width * seg[i].rows;
        if (i)
            assert(seg[i].y >= seg[i - 1].y ||
                   seg[i].width > seg[i - 1].width);
    }
    assert(seg[1].panel_y == TOP && seg[4].panel_y == TOP);

    struct lcd_hwscroll_band many[LCD_HWSCROLL_BANDS];
    for (int i = 0; i < LCD_HWSCROLL_BANDS; i++)
        many[i] = (struct lcd_hwscroll_band){ 0, TOP, 320, H };
    assert(hwscroll_plan_segments(&ring, many, LCD_HWSCROLL_BANDS, seg) ==
           HWSCROLL_SEGMENTS);
}

static bool feasible(int offset, const struct lcd_hwscroll_band *band,
                     int count, int *close)
{
    struct hwscroll_ring ring = { TOP, H, offset };
    struct hwscroll_segment seg[HWSCROLL_SEGMENTS];
    int open, n = hwscroll_plan_segments(&ring, band, count, seg);
    assert(n >= 0);
    bool ok = hwscroll_plan_window(&timing, TOP, seg, n, &open, close);
    assert(open == timing.open);
    return ok;
}

static void test_window(void)
{
    int close;
    /* Slow scroll: two rows exposed, the marker moving 2 rows, the bar. */
    struct lcd_hwscroll_band slow[] = {
        { 314, TOP, 6, H }, { 0, 118, 314, 32 }, { 0, 238, 320, 2 },
    };
    assert(feasible(17, slow, 3, &close));
    printf("slow scroll: close %d us\n", close);

    /* Fast spin: strip 30 rows, marker 60 rows, 6 x 205 bar. */
    struct lcd_hwscroll_band fast[] = {
        { 0, 210, 320, 30 }, { 0, 100, 320, 60 }, { 314, TOP, 6, H },
    };
    for (int offset = 0; offset < H; offset += 17)
        assert(feasible(offset, fast, 3, &close));
    printf("fast spin: close %d us\n", close);
    /* 0x37 stays inside the TE pulse. */
    assert(close <= timing.high - timing.guard - timing.command);

    struct lcd_hwscroll_band header[] = { { 0, TOP, 320, 70 } };
    assert(!feasible(0, header, 1, &close));
    printf("70-row header at %d: close %d us, rejected\n", TOP, close);

    struct lcd_hwscroll_band full[] = { { 0, TOP, 320, H } };
    assert(!feasible(50, full, 1, &close));
    printf("full area: close %d us, rejected\n", close);

    assert(feasible(0, NULL, 0, &close));
    assert(close == timing.high - timing.guard - timing.command);
}

static void test_rise(void)
{
    const uint32_t p = 14940;
    uint32_t base = 0xfffff000u;    /* across the timer's wrap */
    assert(hwscroll_plan_rise(base, p, base) == base);
    assert(hwscroll_plan_rise(base, p, base + 1) == base + p);
    assert(hwscroll_plan_rise(base, p, base + p) == base + p);
    assert(hwscroll_plan_rise(base, p, base + 3 * p - 5) == base + 3 * p);
    assert(hwscroll_plan_rise(base, p, base - 1) == base);
    assert(hwscroll_plan_rise(base, p, base - p) == base - p);
    assert(hwscroll_plan_rise(base, p, base - p - 1) == base - p);
    assert(hwscroll_plan_rise(base, p, base - 2 * p - 1) == base - 2 * p);
}

int main(void)
{
    test_ring();
    test_segments();
    test_window();
    test_rise();
    puts("lcd_hwscroll_plan_test: ok");
    return 0;
}
