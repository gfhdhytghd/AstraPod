#!/usr/bin/env python3
"""Import the app icons, or pack them for the Games, Other Apps and Demos lists.

Each PNG here is one app's icon, 55x55 RGBA with square corners, named
after its module path
under .rockbox/rocks/ with '/' written as '__' (games__2048.png); the native
Music Quiz is native__music_quiz.png. The ids are the ones
apps-ipod/screens/browse/app_list.c lists.

    gen_app_icons.py --import DIR   copy the icons in DIR here, downscaled
                                    to 55x55; run again whenever new art
                                    arrives
    gen_app_icons.py --pack FILE    write the icons the firmware loads at
                                    run time; bundle-theme.sh does this

--import takes DIR/<id>.png, DIR/<id with '/' as '__'>.png and either with
a size suffix (games__2048-96.png), keeps the largest copy of each icon,
squares off rounded corners and needs Pillow. It overwrites the icons it finds and leaves the others.
--pack is pure Python, so a build host needs nothing beyond python3. An app
with no icon here draws the pack's placeholder tile.
"""
import functools
import importlib.util
import re
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
APP_LIST = HERE.parents[2] / 'screens' / 'browse' / 'app_list.c'
MAGIC = b'AICN'
VERSION = 1
ID_BYTES = 48
# The list art sizes of the built-in themes: Classic 55, Scrim 44.
SIZES = (55, 44)
PLACEHOLDER = '*'


def known_ids():
    text = APP_LIST.read_text()
    return set(re.findall(r'"((?:apps|demos|games|native)/[^"]+)"', text))


def file_name(app):
    return app.replace('/', '__') + '.png'


def import_icons(source):
    from PIL import Image

    ids = known_ids()
    best = {}
    for path in sorted(Path(source).rglob('*.png')):
        key = path.relative_to(source).with_suffix('').as_posix()
        key = key.replace('__', '/')
        if key not in ids:
            key = re.sub(r'[-_@](\d+)(px)?$', '', key)
        if key not in ids:
            continue
        with Image.open(path) as image:
            size = min(image.size)
        if key not in best or size > best[key][0]:
            best[key] = (size, path)
    for app, (_, path) in sorted(best.items()):
        with Image.open(path) as image:
            icon = image.convert('RGBA')
        if icon.size != (SIZES[0], SIZES[0]):
            icon = icon.resize((SIZES[0], SIZES[0]), Image.LANCZOS)
        icon.putdata(square_corners(list(icon.getdata()), SIZES[0]))
        icon.save(HERE / file_name(app), optimize=True)
        print('%s <- %s' % (file_name(app), path))
    missing = sorted(ids - set(best))
    if missing:
        print('no icon in %s for: %s' % (source, ' '.join(missing)))


@functools.lru_cache(maxsize=None)
def stock_art():
    spec = importlib.util.spec_from_file_location(
        'gen_stock_art', HERE.parent / 'stock' / 'gen_stock_art.py')
    stock = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(stock)
    return stock


def read_png(path):
    """RGBA tuples, through the stock art's reader beside this directory."""
    width, height, grey, px = stock_art().read_png(path)
    if grey:
        px = [(v, v, v, 255) for v in px]
    return width, height, px


def resample(px, src, dst):
    """Area-average a square src x src icon to dst x dst, alpha premultiplied
    so transparent pixels do not darken the edges."""
    if src == dst:
        return px
    scale = src / dst
    out = []
    for dy in range(dst):
        y0, y1 = dy * scale, (dy + 1) * scale
        for dx in range(dst):
            x0, x1 = dx * scale, (dx + 1) * scale
            acc = [0.0, 0.0, 0.0, 0.0]
            area = 0.0
            for sy in range(int(y0), min(src, int(y1 + 0.999999))):
                wy = min(y1, sy + 1) - max(y0, sy)
                for sx in range(int(x0), min(src, int(x1 + 0.999999))):
                    w = wy * (min(x1, sx + 1) - max(x0, sx))
                    r, g, b, a = px[sy * src + sx]
                    acc[0] += r * a * w
                    acc[1] += g * a * w
                    acc[2] += b * a * w
                    acc[3] += a * w
                    area += w
            alpha = acc[3] / area
            if acc[3]:
                rgb = [c / acc[3] for c in acc[:3]]
            else:
                rgb = [0, 0, 0]
            out.append(tuple(int(round(c)) for c in rgb) +
                       (int(round(alpha)),))
    return out


# The corner box: the icon's straight edges have begun by here.
SQUARE_REACH = 16
# Rows of an edge's outline and highlight, carried round a squared corner.
SQUARE_RIM = 2


