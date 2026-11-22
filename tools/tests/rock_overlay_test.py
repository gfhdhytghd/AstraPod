#!/usr/bin/env python3
"""Check foreground overlay request ownership, bounds and lifetime."""
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
out = Path(sys.argv[1]).resolve()
out.mkdir(parents=True, exist_ok=True)
(out / "plugins").mkdir(exist_ok=True)
(out / "kernel.h").write_text("unsigned int thread_self(void);\n")
(out / "plugins/rock_compat.h").write_text(
    "#include <stdbool.h>\nbool rock_usb_taken(void);\n")
(out / "autoconf.h").write_text(
    '#define ROCKBOX_DIR "/.rockbox"\n#define MEMORYSIZE 64\n')
(out / "test.c").write_text(r"""
#include <assert.h>
#include <string.h>
#include "plugins/rock_overlay.h"

static unsigned int owner = 1;
static bool usb;

unsigned int thread_self(void)
{
    return owner;
}

bool rock_usb_taken(void)
{
    return usb;
}

int main(void)
{
    struct rock_overlay_launch launch;
    char file[MAX_PATH];
    memset(file, 'f', sizeof(file));
    assert(rock_overlay_request("quake", NULL) < 0);
    rock_overlay_begin();
    assert(rock_overlay_request(NULL, NULL) < 0);
    assert(rock_overlay_request("", NULL) < 0);
    assert(rock_overlay_request("../quake", NULL) < 0);
    assert(rock_overlay_request("quake/extra", NULL) < 0);
    assert(rock_overlay_request("123456789012345678901234567890123", NULL) < 0);
    assert(rock_overlay_request("quake", file) < 0);
    owner = 2;
    assert(rock_overlay_request("quake", NULL) < 0);
    assert(!rock_overlay_take(&launch));
    owner = 1;
    usb = true;
    assert(rock_overlay_request("quake", NULL) < 0);
    usb = false;

    file[sizeof(file) - 1] = '\0';
    assert(rock_overlay_request("quake", file) == 0);
    assert(rock_overlay_request("wolf3d", NULL) < 0);
    file[0] = 'x';
    assert(rock_overlay_take(&launch));
    assert(strcmp(launch.path, "/.rockbox/rocks/overlays/quake.ovl") == 0);
    assert(launch.has_parameter && launch.parameter[0] == 'f');
    assert(strlen(launch.parameter) == sizeof(file) - 1);
    assert(!rock_overlay_take(&launch));
    assert(rock_overlay_request("quake", NULL) < 0);
    rock_overlay_end();

    rock_overlay_begin();
    assert(rock_overlay_request("wolf3d", "") == 0);
    assert(rock_overlay_take(&launch));
    assert(launch.has_parameter && *launch.parameter == '\0');
    rock_overlay_end();

    rock_overlay_begin();
    assert(rock_overlay_request("wolf3d", NULL) == 0);
    usb = true;
    assert(!rock_overlay_take(&launch));
    usb = false;
    rock_overlay_end();
    assert(!rock_overlay_take(&launch));
    rock_overlay_begin();
    assert(!rock_overlay_take(&launch));
    rock_overlay_end();

    rock_overlay_begin();
    assert(rock_overlay_request("12345678901234567890123456789012", NULL) == 0);
    assert(rock_overlay_take(&launch) && !launch.has_parameter);
    rock_overlay_end();
    return 0;
}
""")
exe = out / "rock_overlay_test"
subprocess.run(
    ["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
     "-I", str(out), "-I", str(ROOT / "apps-ipod"), "-idirafter",
     str(ROOT / "firmware/include"), "-idirafter", str(ROOT / "firmware/export"),
     str(out / "test.c"), str(ROOT / "apps-ipod/plugins/rock_overlay.c"),
     "-o", str(exe)], check=True,
)
subprocess.run([str(exe)], check=True)
print("PASS overlay names, copied parameters, ownership and one-way teardown")
