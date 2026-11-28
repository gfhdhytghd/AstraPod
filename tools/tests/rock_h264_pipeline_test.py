#!/usr/bin/env python3
"""Run the staged player with cooperative virtual time, cold reads and stalls."""
from pathlib import Path
import re
import subprocess as sp
import sys

root = Path(__file__).resolve().parents[2]
out = Path(sys.argv[1]).resolve()
out.mkdir(parents=True, exist_ok=True)
source = root / "apps/plugins/h264_player"
fixture = root / "tools/tests/fixtures/rock-h264"
headers = out / "headers"
headers.mkdir(exist_ok=True)
for name in ("system.h", "file.h", "rbpaths.h", "config.h", "lcd.h"):
    (headers / name).write_text("")
(headers / "kernel.h").write_text(
    "unsigned int thread_self(void);\nextern volatile long current_tick;\n#define HZ 100\n")
(headers / "rbendian.h").write_text("#include <endian.h>\n#define betoh32 be32toh\n")
staged = out / "video_playback.c"
sp.run([sys.executable, root / "apps-ipod/plugins/stage-h264.py",
        source / "video_playback.c", staged], check=True)


def splice(text, marker, replacement):
    assert text.count(marker) == 1, marker
    return text.replace(marker, replacement)


marker = "                    displayed_ms = pic->pts_ms;"
staged.write_text(splice(staged.read_text(), marker, r"""
                    rb->fdprintf(1, "frame %u %u %u\n",
                                pic->sample, pic->pts_ms, clock_ms);
""" + marker))

harness = (fixture / "probe.c").read_text().split("static int picture_mode(", 1)[0]
harness = "#include <ucontext.h>\n" + harness
harness = splice(harness, "scratch[2 * 1024 * 1024]", "scratch[4 * 1024 * 1024]")
harness = splice(harness, "static int configure(", """
static void wait_ticks(long ticks);
static unsigned int profile;
static FILE *packets;

static void annexb(const uint8_t *data, size_t size)
{
    assert(fwrite("\\0\\0\\0\\1", 1, 4, packets) == 4);
    assert(fwrite(data, 1, size, packets) == size);
}

static int configure(""")
harness = splice(harness, "    configured++;", """
    if (packets)
    {
        const uint8_t *p = avcc + 6;
        unsigned count = avcc[5] & 31;
        for (unsigned group = 0; group < 2; ++group)
        {
            if (group)
                count = *p++;
            for (unsigned i = 0; i < count; ++i)
            {
                unsigned length = p[0] * 256 + p[1];
                p += 2;
                assert(p + length <= avcc + size);
                annexb(p, length);
                p += length;
            }
        }
    }
    configured++;""")
harness = splice(harness, "    decoded++;", """
    ++decoded;
    if (profile != 2)
        wait_ticks(profile == 1 && decoded % 400 == 0 ? 33 :
                   decoded % 73 == 0 ? 6 : decoded % 4 == 0 ? 5 : 2);
    if (packets)
    {
        int pos = 0;
        while (pos < size)
        {
            uint32_t length = 0;
            for (int k = 0; k < nalu_len_size; ++k)
                length = length * 256 + sample[pos++];
            assert(length && length <= (uint32_t)(size - pos));
            annexb(sample + pos, length);
            pos += length;
        }
    }""")
harness = splice(harness, "    assert(w == 320 && h == 240);",
                 "    assert((w == 320 && h == 240) || (w == 640 && h == 368));")
harness = splice(harness, "static void wait_ticks(long ticks) { current_tick += ticks; }",
r"""
static ucontext_t foreground, worker_ctx;
static unsigned char host_stack[65536];
static void (*worker_run)(void *);
static void *worker_arg;
static bool worker_live, worker_stopping;
static long wake;
static unsigned int reads;

static void worker(void)
{
    worker_run(worker_arg);
    worker_live = false;
}

static unsigned int start_worker(void (*run)(void *), void *arg,
                                 void *stack, size_t bytes)
{
    assert(!worker_live && stack && bytes >= 8192);
    assert(getcontext(&worker_ctx) == 0);
    worker_ctx.uc_stack.ss_sp = host_stack;
    worker_ctx.uc_stack.ss_size = sizeof(host_stack);
    worker_ctx.uc_link = &foreground;
    makecontext(&worker_ctx, worker, 0);
    worker_live = true;
    wake = current_tick;
    worker_run = run;
    worker_arg = arg;
    return test_worker_id = 2;
}

static void wait_ticks(long ticks)
{
    if (thread_id == test_worker_id)
    {
        wake = current_tick + ticks;
        assert(swapcontext(&worker_ctx, &foreground) == 0);
    }
    else
    {
        for (long i = 0; i < ticks; ++i)
        {
            ++current_tick;
            /* A runnable worker can run again in the same tick after a
             * voluntary yield. Full and paused queues sleep one tick. */
            while (worker_live && wake <= current_tick)
            {
                thread_id = test_worker_id;
                assert(swapcontext(&foreground, &worker_ctx) == 0);
                thread_id = 1;
            }
        }
    }
}

static bool join_worker(unsigned int id)
{
    assert(id == test_worker_id);
    while (worker_live)
        wait_ticks(1);
    test_worker_id = 0;
    return true;
}

static bool stopping(void) { return worker_stopping; }
static void worker_yield(void) { wait_ticks(0); }

static ssize_t cold_read(int fd, void *buffer, size_t size)
{
    if (thread_id == test_worker_id && size >= 64 * 1024 &&
        ++reads % 7 == 1 && profile == 1)
        wait_ticks(8);
    return read(fd, buffer, size);
}
""")
harness += r"""
int main(int argc, char **argv)
{
    assert(argc == 3);
    reset(FINISH);
    profile = argv[2][0] - '0';
    if (profile == 2)
        packets = fopen("dispatch.264", "wb");
    struct plugin_api test_api = api;
    test_api.rock_worker_start = start_worker;
    test_api.rock_worker_stopping = stopping;
    test_api.rock_worker_join = join_worker;
    test_api.yield = worker_yield;
    test_api.read = cold_read;
    rb = &test_api;

    size_t needed = video_h264_scratch_size(argv[1]);
    assert(needed && needed <= sizeof(scratch));
    const struct video_play_options options = {.resume_menu = resume_menu};
    assert(video_h264_play_named(argv[1], scratch, needed, &options) ==
           VIDEO_FINISHED);
    assert(!active && !open_files && !boosted && !worker_live);
    if (packets)
        fclose(packets);
    printf("counts %u %u %u %u\n", decoded, drawn, opens, reads);
    return 0;
}
"""
probe = out / "pipeline.c"
probe.write_text(harness)
binary = out / "pipeline"
sp.run(["cc", "-std=gnu11", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=undefined", "-DHAVE_HW_H264", "-I", headers, "-I", fixture,
        "-I", source, "-I", root / "apps-ipod", "-I", root / "firmware/export",
        "-I", root / "firmware/target/arm/s5l8702/ipod6g",
        probe, staged, source / "mp4_demux.c", root / "apps-ipod/plugins/rock_h264.c",
        "-o", binary], check=True)


