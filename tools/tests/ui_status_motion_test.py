#!/usr/bin/env python3
"""Check production status indicator targets and 60 Hz interpolation."""
from pathlib import Path
import subprocess
import tempfile

apps = Path(__file__).resolve().parents[2] / "apps-ipod"
ui = apps / "ui"
bar = (ui / "statusbar.c").read_text()
chrome = (ui / "native_chrome.c").read_text()


def section(source, first, last):
    return source[source.index(first):source.index(last)]


program = """
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "audio/play_status.h"
#include "animation.h"
#include "native_chrome.h"
#include "native_theme.h"
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
"""
program += section(bar, "struct indicator_motion {", "struct statusbar {")
program += section(bar, "static int mix_indicator", "static void retire_trace")
program += section(chrome, "void native_chrome_indicators(", "static unsigned int busy_state")
program += section(chrome, "struct status_layout {", "static const char *title_text")
program += section(chrome, "static int clock_origin", "static bool clip_viewport")
program += """
int main(void)
{
    struct native_chrome_state state = { .playmode = STATUS_STOP, .battery = 20,
                                        .numeric = true };
    struct native_chrome_indicators idle, active;
    struct indicator_motion motion = {0};
    native_chrome_indicators(&state, &idle);
    uint32_t start = UINT32_MAX - ui_frame_clock(2);
    assert(advance_indicators(&motion, &idle, start, 8));
    assert(!motion.fade.frames && motion.visible.battery == 20);
    state.playmode = STATUS_PLAY;
    state.battery = 80;
    native_chrome_indicators(&state, &active);
    assert(!advance_indicators(&motion, &active, start, 8));
    assert(motion.fade.frames == 8);
    unsigned previous = 0;
    for (unsigned frame = 1; frame < 8; ++frame)
    {
        assert(advance_indicators(&motion, &active, start + ui_frame_clock(frame), 8));
        unsigned ink = motion.visible.ink[CHROME_PLAY];
        assert(ink > previous && ink < UINT8_MAX);
        assert(!motion.visible.ink[CHROME_PAUSE]);
        assert(motion.visible.battery > 20 && motion.visible.battery <= 80);
        previous = ink;
    }
    assert(advance_indicators(&motion, &active, start + ui_frame_clock(8), 8));
    assert(!memcmp(&motion.visible, &active, sizeof(active)));
    assert(!motion.fade.frames);

    start += ui_frame_clock(9);
    advance_indicators(&motion, &idle, start, 8);
    uint32_t reverse = start + ui_frame_clock(3);
    advance_indicators(&motion, &idle, reverse, 8);
    struct native_chrome_indicators intermediate = motion.visible;
    assert(intermediate.ink[CHROME_PLAY] < UINT8_MAX);
    assert(intermediate.ink[CHROME_PLAY] > 0);
    assert(!advance_indicators(&motion, &active, reverse, 8));
    assert(!memcmp(&motion.visible, &intermediate, sizeof(intermediate)));
    assert(advance_indicators(&motion, &active, reverse + ui_frame_clock(8), 8));
    assert(!memcmp(&motion.visible, &active, sizeof(active)));

    state.playmode = STATUS_STOP;
    state.battery = -1;
    native_chrome_indicators(&state, &idle);
    assert(advance_indicators(&motion, &idle, reverse, 0));
    assert(!motion.fade.frames && motion.visible.battery == -1);
    for (int glyph = 0; glyph < CHROME_GLYPH_COUNT; ++glyph)
        assert(!motion.visible.ink[glyph]);

    state.playmode = STATUS_PAUSE;
    native_chrome_indicators(&state, &active);
    assert(!active.ink[CHROME_PLAY] && active.ink[CHROME_PAUSE] == UINT8_MAX);
    state.playmode = STATUS_FASTFORWARD;
    native_chrome_indicators(&state, &active);
    assert(active.ink[CHROME_PLAY] == UINT8_MAX && !active.ink[CHROME_PAUSE]);

    const struct ui_theme theme = { .metric = {
        [UI_METRIC_STATUS_PAD_LEFT] = 10, [UI_METRIC_STATUS_PAD_RIGHT] = 7,
        [UI_METRIC_STATUS_ICON_SIZE] = 24, [UI_METRIC_STATUS_ICON_GAP] = 2,
        [UI_METRIC_STATUS_GROUP_GAP] = 9,
    }};
    struct native_chrome chrome = { .theme = &theme };
    struct status_layout bar = status_layout(&chrome, 320, 72, 0, 48);
    assert(bar.clock_x == 124 && bar.clock_width == 72);
    assert(bar.battery_x == 289 && bar.play_x == 263 && !bar.percent_width);
    assert(bar.busy_x == 237 && !bar.right_clock);

    /* Both built-in themes share these status metrics. Cover short and long
     * titles, the empty title width for playback modes, and both clock formats. */
    const int titles[] = {0, 48, 500};
    const int clocks[] = {46, 90, 200};
    for (unsigned t = 0; t < sizeof(titles) / sizeof(*titles); ++t)
    for (unsigned c = 0; c < sizeof(clocks) / sizeof(*clocks); ++c)
    for (int percent = 0; percent <= 40; percent += 40)
    {
        bar = status_layout(&chrome, 320, clocks[c], percent, titles[t]);
        assert(bar.clock_width == clocks[c]);
        assert(10 + bar.title_width + 8 <= bar.clock_x || !bar.title_width);
        assert(bar.clock_x + bar.clock_width + 9 <= bar.busy_x);
        assert(bar.busy_x + 24 + 2 == bar.play_x);
        assert(bar.play_x + 24 + 2 == (bar.percent_width ? bar.percent_x : bar.battery_x));
        assert(bar.percent_x + bar.percent_width + 2 == bar.battery_x);
        if (titles[t] == 500 || (clocks[c] == 90 && percent))
            assert(bar.right_clock);
        if (clocks[c] < 200)
            assert(bar.percent_width == percent);
        if (bar.right_clock)
            assert(bar.clock_x + bar.clock_width + 9 == bar.busy_x);
        else
            assert(2 * bar.clock_x + bar.clock_width == 320);
    }
    assert(2 * clock_origin(320, 4290) + 4290 == 320 * 64);
    assert(2 * clock_origin(320, 4912) + 4912 == 320 * 64);
    assert(2 * clock_origin(320, 4291) + 4291 == 320 * 64 + 1);

    puts("PASS status fade, play/pause/stop, timer wrap and centered safe layout");
}
"""

with tempfile.TemporaryDirectory(prefix="podbox-status-motion-") as temp:
    work = Path(temp)
    source = work / "probe.c"
    binary = work / "probe"
    source.write_text(program)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                    "-I", str(apps), "-I", str(ui), str(source),
                    str(ui / "animation.c"), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
