#!/usr/bin/env python3
"""Write AstraPod's logo in the style of the stock Apple logos it replaces.

AstraPod_Logo_Image.png is the About pane's mask. The stock pane shows a
77x80 white glossy mask: a bright rim on every edge, near-white above a
curved gloss line, and a grey that darkens towards the lower right below it.
This draws apps-ipod/bitmaps/source/logo.png in that style, inside the same
ink box (columns 4-70, rows 0-76), with the shading measured from the stock
mask.

AstraPod_About_Logo_Image.png replaces Settings > About's 64x72 logo, an
image rather than a mask: the same kind of gloss in a faint blue-grey over a
soft black shadow, inside the ink box columns 6-58, rows 5-65. Its shading,
tint and shadow are measured from the stock image in the same way.

Run it after changing the logo, then gen_stock_art.py:

    python3 apps-ipod/bitmaps/source/stock/astrapod_logo.py
"""
from pathlib import Path

import numpy as np
from PIL import Image

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parent / 'logo.png'
OUT = HERE / 'AstraPod_Logo_Image.png'
ABOUT_OUT = HERE / 'AstraPod_About_Logo_Image.png'

WIDTH, HEIGHT = 77, 80
INK = (4, 0, 67, 77)                    # x, y, width, height

# Levels as quadratics in (u, v), the position across the ink box, 0 to 1.
ABOVE = (267.5, -21.8, -25.2, 17.3, -5.4, -19.4)
BELOW = (348.4, -204.8, -242.6, 85.3, 127.9, 83.0)
# The gloss line, v as a quadratic in u.
GLOSS = (0.631, -0.994, 0.787)

ABOUT_WIDTH, ABOUT_HEIGHT = 64, 72
ABOUT_INK = (6, 5, 53, 61)
ABOUT_ABOVE = (101.8, 280.7, 605.7, -84.1, -572.5, -644.0)
ABOUT_BELOW = (402.3, -254.5, -327.0, 50.0, 142.8, 189.2)
ABOUT_GLOSS = (1.589, -1.742, 0.888)
ABOUT_RIM = 244
# The darkest and lightest levels inside the stock logo: the quadratics run
# past them where AstraPod's logo reaches parts the apple does not.
ABOUT_RANGE = (175, 255)
# Green and blue as linear functions of red.
ABOUT_TINT = ((0.8177, 46.52), (0.7387, 66.69))
# The shadow: the logo's coverage blurred by a Gaussian of this sigma, in
# pixels, times this alpha.
ABOUT_SHADOW = (2.75, 81.0)


def level(coef, u, v):
    c0, cu, cv, cuu, cvv, cuv = coef
    return c0 + cu * u + cv * v + cuu * u * u + cvv * v * v + cuv * u * v


def coverage(alpha, size, ink):
    """The logo's alpha fitted into the ink box, centred: (cover, x0, y0,
    w, h), cover from 0 to 1."""
    ix, iy, iw, ih = ink
    scale = min(iw / alpha.width, ih / alpha.height)
    w, h = round(alpha.width * scale), round(alpha.height * scale)
    x0, y0 = ix + (iw - w) // 2, iy + (ih - h) // 2
    canvas = Image.new('L', size)
    canvas.paste(alpha.resize((w, h), Image.BOX), (x0, y0))
    return np.asarray(canvas, dtype=float) / 255, x0, y0, w, h


def shade(cover, x0, y0, w, h, above_coef, below_coef, gloss, rim,
          lowest=0, highest=255):
    """The level of every pixel: rim on the edge and the first whole pixel
    inside it, the gloss quadratics elsewhere."""
    height, width = cover.shape
    solid = np.pad(cover >= 0.99, 1)
    inner = (solid[1:-1, 1:-1] & solid[:-2, 1:-1] & solid[2:, 1:-1] &
             solid[1:-1, :-2] & solid[1:-1, 2:])
    ys, xs = np.mgrid[0:height, 0:width]
    u = (xs - x0) / w
    v = (ys - y0) / h
    above = v < np.polyval(gloss, u)
    levels = np.where(above, level(above_coef, u, v), level(below_coef, u, v))
    return np.where(inner, np.clip(levels, lowest, highest), rim)


def blur(image, sigma):
    reach = int(sigma * 4)
    k = np.arange(-reach, reach + 1)
    kernel = np.exp(-k * k / (2 * sigma * sigma))
    kernel /= kernel.sum()
    out = np.pad(image, reach)
    out = np.apply_along_axis(np.convolve, 0, out, kernel, 'same')
    out = np.apply_along_axis(np.convolve, 1, out, kernel, 'same')
    return out[reach:-reach, reach:-reach]


def pane_logo(alpha):
    cover, x0, y0, w, h = coverage(alpha, (WIDTH, HEIGHT), INK)
    levels = shade(cover, x0, y0, w, h, ABOVE, BELOW, GLOSS, 255)
    mask = np.rint(cover * levels).astype(np.uint8)
    Image.fromarray(mask, 'L').save(OUT, optimize=True)


def about_logo(alpha):
    cover, x0, y0, w, h = coverage(alpha, (ABOUT_WIDTH, ABOUT_HEIGHT),
                                   ABOUT_INK)
    red = shade(cover, x0, y0, w, h, ABOUT_ABOVE, ABOUT_BELOW, ABOUT_GLOSS,
                ABOUT_RIM, *ABOUT_RANGE)
    rgb = np.stack([red] + [np.clip(red * k + c, 0, 255)
                            for k, c in ABOUT_TINT], -1)
    sigma, strength = ABOUT_SHADOW
    shadow = np.clip(blur(cover, sigma) * strength, 0, 255) / 255
    # The logo over its shadow, which is black.
    alpha_out = cover + shadow * (1 - cover)
    rgb = np.where(alpha_out[..., None] > 0,
                   rgb * (cover / np.maximum(alpha_out, 1e-6))[..., None], 0)
    rgba = np.dstack([rgb, alpha_out * 255])
    Image.fromarray(np.rint(rgba).astype(np.uint8), 'RGBA').save(
        ABOUT_OUT, optimize=True)


def main():
    alpha = Image.open(SOURCE).convert('RGBA').getchannel('A')
    alpha = alpha.crop(alpha.getbbox())
    pane_logo(alpha)
    about_logo(alpha)


if __name__ == '__main__':
    main()