def run(video, profile):
    trace = sp.run([binary, video, str(profile)], cwd=out, text=True,
                   stdout=sp.PIPE, check=True).stdout
    (out / f"trace-{profile}.txt").write_text(trace)
    frames = [tuple(map(int, line.split()[1:]))
              for line in trace.splitlines() if line.startswith("frame ")]
    counts = tuple(map(int, re.search(r"counts (\d+) (\d+) (\d+) (\d+)",
                                     trace).groups()))
    log = (out / "video-sync.log").read_text()
    (out / f"sync-{profile}.log").write_text(log)
    return frames, counts, log


long_clip = out / "fractional-900s.mp4"
if len(sys.argv) > 2:
    long_clip = Path(sys.argv[2]).resolve()
elif not long_clip.exists():
    sp.run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi",
            "-i", "color=blue:size=320x240:rate=24000/1001:duration=900",
            "-f", "lavfi", "-i", "anullsrc=channel_layout=mono:sample_rate=44100",
            "-c:v", "libx264", "-preset", "ultrafast", "-threads", "2",
            "-profile:v", "baseline", "-level:v", "3.0", "-g", "12",
            "-c:a", "aac", "-shortest", long_clip], check=True)

for profile in (0, 1):
    frames, (decoded, drawn, opens, reads), log = run(long_clip, profile)
    offsets = sorted(clock - pts for _, pts, clock in frames)
    gaps = [b[2] - a[2] for a, b in zip(frames, frames[1:])]
    assert all(pts == n * 1001 * 1000 // 24000 for n, pts, _ in frames)
    skipped = sum(map(int, re.findall(r"skipped_to_idr=(\d+)", log)))
    assert decoded + skipped == 21579 and opens == 1 and reads < decoded // 10
    assert bool(skipped) == bool(profile)
    assert frames[-1][1] >= 899_000 and len(frames) == drawn
    assert 0 <= offsets[0] <= offsets[-1] <= 33
    assert len(frames) > (21400 if profile == 0 else 21000), (profile, len(frames))
    assert max(gaps) < (140 if profile == 0 else 500), (profile, max(gaps))
    assert "peak=3" in log and 89 <= log.count("media_ms=") <= 91
    print(f"profile={profile} decoded={decoded} shown={drawn} idr_skip={skipped} media_reads={reads} "
          f"p95={offsets[len(offsets)*95//100]}ms max={offsets[-1]}ms "
          f"max_display_gap={max(gaps)}ms last_pts={frames[-1][1]}ms")

clip = out / "three-refs-edit.mp4"
sp.run(["ffmpeg", "-v", "error", "-y", "-itsoffset", "0.25", "-f", "lavfi",
        "-i", "testsrc2=size=640x360:rate=24000/1001:duration=4",
        "-f", "lavfi", "-i", "sine=sample_rate=44100:duration=5",
        "-c:v", "libx264", "-threads", "2", "-profile:v", "baseline",
        "-level:v", "3.0", "-refs:v", "3", "-bf", "0", "-g", "48",
        "-c:a", "aac", "-movflags", "+faststart", clip], check=True)
frames, (decoded, drawn, opens, reads), _ = run(clip, 2)
assert decoded == 96 and drawn == 96 and opens == 1
assert frames[0][1] == 250 and frames[-1][1] == 250 + 95 * 1001 * 1000 // 24000
def checksum(args):
    result = sp.check_output(["ffmpeg", "-v", "error", *args,
                              "-map", "0:v:0", "-f", "framemd5", "-"], text=True)
    return [line.split(",")[-1].strip() for line in result.splitlines()
            if not line.startswith("#")]

reference = checksum(["-ignore_editlist", "1", "-i", str(clip)])
dispatch = checksum(["-i", str(out / "dispatch.264")])
assert len(dispatch) == 96 and dispatch == reference
print(f"edit first_pts={frames[0][1]}ms last_pts={frames[-1][1]}ms "
      f"ref3 visible=640x360 frames={len(dispatch)} reference_md5=match")
