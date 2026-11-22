#!/usr/bin/env python3
"""Make a redistributable Baseline fixture and exercise video lifecycle."""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[2]
out = Path(sys.argv[1]).resolve()
out.mkdir(parents=True, exist_ok=True)
stubs = out / "headers"
stubs.mkdir(exist_ok=True)
for name in ("system.h", "file.h", "backlight.h", "button.h", "crc32.h",
             "dir.h", "font.h", "lang.h", "lcd.h", "menu.h", "misc.h",
             "rbpaths.h", "settings.h", "sound.h", "splash.h", "config.h"):
    (stubs / name).write_text("")
(stubs / "kernel.h").write_text("#include <stdbool.h>\nunsigned int thread_self(void);\n")
(stubs / "rbendian.h").write_text("#include <endian.h>\n#define betoh32 be32toh\n")
source = root / "apps/plugins/h264_player"
fixture = root / "tools/tests/fixtures/rock-h264"
staged = out / "video_playback.c"
subprocess.run([sys.executable, root / "apps-ipod/plugins/stage-h264.py",
                source / "video_playback.c", staged], check=True)
video = out / "astrapod-baseline-l30.mp4"
subprocess.run([
    "ffmpeg", "-loglevel", "error", "-y", "-f", "lavfi",
    "-i", "testsrc2=size=320x240:rate=24", "-t", "14", "-an",
    "-pix_fmt", "yuv420p", "-c:v", "libx264", "-profile:v", "baseline",
    "-level:v", "3.0", "-refs", "1", "-x264-params",
    "slices=1:keyint=48:min-keyint=48:scenecut=0", "-movflags", "+faststart",
    video,
], check=True)
broken = out / "truncated.mp4"
broken.write_bytes(video.read_bytes()[:31])
binary = out / "test"
subprocess.run([
    "cc", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
    "-fsanitize=address,undefined", "-DHAVE_HW_H264",
    "-I", stubs, "-I", fixture, "-I", source,
    "-I", root / "apps-ipod", "-I", root / "firmware/export",
    fixture / "probe.c", staged, source / "mp4_demux.c",
    root / "apps-ipod/plugins/rock_h264.c", "-o", binary,
], check=True)
subprocess.run([binary, video, broken], check=True)
