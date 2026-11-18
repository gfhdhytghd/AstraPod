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
"""
program += section(bar, "struct indicator_motion {", "struct statusbar {")
program += section(bar, "static int mix_indicator", "static void retire_trace")
program += section(chrome, "void native_chrome_indicators(", "static unsigned int busy_state")
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
    state.link = 1;
    state.dock = true;
    native_chrome_indicators(&state, &active);
    assert(!advance_indicators(&motion, &active, start, 8));
    assert(motion.fade.frames == 8);
    unsigned previous = 0;
    for (unsigned frame = 1; frame < 8; ++frame)
    {
        assert(advance_indicators(&motion, &active, start + ui_frame_clock(frame), 8));
        unsigned ink = motion.visible.ink[CHROME_PLAY];
        assert(ink > previous && ink < UINT8_MAX);
        assert(motion.visible.ink[CHROME_LINK_WAIT] == ink);
        assert(motion.visible.ink[CHROME_DOCK] == ink);
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
    state.link = 0;
    state.dock = false;
    native_chrome_indicators(&state, &idle);
    assert(advance_indicators(&motion, &idle, reverse, 0));
    assert(!motion.fade.frames && motion.visible.battery == -1);
    for (int glyph = 0; glyph < CHROME_GLYPH_COUNT; ++glyph)
        assert(!motion.visible.ink[glyph]);

    puts("PASS 8 f status fade, battery interval, link/dock, timer wrap, reversal and off");
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
