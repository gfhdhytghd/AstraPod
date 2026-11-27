#!/usr/bin/env python3
"""Exercise the actual button tick with held physical and inline-remote keys."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
driver = (ROOT / "firmware/drivers/button.c").read_text()
source = "\n".join(line for line in driver.splitlines()
                   if line.startswith("#include") and "poweroff.h" in line)
source += """
#include "button.h"
#include <assert.h>

static long lastbtn;
static int lastdata, held, requests, volume_repeats;
static bool enable_sw_poweroff = true;

static void button_remote_post(void) {}
static void check_audio_peripheral_state(void) {}
static void reset_poweroff_timer(void) {}
static void sys_poweroff(void) { requests++; }
static bool button_chord_blocks_poweroff(int key) { return false; }
static int button_read(int *data) { return held; }

bool button_queue_try_post(long key, int data)
{
    if (key == (BUTTON_MULTIMEDIA_VOLUME_UP | BUTTON_REPEAT))
        volume_repeats++;
    return true;
}
"""
source += driver[driver.index("#define REPEAT_START"):
                 driver.index("static int lastdata")]
start = driver.index("static void button_tick(void)")
source += driver[start:driver.index("\n}\n", start) + 3]
source += """
static void press(int key)
{
    held = key;
    for (int i = 0; i < 350; i++)
        button_tick();
}

int main(void)
{
    press(BUTTON_MULTIMEDIA_VOLUME_UP);
    assert(volume_repeats > POWEROFF_COUNT);
    assert(requests == 0);
    press(BUTTON_MULTIMEDIA_VOLUME_DOWN);
    assert(requests == 0);
    press(BUTTON_RC_PLAY);
    assert(requests == 0);
    press(BUTTON_PLAY | BUTTON_RC_LEFT);
    assert(requests > 0);
}
"""

with tempfile.TemporaryDirectory() as tmp:
    directory = Path(tmp)
    (directory / "probe.c").write_text(source)
    (directory / "probe_config.h").write_text("""
#define __CONFIG_H__
#define INIT_ATTR
#define HZ 100
#define HAVE_SW_POWEROFF
#define HAVE_MIKEY_REMOTE
#define HAVE_POWEROFF_WHILE_CHARGING
#define IPOD_ACCESSORY_PROTOCOL
""")
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-include", directory / "probe_config.h",
                    "-I", tmp, "-I", ROOT / "firmware/export",
                    "-I", ROOT / "firmware/target/arm/ipod",
                    "-I", ROOT / "firmware/target/arm/s5l8702/ipod6g",
                    directory / "probe.c", "-o", directory / "probe"],
                   check=True)
    subprocess.run([directory / "probe"], check=True, cwd=tmp)
