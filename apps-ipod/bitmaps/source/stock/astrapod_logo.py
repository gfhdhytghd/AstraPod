#!/usr/bin/env python3
"""Write AstraPod_Logo_Image.png, AstraPod's logo as the About pane's mask.

The stock About pane shows a 77x80 white glossy mask: a bright rim on every
edge, near-white above a curved gloss line, and a grey that darkens towards
the lower right below it. This draws apps-ipod/bitmaps/source/logo.png in
that style, inside the same ink box (columns 4-70, rows 0-76), with the
shading measured from the stock mask. Run it after changing the logo, then
gen_stock_art.py:

    python3 apps-ipod/bitmaps/source/stock/astrapod_logo.py
"""
from pathlib import Path

import numpy as np
from PIL import Image

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parent / 'logo.png'
OUT = HERE / 'AstraPod_Logo_Image.png'

WIDTH, HEIGHT = 77, 80
INK = (4, 0, 67, 77)                    # x, y, width, height

# Levels as quadratics in (u, v), the position across the ink box, 0 to 1.
ABOVE = (267.5, -21.8, -25.2, 17.3, -5.4, -19.4)
BELOW = (348.4, -204.8, -242.6, 85.3, 127.9, 83.0)
# The gloss line, v as a quadratic in u.
GLOSS = (0.631, -0.994, 0.787)


def level(coef, u, v):
    c0, cu, cv, cuu, cvv, cuv = coef
    return c0 + cu * u + cv * v + cuu * u * u + cvv * v * v + cuv * u * v


def main():
    alpha = Image.open(SOURCE).convert('RGBA').getchannel('A')
    alpha = alpha.crop(alpha.getbbox())
    ix, iy, iw, ih = INK
    scale = min(iw / alpha.width, ih / alpha.height)
    w, h = round(alpha.width * scale), round(alpha.height * scale)
    x0, y0 = ix + (iw - w) // 2, iy + (ih - h) // 2
    canvas = Image.new('L', (WIDTH, HEIGHT))
    canvas.paste(alpha.resize((w, h), Image.BOX), (x0, y0))
    cover = np.asarray(canvas, dtype=float) / 255

    # The edge, and the first whole pixel inside it, are rim.
    solid = np.pad(cover >= 0.99, 1)
    inner = (solid[1:-1, 1:-1] & solid[:-2, 1:-1] & solid[2:, 1:-1] &
             solid[1:-1, :-2] & solid[1:-1, 2:])

    ys, xs = np.mgrid[0:HEIGHT, 0:WIDTH]
    u = (xs - x0) / w
    v = (ys - y0) / h
    above = v < np.polyval(GLOSS, u)
    shade = np.where(above, level(ABOVE, u, v), level(BELOW, u, v))
    shade = np.where(inner, np.clip(shade, 0, 255), 255)
    mask = np.rint(cover * shade).astype(np.uint8)
    Image.fromarray(mask, 'L').save(OUT, optimize=True)


if __name__ == '__main__':
    main()
