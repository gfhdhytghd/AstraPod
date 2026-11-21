"""Run the firmware's shell, key and LCD paths with RAM-backed hardware."""
from pathlib import Path
import re
import subprocess
import sys

fixture = Path(__file__).resolve().with_suffix(".c")
root = fixture.parents[2]
out = Path(sys.argv[1]).resolve()
out.mkdir(parents=True, exist_ok=True)


def function(source, name):
    match = re.search(r"(?m)^[\w *]+\b" + name + r"\([^;{}]*\)\s*\{", source)
    assert match, name
    end, depth = match.end(), 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


def without_headers(path):
    return re.sub(r'^#include "(?!rock-compat/).*"\n', "", path.read_text(), flags=re.M)


app = root / "apps-ipod/plugins"
lcd = (root / "firmware/target/arm/s5l8702/lcd-s5l8702.c").read_text()
usb = (root / "firmware/usbstack/usb_core.c").read_text()
(out / "production.inc").write_text("\n".join([
    *(without_headers(app / ("podlink_plugin_test" + suffix + ".c"))
      for suffix in ("_keys", "_lcd", "")),
    without_headers(root / "firmware/drivers/button_usb_remote.c"),
    *(function(lcd, name) for name in (
        "lcd_update_rect", "lcd_present_scene", "lcd_blit_yuv",
        "lcd_copy_presented_for_test",
    )),
    function(usb, "usb_core_handle_transfer_completion"),
]))
(out / "rock-compat").mkdir(exist_ok=True)
modules = (app / "rock-modules.txt").read_text().splitlines()
(out / "rock-compat/rock_test_paths.h").write_text(
    "".join(f'PLUGIN_DIR "/{module}.rock",\n' for module in modules)
)
binary = out / "podlink_plugin_test"
subprocess.run([
    "cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-g",
    "-fsanitize=address,undefined", "-fno-pie", "-no-pie", "-I", str(out),
    "-I", str(root / "firmware/export"),
    "-I", str(root / "firmware/target/arm/s5l8702/ipod6g"),
    str(fixture), "-o", str(binary),
], check=True)
subprocess.run([str(binary)], check=True, timeout=10)
