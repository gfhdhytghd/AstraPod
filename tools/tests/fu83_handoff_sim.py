#!/usr/bin/env python3
"""Measure the production sheet in an SDL build with a synthetic first-frame key.

Instrument a disposable source copy, build it as an ipod6g simulator, then run
BUILD BASE_ZIP OUT. The adapter has no connection to a USB or host service.
"""
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time
import wave
from zipfile import ZipFile

from fu77_lyrics_exit_sim import stop


def edit(source, path, old, new):
    file = source / path
    contents = file.read_text()
    assert contents.count(old) == 1, (path, old)
    file.write_text(contents.replace(old, new))


def instrument(source):
    app = 'apps-ipod/'
    sheet = app + 'screens/playback/handoff_sheet.c'
    edit(source, sheet, '#ifdef HAVE_USB_PODLINK', '#ifdef SIMULATOR')
    for path in ('input/action.c', 'input/action.h'):
        edit(source, app + path, '#ifdef HAVE_USB_PODLINK\nvoid action_handoff_enter',
             '#ifdef SIMULATOR\nvoid action_handoff_enter')
    for extension, next_line in (('c', 'bool skin_handoff_draw_blur'),
                                 ('h', '/* Draw the matching cached blur')):
        edit(source, app + 'skin/skin_albumart_color.' + extension,
             '#ifdef HAVE_USB_PODLINK\n' + next_line, '#ifdef SIMULATOR\n' + next_line)

    edit(source, app + 'audio/playback.c',
         '#ifdef HAVE_USB_PODLINK\n#include "lcd.h"',
         '#ifdef SIMULATOR\n#include "lcd.h"')
    edit(source, app + 'audio/playback.c',
         '        /* Opening the handoff must not shrink',
         '#endif\n#ifdef SIMULATOR\n        /* Opening the handoff must not shrink')

    edit(source, sheet, '        lcd_update_rect(0, top, LCD_WIDTH, LCD_HEIGHT - top);',
         '''    {
        lcd_update_rect(0, top, LCD_WIDTH, LCD_HEIGHT - top);
        extern void fu83_sim_frame(int, int);
        fu83_sim_frame(previous, offset);
    }''')
    edit(source, app + 'screens/playback/wps.c', '        button = skin_wait_for_action(WPS,',
         '''        extern void fu83_sim_sheet(void);
        fu83_sim_sheet();
        button = skin_wait_for_action(WPS,''')
    edit(source, app + 'root_menu.c', '    next_screen = root_menu_setup_screens();',
         """    next_screen = root_menu_setup_screens();
    if (playlist_create("/", NULL) >= 0 &&
        playlist_insert_track(NULL, "/FU83.wav", PLAYLIST_INSERT_LAST, false, true) >= 0)
    {
        playlist_start(0, 0, 0);
        next_screen = GO_TO_WPS;
    }""")
    edit(source, app + 'SOURCES', 'sim/shim-board.c\n', 'sim/shim-board.c\nsim/fu83_sheet.c\n')
    (source / app / 'sim/fu83_sheet.c').write_text(r'''
#include "config.h"
#include "audio.h"
#include "button.h"
#include "kernel.h"
#include "lcd.h"
#include "screens/playback/handoff_sheet.h"
#include "widgets/dialog.h"
#include <assert.h>
#include <stdio.h>

static unsigned trial, frame;
static long accepted, began;
static struct handoff_sheet view = { .title = "Synthetic track", .target = "Rufin" };

void fu83_sim_frame(int previous, int offset)
{
    fprintf(stderr, "FU83 FRAME %u %ld %d %d\n", trial, current_tick - began, previous, offset);
    if (frame++ == 0)
    {
        int key = trial == 1 ? BUTTON_SELECT : BUTTON_MENU;
        button_queue_post(key, 0);
        button_queue_post(key | BUTTON_REL, 0);
    }
}

static int update(int answer)
{
    if (answer == DIALOG_ACCEPT || answer == DIALOG_CANCEL)
        fprintf(stderr, "FU83 INPUT %u %u %d\n", trial, frame, answer);
    if (answer == DIALOG_CANCEL) return answer;
    if (answer == DIALOG_ACCEPT)
    {
        view.preparing = true;
        accepted = current_tick;
    }
    return accepted && current_tick - accepted >= HZ / 3 ?
        DIALOG_ACCEPT : DIALOG_CONTINUE;
}

void fu83_sim_sheet(void)
{
    static long playing_since;
    if (trial >= 2 || (audio_status() & (AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE))
                       != AUDIO_STATUS_PLAY) return;
    if (!playing_since) playing_since = current_tick;
    if (current_tick - playing_since < HZ) return;

    ++trial;
    frame = 0;
    accepted = 0;
    began = current_tick;
    view.preparing = false;
    fprintf(stderr, "FU83 BEGIN %u\n", trial);
    int result = handoff_sheet_run(&view, update);
    fprintf(stderr, "FU83 DONE %u %d\n", trial, result);
    assert(result == (trial == 1 ? DIALOG_ACCEPT : DIALOG_CANCEL));
    playing_since = current_tick;
}
''')


