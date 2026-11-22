#!/usr/bin/env python3
"""Replay 44.1 kHz mono recordings through the staged tuner in DMA blocks."""
from array import array
from pathlib import Path
import subprocess
import sys
import wave

root = Path(__file__).resolve().parents[2]
out = Path(sys.argv[1]).resolve()
out.mkdir(parents=True, exist_ok=True)
staged = out / "pitch_detector.c"
subprocess.run([sys.executable, root / "apps-ipod/plugins/stage-pitch.py",
                root / "apps/plugins/pitch_detector.c", staged], check=True)
text = staged.read_text()
header = (root / "apps-ipod/plugins/rock_pitch.h").read_text()
capture = (root / "apps-ipod/plugins/rock_pitch.c").read_text()


def between(source, first, last):
    start = source.index(first)
    return source[start:source.index(last, start)]


source = out / "pitch-replay.c"
source.write_text("""
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define IDATA_ATTR
#define ICODE_ATTR
""" + between(header, "#define ROCK_PITCH_RATE", "enum rock_pitch_result")
    + between(text, "typedef int32_t fixed;", "/* The recording buffer size */")
    + between(text, "#define BUFFER_SIZE", "#define LCD_FACTOR")
    + between(text, "#define DEFAULT_YIN_THRESHOLD", "\n") + "\n"
    + between(text, "static const fixed yin_threshold_table", "#endif")
    + between(text, "#define VOLUME_THRESHOLD", "\n") + "\n"
    + """
static const int sample_rate = ROCK_PITCH_RATE;
static struct {
    int volume_threshold;
    int sample_size;
    int yin_threshold;
} settings = {VOLUME_THRESHOLD, BUFFER_SIZE, DEFAULT_YIN_THRESHOLD};
static int16_t samples[BUFFER_SIZE];
static fixed yin_buffer[YIN_BUFFER_SIZE];
""" + between(capture, "static void pitch_copy_block", "static enum pcm_dma_status")
    + between(text, "/* Scale the sum", "\n#endif\n\nstatic void display_capture_start")
    + """
int main(int argc, char **argv)
{
    int16_t mono[ROCK_PITCH_CAPTURE_FRAMES];
    int16_t raw[2 * ROCK_PITCH_CAPTURE_FRAMES];
    if (argc == 2)
        settings.volume_threshold = atoi(argv[1]);
    while (fread(mono, sizeof(mono), 1, stdin) == 1)
    {
        /* The native mic recorder saves the left ADC. Keep distinct right
         * samples so a channel or stride error cannot pass as valid PCM. */
        for (size_t i = 0; i < ROCK_PITCH_CAPTURE_FRAMES; ++i)
        {
            raw[2 * i] = mono[i];
            raw[2 * i + 1] = i % 2 ? 500 : -3000;
        }
        pitch_copy_block(samples, raw);
        printf("%.4f\\n", fixed2float(detect_frequency()));
    }
    return ferror(stdin) ? 1 : 0;
}
""")
probe = out / "pitch-replay"
subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
                "-fsanitize=undefined", "-fno-sanitize-recover=undefined",
                source, "-o", probe], check=True)


def replay(path, threshold=None):
    with wave.open(str(path)) as recording:
        assert (recording.getnchannels(), recording.getsampwidth(),
                recording.getframerate()) == (1, 2, 44100)
        pcm = array("h", recording.readframes(recording.getnframes()))
    if sys.byteorder != "little":
        pcm.byteswap()
    frames = 4096
    # The live detector waits for a complete block. Omit any trailing partial.
    args = [probe] if threshold is None else [probe, str(threshold)]
    result = subprocess.run(args, input=pcm.tobytes(), capture_output=True,
                            check=True)
    pitches = [float(line) for line in result.stdout.splitlines()]
    assert len(pitches) == len(pcm) // frames
    return pitches


if len(sys.argv) > 2:
    for path in sys.argv[2:]:
        for threshold in (None, 50, 15, 5):
            print(f"{path} threshold={threshold or 'default'}")
            for i, hz in enumerate(replay(path, threshold)):
                print(f"{i * 4096 / 44100:.3f} {hz:.4f}")
else:
    fixtures = root / "tools/tests/fixtures/rock-pitch"
    recordings = sorted(fixtures.glob("*.wav"))
    assert recordings
    for path in recordings:
        pitches = replay(path)
        assert pitches
        if path.stem.endswith("-tone"):
            assert all(437 <= hz <= 443 for hz in pitches), (path.name, pitches)
        else:
            assert all(hz == 0 for hz in pitches), (path.name, pitches)
        print(f"PASS: {path.name}, {len(pitches)} blocks, "
              f"{min(pitches):.2f} to {max(pitches):.2f} Hz")
