#!/usr/bin/env python3
"""stock_flip.py STOCKFW_DIR OUTPUT_DIR PANEL_PNG [visible_x,y,width,height]

Replay the capture buttons with their release-to-animation offsets from §6.5.
Draw the app's captured panel as the back texture. A 256x256 image supplies
the whole texture. For an LCD screenshot, supply its panel bounds. The
fixture uses the top-left background colour to fill the hidden part.
"""
import csv
import math
from pathlib import Path
import subprocess
import sys

import numpy as np
from PIL import Image, ImageDraw

from stock_replay import build_probe, population, read_csv

CENTRE = (0, -9, 72.5, 0)
BACK = (0, 9.65, 116.25, 180)
PLAY = (-43, -10, 72.5, -9)
# Transition, capture, button-up index, UI delay (ms), frame range (inclusive).
# E uses the first captured return frame because the emulator stalls in WPS.
CASES = (
    ('A', 'flip_a', 0, 38, 1, 15), ('A', 'flip_b', 0, 20, 1, 15),
    ('B', 'flip_a', 1, 49, 16, 42),
    ('C', 'flip_a', 3, 46, 58, 73), ('C', 'flip_b', 1, 46, 16, 31),
    ('D', 'flip_c', 0, 24, 1, 16), ('E', 'flip_a', None, 0, 74, 104),
)
MOTION = {
    'A': (CENTRE, BACK, 500, 5), 'B': (BACK, CENTRE, 1000, 5),
    'C': ((*BACK[:3], -180), PLAY, 500, 2), 'D': (CENTRE, PLAY, 500, 2),
    'E': (PLAY, CENTRE, 1000, 5),
}


def polygon(op, elapsed):
    start, end, duration, power = MOTION[op]
    t = np.clip((elapsed / 1000 - (250 if op == 'E' else 0)) / duration, 0, 1)
    e = 1 - (1 - t) ** power
    x, y, z, yaw = np.array(start) + (np.array(end) - start) * e
    angle = math.radians(yaw)
    result = []
    for h in [-35, 35]:
        k = 160 / (160 - z - h * math.sin(angle))
        result.extend([160 + k * (x + h * math.cos(angle)),
                       120 + k * (y - 35), 120 + k * (y + 35)])
    return result, abs(yaw) > 90


def measured(stock, edge):
    # Sample the left silhouette by row and the top by column (§6.5,
    # perframe.py, YMAX=182). Compare at logged times without a per-frame fit.
    xl, tl, bl, xr, tr, br = edge
    xs, ys = [xl, xr, xr, xl], [tl, tr, br, bl]
    dark = np.min(np.asarray(stock), axis=2) < 231
    err = []
    for y in range(26, 182, 6):
        crosses = [xs[i] + (y - ys[i]) * (xs[i-1] - xs[i]) / (ys[i-1] - ys[i])
                   for i in range(4) if min(ys[i-1], ys[i]) <= y <= max(ys[i-1], ys[i])
                   and ys[i] != ys[i-1]]
        hits = np.flatnonzero(dark[y, :214])
        if crosses and max(0, min(crosses)) < 214 and hits.size:
            err.append(abs(hits[0] - max(0, min(crosses))))
    for x in range(20, 212, 8):
        if min(xl, xr) <= x <= max(xl, xr) and xl != xr:
            top = tl + (tr-tl) * (x-xl) / (xr-xl)
            hits = np.flatnonzero(dark[23:, x])
            if 23 <= top <= 239 and hits.size:
                err.append(abs(hits[0] + 23 - top))
    return err