def capture(build, base, out):
    out.mkdir()
    root = Path(tempfile.mkdtemp(prefix='fu83-disk-', dir=build))
    with ZipFile(build / 'rockbox.zip') as bundle:
        bundle.extractall(root)
    with ZipFile(base) as bundle:
        for font in ('12-noto-sans.fnt', '18-noto-sans.fnt', '26-noto-sans-medium.fnt'):
            bundle.extract('.rockbox/fonts/' + font, root)
    (root / '.rockbox/config.cfg').write_text(
        'font: /.rockbox/fonts/18-noto-sans.fnt\nstart in screen: files\n'
        'show files: all\nbacklight timeout: off\n')
    with wave.open(str(root / 'FU83.wav'), 'wb') as audio:
        audio.setparams((1, 2, 22050, 0, 'NONE', 'not compressed'))
        audio.writeframes(bytes(22050 * 2 * 45))

    sim = server = None
    with (out / 'xvfb.log').open('w') as xlog, (out / 'simulator.log').open('w') as slog:
        try:
            read, write = os.pipe()
            server = subprocess.Popen(['Xvfb', '-displayfd', str(write), '-screen', '0',
                                       '800x600x24', '-nolisten', 'tcp'],
                                      pass_fds=(write,), stderr=xlog)
            os.close(write)
            with os.fdopen(read) as ready:
                display = ':' + ready.readline().strip()
            env = dict(os.environ, DISPLAY=display, SDL_VIDEODRIVER='x11',
                       SDL_AUDIODRIVER='dummy')
            sim = subprocess.Popen([str(build / 'rockboxui'), '--root', str(root),
                                    '--nobackground', '--zoom', '1'], cwd=build,
                                   env=env, stdout=slog, stderr=subprocess.STDOUT)
            for _ in range(250):
                if 'FU83 DONE 2 ' in (out / 'simulator.log').read_text(errors='replace'):
                    break
                assert sim.poll() is None, 'SDL simulator exited'
                time.sleep(.04)
            else:
                from PIL import ImageGrab
                ImageGrab.grab(xdisplay=display).crop((240, 180, 560, 420)).save(out / 'timeout.png')
                raise AssertionError('Handoff sheet did not complete both SDL trials')
        finally:
            stop(sim)
            stop(server)

    log = (out / 'simulator.log').read_text()
    counts = []
    for trial in (1, 2):
        updates = [tuple(map(int, row)) for row in re.findall(
            rf'FU83 FRAME {trial} (\d+) (\d+) (\d+)', log)]
        assert re.search(rf'FU83 INPUT {trial} 1 \d+', log), log
        enter = [row for row in updates if row[2] < row[1]]
        leave = [row for row in updates if row[2] > row[1]]
        assert len(enter) >= (6 if trial == 1 else 1), updates
        # A reversal can start near the bottom. Integer row rounding and
        # skipped ticks can reduce its number of distinct visible updates.
        assert len(leave) >= (5 if trial == 1 else 1) and leave[-1][2] == 240, updates
        assert enter[-1][2] == (0 if trial == 1 else leave[0][1]), updates
        assert leave[0][1] == enter[-1][2], updates
        counts.append(dict(trial=trial, enter_frames=len(enter),
                           exit_frames=len(leave), updates=updates))
    (out / 'frames.json').write_text(json.dumps(counts, indent=2) + '\n')
    print('FU83 SDL:', [(r['enter_frames'], r['exit_frames']) for r in counts])


if __name__ == '__main__':
    if sys.argv[1] == '--instrument':
        instrument(Path(sys.argv[2]).resolve())
    else:
        capture(*(Path(arg).resolve() for arg in sys.argv[1:]))
