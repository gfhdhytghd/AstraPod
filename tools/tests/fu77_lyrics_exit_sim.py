#!/usr/bin/env python3
"""Capture every LCD update during a lyrics exit on an instrumented SDL build.

Run --instrument on a disposable source copy before configuring the simulator.
Then run this script with BUILD BASE_ZIP OUT.
"""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import wave
from zipfile import ZipFile

from PIL import Image


def instrument(source):
    lcd = source / 'firmware/target/hosted/sdl/lcd-bitmap.c'
    code = lcd.read_text()
    code = code.replace('SDL_Surface* lcd_surface;', '''#include <stdio.h>
#include <stdlib.h>

SDL_Surface* lcd_surface;
static int fu77_stage, fu77_frame;

void fu77_exit_stage(int stage)
{
    fu77_stage = stage;
}

int fu77_no_arena(void)
{
    return getenv("FU77_NO_ARENA") != NULL;
}

static void fu77_capture(void)
{
    const char *dir = getenv("FU77_EXIT_CAPTURE");
    if (!dir || !fu77_stage) return;
    char path[1024];
    snprintf(path, sizeof path, "%s/frame-%03d-%d.bmp", dir, fu77_frame++, fu77_stage);
    SDL_SaveBMP(lcd_surface, path);
}
''')
    old = '                   background ? UI_LCD_POSX : 0, background? UI_LCD_POSY : 0);\n}'
    assert old in code
    code = code.replace(old, old[:-1] + '    fu77_capture();\n}', 1)
    lcd.write_text(code)
    viewer = source / 'apps-ipod/viewers/lyric_viewer/lyric_viewer.c'
    code = viewer.read_text()
    old = '    lv.claimed = arena != NULL;'
    assert old in code
    code = code.replace(old, '''#ifdef SIMULATOR
    extern int fu77_no_arena(void);
    if (arena && fu77_no_arena()) { app_release_buffer("lyrics"); arena = NULL; }
#endif
''' + old, 1)
    assert '\ndone:\n' in code
    viewer.write_text(code.replace('\ndone:\n', '''
done:
#ifdef SIMULATOR
    extern void fu77_exit_stage(int);
    fu77_exit_stage(1);
#endif
''', 1))
    wps = source / 'apps-ipod/screens/playback/wps.c'
    code = wps.read_text()
    old = '        button = skin_wait_for_action(WPS,'
    assert old in code
    code = code.replace(old, '''#ifdef SIMULATOR
        extern void fu77_exit_stage(int);
        fu77_exit_stage(0);
#endif
''' + old, 1)
    match = code.index('if (lyric_viewer() == GO_TO_ROOT)')
    after = code.index('\n            }', match) + len('\n            }')
    wps.write_text(code[:after] + '''
#ifdef SIMULATOR
            fu77_exit_stage(2);
#endif''' + code[after:])


def stop(child):
    if child is not None and child.poll() is None:
        child.terminate()
        try:
            child.wait(timeout=3)
        except subprocess.TimeoutExpired:
            child.kill()
            child.wait()


