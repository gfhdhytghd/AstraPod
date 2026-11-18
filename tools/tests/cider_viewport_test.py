"""Verify that local and remote WPS share the native painter and viewport."""
from pathlib import Path

root = Path(__file__).resolve().parents[2]
player = root / "apps-ipod/screens/playback"
wps = (player / "wps.c").read_text()
native = (root / "apps-ipod/ui/native_wps.c").read_text()

assert not (player / "cider_screen.c").exists()
assert "cider_wps_action(button)" in wps
assert "native_wps_render(state, submit)" in wps
assert "paint_viewport(" in native
assert "cider_active()" in native
assert not (root / "apps-ipod/skin/skin_parser.c").exists()
for file in ("cider_browser.c", "cider_wps.c"):
    assert "viewport_set_fullscreen" not in (player / file).read_text()
print("PASS local and remote WPS share the native painter and viewport")
