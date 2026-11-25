#!/usr/bin/env python3
"""Check the production refresh clock and cubic curves in a host build."""
import ctypes as c
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]


class Tween(c.Structure):
    _fields_ = [
        ("start_clock", c.c_uint32),
        ("from_", c.c_int),
        ("to", c.c_int),
        ("frames", c.c_uint),
        ("ease_in", c.c_bool),
    ]


with tempfile.TemporaryDirectory(prefix="podbox-ui-test-") as temp:
    lib = Path(temp) / "animation.so"
    subprocess.run(
        ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC",
         str(root / "apps-ipod/ui/animation.c"),
         str(root / "tools/tests/fixtures/ui_refresh_stub.c"), "-o", str(lib)],
        check=True,
    )
    animation = c.CDLL(str(lib))
    animation.ui_frame_clock.argtypes = [c.c_uint]
    animation.ui_frame_clock.restype = c.c_uint32
    animation.ui_clock_frame.argtypes = [c.c_uint32]
    animation.ui_clock_frame.restype = c.c_uint
    animation.ui_tween_sample.argtypes = [c.POINTER(Tween), c.c_uint]
    animation.ui_tween_sample.restype = c.c_int

    # One frame per refresh of the nominal 66.96 Hz panel, 14934.29 us.
    for f in range(1, 601):
        clock = animation.ui_frame_clock(f)
        assert clock == -(-f * 3823178 // 256)
        assert animation.ui_clock_frame(clock - 1) == f - 1
        assert animation.ui_clock_frame(clock) == f
    assert animation.ui_frame_clock(1) == 14935
    assert animation.ui_clock_frame(1000000) == 66
    # Authored 60 fps durations play about 11% faster.
    assert 0.89 < animation.ui_frame_clock(60) / 1e6 < 0.90

    for start, end, frames, in_curve in [(320, 0, 12, False), (0, 320, 10, True)]:
        tween = Tween(0, start, end, frames, in_curve)
        steps = [animation.ui_tween_sample(tween, f) for f in range(frames + 1)]
        assert steps[0] == start
        assert steps[-1] == animation.ui_tween_sample(tween, frames + 30) == end
        assert all((b - a) * (end - start) >= 0 for a, b in zip(steps, steps[1:]))
        if in_curve:
            assert steps[1] - steps[0] < steps[-1] - steps[-2]
        else:
            assert steps[0] - steps[1] > steps[-2] - steps[-1]

    push = Tween(0, 320, 0, 12, False)
    offset = animation.ui_tween_sample(push, 3)
    reverse = Tween(0, offset, 320, 10, True)
    assert animation.ui_tween_sample(reverse, 0) == offset
    assert animation.ui_tween_sample(reverse, 10) == 320
    wrapped_start = 0xffffffff - animation.ui_frame_clock(3)
    wrapped_end = c.c_uint32(wrapped_start + animation.ui_frame_clock(12)).value
    assert animation.ui_clock_frame(c.c_uint32(wrapped_end - wrapped_start).value) == 12

print("PASS refresh-period boundaries, wraparound, 12 f / 10 f, skipped and reversed frames")
