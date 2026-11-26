#!/usr/bin/env python3
"""ui_pace's half-rate lock: a locked animation presents every second
refresh from its first frame on, whatever each frame costs; an unlocked one
still starts at full rate.

Argument: OUTPUT_DIRECTORY. Compiles begin(), measure() and ui_pace_next()
from apps-ipod/ui/pace.c against a simulated clock.
"""
from pathlib import Path
import re
import subprocess
import sys

root = Path(__file__).resolve().parents[2]
out = Path(sys.argv[1]).resolve()
out.mkdir(parents=True, exist_ok=True)
pace = (root / "apps-ipod/ui/pace.c").read_text()
header = (root / "apps-ipod/ui/pace.h").read_text()


def function(source, name):
    start = re.search(r"(?m)^(?:static )?(?:void|uint32_t) " + name +
                      r"\([^;{}]*\)\s*\{", source)
    assert start, name
    end, depth = start.end(), 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start.start():end]


struct = re.search(r"struct ui_pace \{.*?\n\};", header, re.S).group(0)

fixture = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define LATE_LIMIT 2
#define RECOVERY_FRAMES 4
#define PERIOD 14945u
static uint32_t now_us = 1000000;
#define USEC_TIMER now_us
static bool ui_te_pacing = true;
static const void *last_pace;
static uint32_t ui_frame_period(void) { return PERIOD; }
static unsigned ui_clock_frame(uint32_t clock) { return clock / PERIOD; }
static uint32_t ui_frame_clock(unsigned frame) { return frame * PERIOD; }
@STRUCT@
@BEGIN@
@MEASURE@
@NEXT@

/* Run an animation whose frames cost `cost` periods (the first `first`),
 * and return the refreshes between successive deadlines in `steps`. */
static void run(bool half_rate, double first, double cost, unsigned *steps,
                int count)
{
    struct ui_pace pace = {0};
    pace.half_rate = half_rate;
    uint32_t start = now_us - now_us % PERIOD;
    now_us = start + (uint32_t)(first * PERIOD);
    uint32_t due = ui_pace_next(&pace, start), last = start;
    for (int i = 0; i < count; i++)
    {
        steps[i] = (due - last + PERIOD / 2) / PERIOD;
        last = due;
        now_us = due + (uint32_t)(cost * PERIOD);
        due = ui_pace_next(&pace, start);
    }
}

int main(void)
{
    unsigned steps[40];
    /* Locked: cheap frames, a 2.8-period first frame, and 1.6-period frames
     * all keep two refreshes between deadlines after the first. */
    const double firsts[] = { 0.2, 2.8, 0.2 }, costs[] = { 0.3, 0.5, 1.6 };
    for (int c = 0; c < 3; c++)
    {
        run(true, firsts[c], costs[c], steps, 40);
        for (int i = 1; i < 40; i++)
            assert(steps[i] == 2);
        assert(steps[0] % 2 == 0 && steps[0] >= 2);
    }
    /* Unlocked: cheap frames run at every refresh, as before. */
    run(false, 0.2, 0.3, steps, 40);
    for (int i = 1; i < 40; i++)
        assert(steps[i] == 1);
    puts("PASS locked pacing presents every second refresh; unlocked stays at full rate");
    return 0;
}
"""
fixture = (fixture.replace("@STRUCT@", struct)
           .replace("@BEGIN@", function(pace, "begin"))
           .replace("@MEASURE@", function(pace, "measure"))
           .replace("@NEXT@", function(pace, "ui_pace_next")))
source = out / "pace_half_rate.c"
source.write_text(fixture)
binary = out / "pace_half_rate"
subprocess.run(["gcc", "-std=gnu11", "-Wall", "-Wno-unused-function",
                "-Wno-unused-parameter", "-fsanitize=address,undefined",
                "-fno-sanitize-recover=all", "-g", str(source), "-o",
                str(binary)], check=True)
result = subprocess.run([str(binary)], check=True, capture_output=True,
                        text=True)
print(result.stdout, end="")