def capture(build, base, out):
    out.mkdir(parents=True, exist_ok=False)
    root = Path(tempfile.mkdtemp(prefix='exit-', dir=build))
    with ZipFile(build / 'rockbox.zip') as zipfile:
        zipfile.extractall(root)
    with ZipFile(base) as zipfile:
        for font in ('18-noto-sans.fnt', '26-noto-sans-medium.fnt', '12-noto-sans.fnt'):
            zipfile.extract('.rockbox/fonts/' + font, root)
    rockbox = root / '.rockbox'
    for name in ('config01.cfg', 'nvram.bin'):
        (rockbox / name).unlink(missing_ok=True)
    (rockbox / 'wps').mkdir(exist_ok=True)
    (rockbox / 'wps/fu77.wps').write_text('%we\n%V(0,36,320,204,-)%Vf(ff00ff)\n%acFU77 PLAYING\n%ac%it\n')
    (rockbox / 'wps/fu77.sbs').write_text('%Vi(ui,0,36,320,204,-)\n%V(0,0,320,36,-)%Vf(00ff00)\n%acFU77 STATUS\n')
    (rockbox / 'config.cfg').write_text(
        'font: /.rockbox/fonts/18-noto-sans.fnt\nwps: /.rockbox/wps/fu77.wps\n'
        'sbs: /.rockbox/wps/fu77.sbs\nbackground color: 080d17\n'
        'statusbar: top\nstart in screen: files\nshow files: all\nbacklight timeout: off\n' +
        ('lang: /.rockbox/langs/chinese-simp.lng\n' if os.environ.get('FU77_HANDOFF') else ''))
    with wave.open(str(root / 'Test.wav'), 'wb') as audio:
        audio.setparams((1, 2, 22050, 0, 'NONE', 'not compressed'))
        audio.writeframes(bytes(22050 * 2 * 120))
    (root / 'Test.lrc').write_text('[00:00.00]Lyrics exit test\n[00:30.00]Second line\n')
    xvfb = sim = None
    with (out / 'xvfb.log').open('w') as xvlog, (out / 'sim.log').open('w') as simlog:
        try:
            server, display = os.pipe()
            xvfb = subprocess.Popen(['Xvfb', '-displayfd', str(display), '-screen', '0',
                                     '800x600x24'], pass_fds=(display,), stderr=xvlog)
            os.close(display)
            with os.fdopen(server) as result:
                display_name = ':' + result.readline().strip()
            env = dict(os.environ, DISPLAY=display_name, SDL_VIDEODRIVER='x11',
                       SDL_AUDIODRIVER='dummy', FU77_EXIT_CAPTURE=str(out))
            sim = subprocess.Popen([str(build / 'rockboxui'), '--root', str(root),
                                    '--nobackground', '--zoom', '1', '--screenshot'],
                                   cwd=build, env=env, stdout=simlog, stderr=subprocess.STDOUT)
            for _ in range(100):
                search = subprocess.run(['xdotool', 'search', '--onlyvisible', '--name',
                                         'iPod 6G'], env=env, capture_output=True, text=True)
                if search.returncode == 0:
                    window = search.stdout.splitlines()[-1]
                    break
                assert sim.poll() is None, 'SDL simulator exited'
                time.sleep(.05)
            else:
                raise AssertionError('No SDL window')
            def key(name):
                subprocess.run(['xdotool', 'key', '--window', window, '--delay', '70', name],
                               env=env, check=True)

            from PIL import ImageGrab
            def screenshot(name):
                ImageGrab.grab(xdisplay=display_name).crop((240, 180, 560, 420)).save(out / name)

            time.sleep(2.5 if os.environ.get('FU77_HANDOFF') else 1.0)
            window = subprocess.check_output(['xdotool', 'search', '--onlyvisible', '--name',
                                              'iPod 6G'], env=env, text=True).splitlines()[-1]
            screenshot('files.png')
            key('Down')
            key('Down')
            key('Return')
            time.sleep(.8)
            screenshot('wps.png')
            if os.environ.get('FU77_HANDOFF'):
                key('Return')
                time.sleep(.4)
                screenshot('question-accept.png')
                key('Right')
                time.sleep(.25)
                screenshot('question-stay.png')
                key('Left')
                time.sleep(.2)
                key('Return')
                time.sleep(.35)
                screenshot('preparing.png')
                time.sleep(.95)
                screenshot('failure.png')
                return 0
            key('Return')
            time.sleep(.5)
            screenshot('lyrics.png')
            key('Return')
            time.sleep(.5)
            screenshot('returned.png')
            if os.environ.get('FU77_REPEAT'):
                for _ in range(12):
                    key('Return')
                    time.sleep(.27)
                    key('Return')
                    time.sleep(.25)
                key('u')
                time.sleep(1)
                screenshot('after-13-visits-usb.png')
                assert sim.poll() is None, 'SDL simulator did not survive virtual USB'
        finally:
            stop(sim)
            stop(xvfb)
    count = len(list(out.glob('frame-*.bmp')))
    if os.environ.get('FU77_REPEAT'):
        assert count >= 13, f'Expected frames from 13 exits, captured {count}'
    return count


def check(out, count):
    def inspect(path):
        frame = Image.open(path).convert('RGB')
        top = frame.crop((0, 0, 320, 36))
        body = frame.crop((0, 36, 320, 240))
        green = sum(g > 45 and g > 1.8 * (r + b) for r, g, b in top.get_flattened_data())
        magenta = sum(r > 45 and b > 45 and g < .5 * min(r, b)
                      for r, g, b in body.get_flattened_data())
        return {'status_pixels': green, 'wps_pixels': magenta,
                'body_colors': len(body.getcolors(65536) or [])}

    reference = inspect(out / 'wps.png')
    assert reference['status_pixels'] > 20 and reference['wps_pixels'] > 20, reference
    frames = {}
    for path in sorted(out.glob('frame-*.bmp'))[:count]:
        frames[path.with_suffix('.png').name] = inspect(path)
        Image.open(path).convert('RGB').save(path.with_suffix('.png'))
    (out / 'frames.json').write_text(json.dumps(dict(reference=reference, frames=frames),
                                                indent=2) + '\n')
    assert frames, 'No LCD updates captured during exit'
    assert all(row['status_pixels'] > 20 and row['wps_pixels'] > 20
               for row in frames.values()), 'Exit presented a frame without the WPS or status bar'
    print(f'PASS {len(frames)} LCD updates, all with the WPS and status bar')


if __name__ == '__main__':
    if sys.argv[1] == '--instrument':
        instrument(Path(sys.argv[2]))
    else:
        build, base, out = map(lambda value: Path(value).resolve(), sys.argv[1:])
        count = capture(build, base, out)
        if not os.environ.get('FU77_HANDOFF'):
            check(out, count)
