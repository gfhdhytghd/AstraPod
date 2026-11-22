#!/usr/bin/env python3
"""Check packed keymap bounds under ASan and UBSan."""
import os
from pathlib import Path
import subprocess
from tempfile import TemporaryDirectory


ROOT = Path(__file__).resolve().parents[2]
ACTION_HEADER = """
#include <stdbool.h>
#define CONTEXT_STOPSEARCHING 0xFFFFFFFF
#define CONTEXT_REMAPPED 0x08000000
#define BUTTON_NONE 0
#define ACTION_NONE 0
#define LAST_ACTION_PLACEHOLDER 160
struct button_mapping { int action_code; int button_code; int pre_button_code; };
"""
PROBE = """
#include <assert.h>
#include <limits.h>
#include "rock_keymap_format.h"

int main(void)
{
    const struct button_mapping stop = { -1, 0, 0 };
    struct button_mapping map[] = {
        { CONTEXT_REMAPPED, 3, 1 },
        { CONTEXT_REMAPPED | 1, 5, 1 },
        { -1, 0, 0 },
        { 3, 4, 0 },
        { -1, 0, 0 },
        { 7, 8, 4 },
        { -1, 0, 0 },
    };
    assert(rock_keymap_valid(&stop, 1));
    assert(rock_keymap_valid(map, 7));
    for (int count = 0; count < 7; ++count)
        assert(!rock_keymap_valid(map, count));
    assert(!rock_keymap_valid(map, ROCK_KEYMAP_MAX_ENTRIES + 1));
    map[1].pre_button_code = 2; /* Sentinel would equal count. */
    assert(!rock_keymap_valid(map, 7));
    map[1].pre_button_code = INT_MAX;
    assert(!rock_keymap_valid(map, 7));
    map[1].pre_button_code = -1;
    assert(!rock_keymap_valid(map, 7));
    map[1].pre_button_code = 1;
    map[1].button_code = 3; /* Groups cannot share storage. */
    assert(!rock_keymap_valid(map, 7));
    map[1].button_code = 5;
    map[5].action_code = -1;
    assert(!rock_keymap_valid(map, 7));
    map[5].action_code = 7;
    map[5].button_code = 0;
    assert(!rock_keymap_valid(map, 7));
    map[5].button_code = 8;
    map[6].pre_button_code = 1;
    assert(!rock_keymap_valid(map, 7));
    return 0;
}
"""


def main():
    with TemporaryDirectory() as tmp:
        directory = Path(tmp)
        (directory / "action.h").write_text(ACTION_HEADER)
        (directory / "test.c").write_text(PROBE)
        binary = directory / "test"
        subprocess.run(
            [
                os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra",
                "-Werror", "-fsanitize=address,undefined", "-no-pie",
                "-I", str(directory), "-I", str(ROOT / "apps-ipod/plugins"),
                str(directory / "test.c"), "-o", str(binary),
            ],
            check=True,
        )
        subprocess.run([binary], check=True)
    print("PASS packed keymap extent, group ordering and sentinel checks")


if __name__ == "__main__":
    main()
