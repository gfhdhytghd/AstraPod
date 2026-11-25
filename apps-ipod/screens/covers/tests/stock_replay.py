#!/usr/bin/env python3
"""python stock_replay.py STOCKFW_DIR OUTPUT_DIR

Build the production geometry with UBSan and replay the seven RGB565 captures.
Requires the configured build-sim-ipod6g, numpy, Pillow and the stock fixture.
Images and measured CSVs go into OUTPUT_DIR, outside the source tree.
"""
import csv
import math
import runpy
import shlex
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw


def read_csv(path):
    with path.open() as src:
        return list(csv.DictReader(src))


def population(errors):
    values = sorted(errors)
    return (values[len(values) // 2], values[len(values) * 9 // 10], values[-1])


def reference(fixture, run):
    args = sys.argv
    try:
        sys.argv = [str(fixture / 'tools/stock_model.py'), str(run), '34', '1']
        return runpy.run_path(sys.argv[0])
    finally:
        sys.argv = args


def commands(run, frames):
    # The capture clock timestamps input injection. The measured firmware UI
    # latency and global driver's dead band apply only to this capture replay.
    events = []
    threshold = held = remainder = 0
    for row in read_csv(run / 'input.csv'):
        if row['op'] == 'touch':
            threshold, held = 6, 0
        elif row['op'] == 'step':
            delta = int(row['arg'])
            if abs(delta) < threshold:
                held += delta
                if abs(held) >= 4:
                    threshold = held = 0
            elif abs(delta) < 36:
                remainder += delta
                steps = math.trunc(remainder / 4)
                remainder -= 4 * steps
                if steps:
                    events.append((round(float(row['t_rel_ms']) * 1000) + 34000,
                                   0, 's', steps))
    arrivals = [e[0] for e in events]
    times = [round(1000 * float(f['t_rel_ms'])) for f in frames]
    first, last = arrivals[0], arrivals[-1]
    # Show a settled start/end, a handoff and sustained motion (or reversal).
    targets = [times[0], first + 120000, (first + last) // 2 + 100000,
               last + 75000, times[-1]]
    featured = sorted({min(times, key=lambda t: abs(t - target)) for target in targets})
    frame_num = 0
    while True:
        t = round(frame_num * 1000000 / 66.96)
        if t > times[-1]:
            break
        events.append((t, 1, 'h' if frame_num % 2 else 'b', 0))
        frame_num += 1
    for t in times:
        events.append((t, 2, 'r' if t in featured else 'f', 0))
    return ''.join(f'{op} {t} {arg}\n' for t, _, op, arg in sorted(events)), featured


def sheet(run, work, frames, featured):
    files = {round(1000 * float(f['t_rel_ms'])): f['file'] for f in frames}
    sheet = Image.new('RGB', (640, 264 * len(featured)), 'white')
    draw = ImageDraw.Draw(sheet)
    for i, t in enumerate(featured):
        src = np.fromfile(run / 'frames' / files[t], '<u2').reshape(240, 320).astype('uint32')
        rgb = np.stack([((src >> 11) & 31) * 255 // 31,
                        ((src >> 5) & 63) * 255 // 63, (src & 31) * 255 // 31], -1)
        y = 264 * i
        draw.text((5, y + 4), f'{run.name}   stock   {t/1000:.3f} ms', fill='black')
        draw.text((325, y + 4), 'AstraPod C renderer (simulator build)', fill='black')
        sheet.paste(Image.fromarray(rgb.astype('uint8')), (0, y + 24))
        sheet.paste(Image.open(work / f'render-{t}.ppm'), (320, y + 24))
    sheet.save(work / 'comparison.png')


def replay(fixture, out, scenario):
    run = fixture / 'emu' / 'runs' / scenario
    work = out / scenario
    work.mkdir(exist_ok=True)
    # Run the supplied tracker against the original pixels. Keep its outputs
    # separate from the capture, which remains a read-only fixture.
    for name in ('input.csv', 'frames'):
        if not (work / name).exists():
            (work / name).symlink_to(run / name)
    subprocess.run([sys.executable, fixture / 'emu/analyze.py', work], check=True)
    model = reference(fixture, work)
    frames = read_csv(run / 'frames/frames.csv')
    stream, featured = commands(run, frames)
    proc = subprocess.run([out / 'stock_replay', out / 'art.rgb'], input=stream,
                          cwd=work, text=True, stdout=subprocess.PIPE, check=True)
    observed = {}
    quad_errors = []
    for line in proc.stdout.splitlines():
        t, sel, album, x, xl, tl, bl, xr, tr, br = map(int, line.split())
        observed[t, album] = [k / 65536 for k in (x, xl, xr)]
        tm = t / 1000
        # Use integer microseconds for event ordering at equal frame times.
        state = model['init']
        expected = 0
        for when, selected, poses in model['timeline']:
            if round(when * 1000) <= t:
                state, expected = poses, selected
        assert sel == expected
        _, z, yaw = p = model['cur'](state[album], tm)
        left, right = model['corners'](p)
        sine = math.sin(math.radians(yaw))
        for x_ref, depth, xc, top, bot in (
            (left, 160 - z + 35 * sine, xl, tl, bl),
            (right, 160 - z - 35 * sine, xr, tr, br),
        ):
            for actual, ref in ((xc, x_ref), (top, 120 - 44 * 160 / depth),
                                (bot, 120 + 26 * 160 / depth)):
                quad_errors.append(abs(actual / 65536 - ref))
    assert len({t for t, a in observed}) == len(frames)
    assert sel + 1 == int(read_csv(work / 'track.csv')[-1]['front_album'])
    tracked, absent = [], 0
    for row in read_csv(work / 'covers.csv'):
        key = round(float(row['t_ms']) * 1000), int(row['album']) - 1
        if key not in observed:
            absent += 1
            continue
        x, left, right = observed[key]
        if x >= -5 and (x <= 5 or max(left, right) < 320):
            tracked.append(abs(max(left, right) - 3 - float(row['x_right'])))
        if x <= 5 and (x >= -5 or min(left, right) > 0):
            tracked.append(abs(min(left, right) + 2 - float(row['x_left'])))
    sheet(run, work, frames, featured)
    result = (scenario, len(frames), sel + 1, len(tracked), absent,
              *population(tracked), *population(quad_errors))
    print('result', *result, flush=True)
    return result


def main():
    fixture, out = [Path(p).resolve() for p in sys.argv[1:]]
    out.mkdir(parents=True, exist_ok=True)
    tests = Path(__file__).resolve().parent
    sim = tests.parents[3] / 'build-sim-ipod6g'
    flags = subprocess.check_output(
        ['make', '-s', '--no-print-directory', '--eval=replay-flags: ; @echo $(CFLAGS)',
         'replay-flags'], cwd=sim, text=True).splitlines()[-1]
    subprocess.run(['cc', *shlex.split(flags), '-ffunction-sections', '-fdata-sections',
                    '-fsanitize=undefined', '-fno-sanitize-recover=all', '-c',
                    tests / 'stock_pixels.c', '-o', out / 'stock_pixels.o'], check=True)
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=undefined', '-fno-sanitize-recover=all',
                    tests / 'stock_replay.c', out / 'stock_pixels.o', '-Wl,--gc-sections', '-lm',
                    '-o', out / 'stock_replay'], check=True)
    with (out / 'art.rgb').open('wb') as artwork:
        for i in range(1, 25):
            art = Image.open(fixture / 'disk/src' / f'c{i:03}.jpg')
            artwork.write(art.convert('RGB').resize((128, 128)).tobytes())
    with (out / 'results.csv').open('w') as dst:
        rows = csv.writer(dst)
        rows.writerow('scenario frames final tracked absent tracker_p50 tracker_p90 tracker_max model_p50 model_p90 model_max'.split())
        for run in ('slow250', 'fast22', 'single', 'rev120', 'ramp37', 'fast36', 'rev37'):
            rows.writerow(replay(fixture, out, run))
            dst.flush()


if __name__ == '__main__':
    main()