def run_case(fixture, out, case):
    op, run, button, latency, first, last = case
    folder = fixture / 'emu/runs' / run
    frames = read_csv(folder / 'frames/frames.csv')[first:last+1]
    buttons = [r for r in read_csv(folder / 'input.csv') if r['op'] == 'btn' and r['arg2'] == 'up']
    origin = float(frames[0]['t_rel_ms']) if button is None else float(buttons[button]['t_rel_ms']) + latency
    elapsed = [round((float(f['t_rel_ms']) - origin) * 1000) for f in frames]
    wanted = [0, 55, 90, 170, 310, 500] if op != 'E' else [0, 220, 300, 400, 650, 1100]
    if op == 'B':
        wanted = [0, 90, 140, 230, 450, 900]
    picks = sorted({min(range(len(frames)), key=lambda i: abs(elapsed[i]/1000 - t)) for t in wanted})
    work = out / (op + '-' + run)
    work.mkdir(exist_ok=True)
    # D frames 3 and 4 retain a side wall after its centre has left |x| < 100.
    wall_check = {2, 3, 4} if op in ('D', 'E') else set()
    inputs = ''.join(f'{op} {t} {int(i in picks or i in wall_check)}\n'
                     for i, t in enumerate(elapsed))
    proc = subprocess.run([out / 'stock_flip', out / 'art.rgb', out / 'panel.rgb'], cwd=work,
                          input=inputs, text=True, stdout=subprocess.PIPE, check=True)
    measured_errors, ref_errors = [], []
    sheet = Image.new('RGB', (640, len(picks)*262), 'white')
    draw = ImageDraw.Draw(sheet)
    for i, (frame, line) in enumerate(zip(frames, proc.stdout.splitlines(), strict=True)):
        _, time, sides, back, done, *coords = line.split()
        t = int(time)
        edge = np.array(coords, dtype=float) / 65536
        reference, expected_back = polygon(op, t)
        assert bool(int(back)) == expected_back
        assert bool(int(done)) == (t >= (250000 if op == 'E' else 0) + 1000 * MOTION[op][2])
        if op in ('A', 'B'):
            assert int(sides) == 3
        ref_errors.extend(abs(edge - reference))
        data = np.fromfile(folder / 'frames' / frame['file'], '<u2').reshape(240, 320).astype('uint32')
        rgb = np.stack([((data>>11)&31)*255//31, ((data>>5)&63)*255//63, (data&31)*255//31], -1)
        stock = Image.fromarray(rgb.astype('uint8'))
        measured_errors.extend(measured(stock, edge))
        if i in wall_check:
            def wall_edge(image):
                band = np.asarray(image).astype(int)[55:145, 200:]
                cols = np.flatnonzero((np.ptp(band, axis=2) > 30).sum(axis=0) > 5)
                return int(cols[0] + 200) if cols.size else 320
            actual = wall_edge(Image.open(work / f'{op}-{t}.ppm'))
            target = wall_edge(stock)
            assert abs(actual-target) <= (8 if op == 'E' else 5), (frame['file'], actual, target)
        if i in picks:
            y = picks.index(i) * 262
            draw.text((5, y+3), f'{op}   {run}   stock   {frame["t_rel_ms"]} ms', fill='black')
            draw.text((325, y+3), f'AstraPod C   +{t/1000:.1f} ms', fill='black')
            sheet.paste(stock, (0, y+22))
            sheet.paste(Image.open(work / f'{op}-{t}.ppm'), (320, y+22))
    sheet.save(work / 'comparison.png')
    return (op, run, len(frames), origin, len(measured_errors), *population(measured_errors),
            *population(ref_errors))


def main():
    fixture, out, image = [Path(p).resolve() for p in sys.argv[1:4]]
    build_probe(fixture, out, 'stock_flip', ['-DREPLAY_STOCK_FLIP'])
    panel = Image.open(image).convert('RGB')
    if panel.size != (256, 256):
        x, y, w, h = map(int, sys.argv[4].split(','))
        texture = Image.new('RGB', (256, 256), panel.getpixel((0, 0)))
        texture.paste(panel.crop((x, y, x+w, y+h)), (x-32, y-27))
        panel = texture
    (out / 'panel.rgb').write_bytes(panel.tobytes())
    with (out / 'results.csv').open('w') as dst:
        writer = csv.writer(dst)
        writer.writerow('transition run frames origin_ms edges stock_p50 stock_p90 stock_max float_p50 float_p90 float_max'.split())
        for case in CASES:
            row = run_case(fixture, out, case)
            print(*row, flush=True)
            writer.writerow(row)


if __name__ == '__main__':
    main()
