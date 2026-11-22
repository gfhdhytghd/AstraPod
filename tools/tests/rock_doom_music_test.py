#!/usr/bin/env python3
"""Exercise the staged Doom synthesizer with an original MUS fixture."""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[2]
out = Path(sys.argv[1]).resolve()
out.mkdir(parents=True, exist_ok=True)
probe = out / "doom-music.c"
probe.write_text(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define SAMPLECOUNT 1024
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

static int16_t mixbuffer[2 * SAMPLECOUNT];
static unsigned short samplerate = 44100;
static bool stream_active = true;
static int snd_MusicVolume = 15;

#include "doom-music.inc"

static bool rendered(void)
{
    memset(mixbuffer, 0, sizeof(mixbuffer));
    mus_mix();
    bool nonzero = false;
    for (size_t i = 0; i < SAMPLECOUNT; i++)
    {
        assert(mixbuffer[2*i] == mixbuffer[2*i+1]);
        nonzero |= mixbuffer[2*i] != 0;
    }
    return nonzero;
}

int main(void)
{
    /* A4 at velocity 100 for 8 ticks, then release and end. */
    uint8_t fixture[] = {
        'M', 'U', 'S', 0x1a, 7, 0, 16, 0, 1, 0, 0, 0, 0, 0, 0, 0,
        0x90, 69 | 128, 100, 8, 0x00, 69, 0x60,
    };
    assert(!I_RegisterRockSong(fixture, sizeof(fixture) - 1));
    assert(I_RegisterRockSong(fixture, sizeof(fixture)));
    I_PlaySong(1, false);
    assert(rendered());
    I_PauseSong(1);
    const uint8_t *paused_at = score;
    assert(!rendered() && score == paused_at);
    I_ResumeSong(1);
    assert(rendered());
    assert(rendered() && !I_QrySongPlaying(1));
    assert(!rendered());
    I_PlaySong(1, true);
    for (int i = 0; i < 10; i++)
        assert(rendered());
    I_UnRegisterSong(1);
    assert(!I_QrySongPlaying(1) && !rendered());

    fixture[19] = 128; /* Truncated/invalid variable delay. */
    fixture[20] = 128;
    fixture[21] = 128;
    fixture[22] = 128;
    assert(I_RegisterRockSong(fixture, sizeof(fixture)));
    I_PlaySong(1, true);
    assert(!rendered() && !I_QrySongPlaying(1));
    I_ShutdownMusic();
}
''')
binary = out / "doom-music"
subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
                "-I", root / "apps-ipod/plugins", probe, "-o", binary], check=True)
subprocess.run([binary], check=True)
print("PASS: MUS PCM, pause/resume, endings, looping and malformed tracks")
