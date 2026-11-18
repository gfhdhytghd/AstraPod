#!/usr/bin/env python3
"""Capture FU83 and native handoff motion in disposable iPod 6G SDL builds.

Use --instrument SOURCE_COPY on a clean baseline and a candidate copy.
Build each as an iPod 6G simulator, including lib/rbcodec/codecs/wav.codec.
Run OLD_BUILD NEW_BUILD RESOURCE_ZIP EVIDENCE. No host or device participates.
"""
from pathlib import Path
import json
import os
import re
import subprocess
import sys
import tempfile
import time
import wave
from zipfile import ZipFile

from PIL import Image, ImageChops

from fu77_lyrics_exit_sim import stop
from fu83_handoff_sim import instrument as instrument_fu83

FPS = 60
SIZE = (320, 240)

ADAPTER = r'''
#include "config.h"
#include "SDL.h"
#include "audio.h"
#include "button.h"
#include "kernel.h"
#include "lcd.h"
#include "screens/playback/handoff_sheet.h"
#include "skin/skin_albumart_color.h"
#include "system.h"
#include "widgets/dialog.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#define FPS 60
extern SDL_Surface *lcd_surface;
static unsigned int trial, sample;
static uint32_t began, opened, closed;
static bool closing;
static struct handoff_sheet view = { .title = "Synthetic track", .target = "Rufin" };

static unsigned int frames_since(uint32_t origin)
{
    return (uint64_t)((uint32_t)USEC_TIMER - origin) * FPS / 1000000u;
}

static void capture(const char *label)
{
    char path[1024];
    int n = snprintf(path, sizeof path, "%s/%u-%s.bmp",
                     getenv("PODBOX_HANDOFF_CAPTURES"), trial, label);
    assert(n > 0 && (size_t)n < sizeof path);
    assert(SDL_SaveBMP(lcd_surface, path) == 0);
}

static void request_close(void)
{
    closing = true;
    button_queue_post(BUTTON_MENU, 0);
    button_queue_post(BUTTON_MENU | BUTTON_REL, 0);
}

void fu83_sim_frame(int previous, int offset)
{
    char label[24];
    snprintf(label, sizeof label, "frame-%03u", ++sample);
    capture(label);
    fprintf(stderr, "HANDOFF FRAME %u %u %u %u %d %d\n", trial, sample,
            frames_since(began), closed ? frames_since(closed) : 0,
            previous, offset);
    if (!offset && !opened)
    {
        opened = USEC_TIMER;
        capture("open");
    }
    if (trial == 2 && sample == 1)
        request_close();
}

static int update(int answer)
{
    if (answer == DIALOG_CANCEL)
    {
        closed = USEC_TIMER;
        fprintf(stderr, "HANDOFF INPUT %u %u\n", trial, sample);
        return answer;
    }
    if (trial == 1 && opened && !closing && frames_since(opened) >= 6)
        request_close();
    return DIALOG_CONTINUE;
}

void fu83_sim_sheet(void)
{
    static uint32_t playing_since;
    if (trial >= 2 || (audio_status() & (AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE))
                       != AUDIO_STATUS_PLAY)
        return;
    if (!playing_since)
        playing_since = USEC_TIMER;
    if (frames_since(playing_since) < FPS)
        return;

    ++trial;
    sample = opened = closed = 0;
    closing = false;
    /* Sync the visible LCD with the same finished WPS the sheet captures. */
    lcd_update();
    capture("before");
    fprintf(stderr, "HANDOFF ART %u %d\n", trial, skin_lyrics_art() >= 0);
    began = USEC_TIMER;
    assert(handoff_sheet_run(&view, update) == DIALOG_CANCEL);
    capture("after");
    fprintf(stderr, "HANDOFF DONE %u %u\n", trial, frames_since(closed));
    playing_since = USEC_TIMER;
}
'''


def instrument(source):
    instrument_fu83(source)
    (source / "apps-ipod/sim/fu83_sheet.c").write_text(ADAPTER)