def square_corners(px, size):
    """Fill the rounded-off corners of an icon tile, so it is square.

    The outer rows follow the straight edges, alpha and colour, so the
    outline, highlight and shadow run on into the corner; the rest of the
    corner settles from the pixels round it. Art inside the old curve is
    left as it is."""
    last = size - 1

    def at(x, y):
        return px[y * size + x]

    def along(x, y, dx, dy, want):
        """The edge's colour nearest the corner: the first pixel towards
        the middle whose alpha is the straight edge's."""
        while abs(at(x, y)[3] - want) > 8 and 0 < x < last and 0 < y < last:
            x, y = x + dx, y + dy
        return at(x, y)

    def profile(get):
        vals = sorted(get(k)[3]
                      for k in range(SQUARE_REACH, last - SQUARE_REACH + 1))
        return vals[len(vals) // 2]

    row_alpha = [profile(lambda k: at(k, y)) for y in range(size)]
    col_alpha = [profile(lambda k: at(x, k)) for x in range(size)]
    out = list(px)
    hole = []
    for y in range(size):
        for x in range(size):
            left, top = x < SQUARE_REACH, y < SQUARE_REACH
            if (not (left or x > last - SQUARE_REACH) or
                    not (top or y > last - SQUARE_REACH) or
                    at(x, y)[3] >= 248):
                continue
            ex = x if left else last - x
            ey = y if top else last - y
            alpha = row_alpha[y] * col_alpha[x] / 255
            if min(ex, ey) > SQUARE_RIM and alpha > 254:
                hole.append(y * size + x)
                continue
            edge = along(x, y, 1 if left else -1, 0, row_alpha[y])
            side = along(x, y, 0, 1 if top else -1, col_alpha[x])
            if ey != ex:
                rgb = edge[:3] if ey < ex else side[:3]
            else:
                rgb = [(p + q) / 2 for p, q in zip(edge[:3], side[:3])]
            out[y * size + x] = tuple(rgb) + (alpha,)
    for i in hole:
        out[i] = None
    for _ in range(200):
        settled = {}
        for i in hole:
            near = [out[j] for j in (i - 1, i + 1, i - size, i + size)
                    if out[j] is not None and out[j][3] > 200]
            if near:
                settled[i] = tuple(sum(c[k] for c in near) / len(near)
                                   for k in range(3)) + (255,)
        for i, value in settled.items():
            out[i] = value
    return [tuple(int(round(c)) for c in p) for p in out]


def placeholder(size):
    """A grey square tile, darkening downwards."""
    out = []
    for y in range(size):
        grey = int(round(222 - 70 * y / (size - 1)))
        out += [(grey, grey, grey + 6, 255)] * size
    return out


def block(px, size):
    """RGB565 little-endian rows, then the alpha as the LCD's alpha bitmaps
    keep it: 4 bits a pixel, 0 opaque, first pixel in the low nibble, each
    row starting on a byte. Both parts padded to 4 bytes."""
    pixels = bytearray()
    for r, g, b, _ in px:
        pixels += struct.pack('<H', ((r * 31 + 127) // 255) << 11 |
                              ((g * 63 + 127) // 255) << 5 |
                              ((b * 31 + 127) // 255))
    pixels += bytes(-len(pixels) % 4)
    alpha = bytearray()
    for y in range(size):
        row = [(255 - px[y * size + x][3]) >> 4 for x in range(size)]
        row += [15] * (len(row) % 2)
        alpha += bytes(row[i] | row[i + 1] << 4 for i in range(0, len(row), 2))
    alpha += bytes(-len(alpha) % 4)
    return bytes(pixels + alpha)


def pack(path):
    """Header, the sizes, then per icon its id and one offset per size, then
    the blocks. Little-endian throughout."""
    icons = [(PLACEHOLDER, None)]
    for png in sorted(HERE.glob('*.png')):
        width, height, px = read_png(png)
        if (width, height) != (SIZES[0], SIZES[0]):
            sys.exit('%s: %dx%d, not %dx%d' % (png, width, height,
                                               SIZES[0], SIZES[0]))
        icons.append((png.stem.replace('__', '/'), px))
    head = struct.pack('<4sIII', MAGIC, VERSION, len(icons), len(SIZES))
    head += b''.join(struct.pack('<I', s) for s in SIZES)
    at = len(head) + len(icons) * (ID_BYTES + 4 * len(SIZES))
    entries, blocks = [], []
    for app, px in icons:
        offsets = []
        for size in SIZES:
            data = block(resample(px, SIZES[0], size) if px else
                         placeholder(size), size)
            offsets.append(at)
            blocks.append(data)
            at += len(data)
        name = app.encode()
        if len(name) >= ID_BYTES:
            sys.exit('%s: id longer than %d bytes' % (app, ID_BYTES - 1))
        entries.append(name.ljust(ID_BYTES, b'\0') +
                       struct.pack('<%dI' % len(SIZES), *offsets))
    Path(path).write_bytes(head + b''.join(entries) + b''.join(blocks))


def main():
    if len(sys.argv) == 3 and sys.argv[1] == '--pack':
        pack(sys.argv[2])
    elif len(sys.argv) == 3 and sys.argv[1] == '--import':
        import_icons(sys.argv[2])
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
