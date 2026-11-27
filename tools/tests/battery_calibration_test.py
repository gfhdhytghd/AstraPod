#!/usr/bin/env python3
"""Run the iPod Classic calibration and status bar against synthetic traces."""

import os
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
SYSTEM = ROOT / "apps-ipod/system"
TARGET = ROOT / "firmware/target/arm/s5l8702/ipod6g"

HARNESS = r"""
#include <assert.h>
#include <stdio.h>
#include "battery_calibration_model.h"
#include "battery-curve-6g.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define IPOD_6G 1
#define POWER_THREAD_STEP_TICKS 50

static unsigned long current_tick;
static int millivolts;
static int full_mv = BATTERY_STOCK_FULL_MV;
static bool display_full;

static int battery_calibration_icon_level(int percent)
{
    (void)percent;
    display_full = battery_cal_full(millivolts, full_mv, display_full);
    return display_full ? 15 : battery_cal_stock_level(millivolts, full_mv);
}

/* ICON UPDATE */

static void good_cycle(void)
{
    struct battery_cal_trace trace = {0};
    struct battery_cal_sample sample;
    for (unsigned s = 0; s < 90 * 60; s += 5)
    {
        bool complete = s >= 80 * 60;
        int mv = complete ? 4200 + (s % 3) - 1 : 3860 + s / 15;
        assert(battery_cal_step(&trace, s, BATTERY_ON_CHARGER,
                 complete, mv, &sample) == BATTERY_CAL_NONE);
    }
    unsigned at = 90 * 60;
    assert(battery_cal_step(&trace, at, BATTERY_ON_CHARGER, true, 4200,
                 &sample) == BATTERY_CAL_CHARGE);
    assert(sample.mv == 4200 && sample.count == 121);
    at += 5;
    assert(battery_cal_step(&trace, at, BATTERY_ON_CHARGER, true, 4200,
                 &sample) == BATTERY_CAL_NONE);

    for (unsigned age = 0; age <= 180; age += 5)
    {
        bool light_load = age >= 120 && age != 125 && age != 135;
        int mv = light_load ? 4140 + (age % 3) - 1 : 4030;
        enum battery_cal_event expected = age == 180 ?
                                          BATTERY_CAL_DISCHARGE :
                                          BATTERY_CAL_NONE;
        assert(battery_cal_step(&trace, at + 5 + age, BATTERY_ON_BATTERY,
                 light_load, mv, &sample) == expected);
    }
    assert(sample.mv == 4140 && sample.count == 11);
    assert(battery_cal_smooth(0, sample.mv) == 4140);
    assert(battery_cal_smooth(4160, sample.mv) == 4150);

    full_mv = battery_cal_full_mv(4140, false);
    assert(full_mv == 4060);
    unsigned short table[11];
    battery_cal_curve(table, full_mv);
    assert(table[0] == 3569 && table[10] == 4060);
    assert(table[5] == 3806);
    assert(battery_cal_full_mv(4200, true) == 4200);
    assert(battery_cal_full_mv(0, true) == 4011);
    assert(battery_cal_full_mv(0, false) == 4011);

    full_mv = battery_cal_full_mv(4200, true);
    millivolts = 4200;
    assert(ui_art_battery_index(100) == 22);
    full_mv = battery_cal_full_mv(4140, false);
    for (int sag = 20; sag <= 40; sag += 5)
    {
        current_tick += POWER_THREAD_STEP_TICKS;
        millivolts = 4140 - sag;
        assert(ui_art_battery_index(100) == 22);
    }
    millivolts = full_mv - 10;
    current_tick += POWER_THREAD_STEP_TICKS;
    assert(ui_art_battery_index(99) == 22);
    millivolts = full_mv - 13;
    current_tick += POWER_THREAD_STEP_TICKS;
    assert(ui_art_battery_index(99) == 21);
    assert(ui_art_battery_index(99) == 21);
    current_tick += POWER_THREAD_STEP_TICKS;
    assert(ui_art_battery_index(99) == 20);
    millivolts = 3568;
    for (int index = 19; index >= 0; index--)
    {
        current_tick += POWER_THREAD_STEP_TICKS;
        assert(ui_art_battery_index(0) == (unsigned)index);
    }
}

static void rejected_cycles(void)
{
    struct battery_cal_trace trace = {0};
    struct battery_cal_sample sample;
    for (unsigned s = 0; s < 89 * 60; s += 5)
        assert(battery_cal_step(&trace, s, BATTERY_ON_CHARGER, true,
                 4160, &sample) == BATTERY_CAL_NONE);
    for (unsigned s = 89 * 60; s <= 93 * 60; s += 5)
        assert(battery_cal_step(&trace, s, BATTERY_ON_BATTERY, true,
                 4100, &sample) == BATTERY_CAL_NONE);

    trace = (struct battery_cal_trace){0};
    for (unsigned s = 0; s <= 120 * 60; s += 5)
        assert(battery_cal_step(&trace, s, BATTERY_ON_CHARGER, true,
                 s % 2 ? 4210 : 4180, &sample) == BATTERY_CAL_NONE);
    assert(!trace.plateau_mv);

    trace = (struct battery_cal_trace){0};
    for (unsigned s = 0; s < 90 * 60; s += 5)
        assert(battery_cal_step(&trace, s, BATTERY_ON_CHARGER, true,
                 4260, &sample) == BATTERY_CAL_NONE);
    assert(battery_cal_step(&trace, 90 * 60, BATTERY_ON_CHARGER, true,
                 4200, &sample) == BATTERY_CAL_NONE);
    assert(battery_cal_step(&trace, 91 * 60, BATTERY_ON_CHARGER, true,
                 4200, &sample) == BATTERY_CAL_NONE);
    assert(trace.plug_at == 91 * 60 && trace.count == 1);

    trace = (struct battery_cal_trace){0};
    for (unsigned s = 0; s <= 90 * 60; s += 5)
        battery_cal_step(&trace, s, BATTERY_ON_CHARGER, true, 4170, &sample);
    assert(trace.plateau_mv);
    for (unsigned s = 90 * 60 + 5; s <= 94 * 60; s += 5)
        assert(battery_cal_step(&trace, s, BATTERY_ON_BATTERY, true,
                 s % 2 ? 4100 : 4140, &sample) == BATTERY_CAL_NONE);
}

static void stock_curve(void)
{
    static const unsigned short expected[] = { BATTERY_STOCK_DECILES };
    static const int adc[] = {
        675, 721, 732, 750, 761, 769, 776, 784,
        792, 801, 825, 847, 865, 877, 901,
    };
    int thresholds[15];
    for (int i = 0; i < 15; i++)
        thresholds[i] = 2250 + adc[i] * 2000 / 1023;

    unsigned short table[11];
    battery_cal_curve(table, BATTERY_STOCK_FULL_MV);
    for (unsigned i = 0; i < 10; i++)
    {
        unsigned index = i * 14 / 10;
        unsigned fraction = i * 14 % 10;
        int mv = (thresholds[index] * (10 - fraction) +
                  thresholds[index + 1] * fraction + 5) / 10;
        assert(table[i] == expected[i] && table[i] == mv);
    }
    assert(table[10] == expected[10] && table[10] == thresholds[14]);

    static const unsigned frames[] = {
        1, 2, 4, 5, 7, 8, 10, 11, 13, 14, 16, 17, 18, 20, 22,
    };
    for (int i = 0; i < 15; i++)
    {
        assert(battery_cal_stock_level(thresholds[i] - 1, 4011) == i);
        assert(battery_cal_stock_level(thresholds[i], 4011) == i + 1);
        millivolts = thresholds[i];
        assert(ui_art_battery_index(0) == frames[i]);
    }
}

int main(void)
{
    stock_curve();
    good_cycle();
    rejected_cycles();
    puts("battery calibration traces: passed");
    return 0;
}
"""


def main():
    theme = (ROOT / "apps-ipod/ui/theme_art.c").read_text()
    start = theme.index("unsigned ui_art_battery_index(")
    end = theme.index("enum stock_art_id ui_art_battery(", start)
    program = HARNESS.replace("/* ICON UPDATE */", theme[start:end])
    with tempfile.TemporaryDirectory() as tmp:
        source = Path(tmp) / "trace.c"
        binary = Path(tmp) / "trace"
        source.write_text(program)
        subprocess.run([
            os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra",
            "-Werror", "-I", SYSTEM, "-I", TARGET, source,
            SYSTEM / "battery_calibration_model.c", "-o", binary,
        ], check=True)
        subprocess.run([binary], check=True)


if __name__ == "__main__":
    main()