def run(build, resources, mode, out):
    out.mkdir()
    with tempfile.TemporaryDirectory(prefix="handoff-sdl-") as temp, \
            (out / "xvfb.log").open("w") as xlog, (out / "sim.log").open("w") as slog:
        root = Path(temp)
        with ZipFile(resources) as archive:
            for entry in archive.namelist():
                if entry.startswith((".rockbox/fonts/", ".rockbox/themes/", ".rockbox/wps/",
                                     ".rockbox/icons/", ".rockbox/backdrops/")):
                    archive.extract(entry, root)
        rb = root / ".rockbox"
        (rb / "codecs").mkdir()
        (rb / "codecs/wav.codec").write_bytes((build / "lib/rbcodec/codecs/wav.codec").read_bytes())
        config = (rb / "themes/scrim.cfg").read_text()
        (rb / "config.cfg").write_text(config + "\nbacklight timeout: off\n"
                                       f"animations: {'on' if mode == 'on' else 'off'}\n")
        with wave.open(str(root / "FU83.wav"), "wb") as tone:
            tone.setparams((1, 2, 22050, 0, "NONE", "not compressed"))
            tone.writeframes(bytes(22050 * 2 * 45))
        cover = Image.linear_gradient("L").resize((128, 128))
        Image.merge("RGB", (cover, cover.transpose(Image.Transpose.TRANSPOSE), cover)).save(
            root / "cover.bmp")
        (root / "runtime").mkdir(mode=0o700)
        simulator = server = None
        try:
            reader, writer = os.pipe()
            server = subprocess.Popen(["Xvfb", "-displayfd", str(writer), "-screen", "0",
                                       "800x600x24", "-nolisten", "tcp"],
                                      pass_fds=(writer,), stderr=xlog)
            os.close(writer)
            with os.fdopen(reader) as ready:
                display = ":" + ready.readline().strip()
            env = dict(os.environ, DISPLAY=display, SDL_VIDEODRIVER="x11",
                       SDL_AUDIODRIVER="dummy", XDG_RUNTIME_DIR=str(root / "runtime"),
                       PODBOX_HANDOFF_CAPTURES=str(out))
            simulator = subprocess.Popen([str(build / "rockboxui"), "--root", str(root),
                                          "--nobackground", "--zoom", "1", "--screenshot"],
                                         cwd=root, env=env, stdout=slog, stderr=subprocess.STDOUT)
            for _ in range(FPS * 15):
                if "HANDOFF DONE 2 " in (out / "sim.log").read_text(errors="replace"):
                    break
                assert simulator.poll() is None, out / "sim.log"
                time.sleep(1 / FPS)
            else:
                raise AssertionError(f"Missing completed SDL trial. See {out / 'sim.log'}")
        finally:
            stop(simulator)
            stop(server)

    log = (out / "sim.log").read_text()
    metrics = {}
    for trial in (1, 2):
        before = read_image(out / f"{trial}-before.bmp")
        after = read_image(out / f"{trial}-after.bmp")
        assert not ImageChops.difference(before, after).getbbox(), (mode, trial, "restored")
        before.save(out / f"{trial}-before.png")
        after.save(out / f"{trial}-after.png")
        samples = [tuple(map(int, row)) for row in re.findall(
            rf"HANDOFF FRAME {trial} (\d+) (\d+) (\d+) (\d+) (\d+)", log)]
        open_frames = [offset for _, _, _, previous, offset in samples if offset < previous]
        close_frames = [offset for _, _, _, previous, offset in samples if offset > previous]
        assert open_frames and close_frames and close_frames[-1] == SIZE[1], (mode, trial, samples)
        assert re.search(rf"HANDOFF INPUT {trial} {1 if trial == 2 else r'[0-9]+'}\b", log), log
        for sample, _, _, _, offset in samples:
            frame = read_image(out / f"{trial}-frame-{sample:03d}.bmp")
            if offset:
                bounds = (0, 0, SIZE[0], offset)
                assert not ImageChops.difference(before.crop(bounds), frame.crop(bounds)).getbbox()
        if trial == 2:
            assert open_frames == [next(p for _, _, _, p, o in samples if o > p)], samples
        opening_endpoint = next((elapsed for _, elapsed, _, _, offset in samples if not offset), None)
        close_elapsed = int(re.search(rf"HANDOFF DONE {trial} (\d+)", log).group(1))
        art = re.search(rf"HANDOFF ART {trial} ([01])", log).group(1) == "1"
        assert art, "Synthetic cover art did not reach the real handoff painter"
        if trial == 1:
            read_image(out / f"{trial}-open.bmp").save(out / f"{trial}-open.png")
            if mode == "on":
                assert opening_endpoint >= 12, opening_endpoint
        metrics[trial] = {"cover_art": art,
                          "open_updates": len(open_frames), "close_updates": len(close_frames),
                          "opening_endpoint_f": opening_endpoint, "close_elapsed_f": close_elapsed,
                          "restored_pixels_changed": 0,
                          "frame_samples": samples}
    return metrics


def read_image(path):
    with Image.open(path) as bmp:
        assert bmp.size == SIZE, (path, bmp.size)
        return bmp.convert("RGB")


if __name__ == "__main__":
    if sys.argv[1] == "--instrument":
        instrument(Path(sys.argv[2]).resolve())
    else:
        old, new, resources, out = map(lambda s: Path(s).resolve(), sys.argv[1:])
        out.mkdir(parents=True)
        result = {mode: run(build, resources, mode, out / mode)
                  for mode, build in (("old", old), ("off", new), ("on", new))}
        baseline = read_image(out / "old/1-open.bmp")
        for mode in ("off", "on"):
            assert not ImageChops.difference(
                baseline, read_image(out / mode / "1-open.bmp")).getbbox(), mode
        (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
        print("PASS identical open handoff and restored background, with a first-frame reversal")
