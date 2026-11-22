#!/usr/bin/env python3
"""Bake the native AstraPod logo and About QR bitmaps for the firmware."""

import argparse
from io import BytesIO
from pathlib import Path
import subprocess

from PIL import Image, ImageChops, ImageDraw, ImageFont


URL = "https://github.com/gfhdhytghd/AstraPod"
LOGO_W, LOGO_H = 200, 46
MARK = 40
LABEL_GAP = 10
BASELINE = 34
CAP_HEIGHT = 22
WHITE = (255, 255, 255)
ACCENT = (0x9D, 0xB8, 0xFF)

# The B series mark, apps-ipod/bitmaps/source/logo.svg, in its 100-unit
# viewBox. MARK_VIEW is the square of it that fills the MARK slot.
MARK_VIEW = (12, 88)
RING_R, RING_W = 33, 9
RING_GAP = (-72, -18)                  # degrees, clockwise from 3 o'clock
RING_ENDS = ((60.20, 18.62), (81.38, 39.80))
RING_TOP, RING_BOTTOM = (0x9D, 0xB8, 0xFF), (0x54, 0x68, 0xE8)
DOT_R = 10
DOT = (0x9D, 0xB8, 0xFF)
STAR = (0xFF, 0xD2, 0x7A)
STAR_PATH = ((73.3345, 12.6655), (75.2945, 24.7055), (87.3345, 26.6655),
             (75.2945, 28.6255), (73.3345, 40.6655), (71.3745, 28.6255),
             (59.3345, 26.6655), (71.3745, 24.7055), (73.3345, 12.6655))


def wordmark_font():
    match = subprocess.check_output(
        ["fc-match", "-f", "%{file}\n%{index}\n", "HarmonyOS Sans:style=Medium"],
        text=True,
    )
    path, index = match.splitlines()
    for size in range(CAP_HEIGHT, CAP_HEIGHT * 2):
        font = ImageFont.truetype(path, size, index=int(index),
                                  layout_engine=ImageFont.Layout.RAQM)
        if font.getname() != ("HarmonyOS Sans", "Medium"):
            raise SystemExit("The AstraPod logo requires HarmonyOS Sans Medium.")
        _, top, _, bottom = font.getmask("H").getbbox()
        if bottom - top == CAP_HEIGHT:
            return font
    raise SystemExit("No font size gives the required 22 px cap height.")


def mark_image():
    """The mark as an RGBA image MARK pixels square."""
    samples = 8
    size = MARK * samples
    lo, hi = MARK_VIEW
    scale = size / (hi - lo)

    def px(x, y):
        return (x - lo) * scale, (y - lo) * scale

    def disc(draw, cx, cy, r):
        x, y = px(cx, cy)
        draw.ellipse((x - r * scale, y - r * scale,
                      x + r * scale, y + r * scale), fill=255)

    ring = Image.new("L", (size, size))
    draw = ImageDraw.Draw(ring)
    outer = RING_R + RING_W / 2
    draw.arc((*px(50 - outer, 50 - outer), *px(50 + outer, 50 + outer)),
             RING_GAP[1], RING_GAP[0] + 360, fill=255,
             width=round(RING_W * scale))
    for x, y in RING_ENDS:
        disc(draw, x, y, RING_W / 2)

    dot = Image.new("L", (size, size))
    disc(ImageDraw.Draw(dot), 50, 50, DOT_R)

    # Each pair after the first point is a quadratic control and end point.
    points = [px(*STAR_PATH[0])]
    for i in range(1, len(STAR_PATH), 2):
        (x0, y0), (cx, cy), (x1, y1) = points[-1], px(*STAR_PATH[i]), \
            px(*STAR_PATH[i + 1])
        for step in range(1, 17):
            t = step / 16
            points.append(((1 - t) ** 2 * x0 + 2 * (1 - t) * t * cx + t * t * x1,
                           (1 - t) ** 2 * y0 + 2 * (1 - t) * t * cy + t * t * y1))
    star = Image.new("L", (size, size))
    ImageDraw.Draw(star).polygon(points, fill=255)

    # The ring's gradient runs corner to corner of the circle's own box.
    gradient = Image.new("RGB", (MARK, MARK))
    box_lo, box_hi = 50 - RING_R, 50 + RING_R
    for y in range(MARK):
        for x in range(MARK):
            u = lo + (x + 0.5) * (hi - lo) / MARK
            v = lo + (y + 0.5) * (hi - lo) / MARK
            t = min(max((u + v - 2 * box_lo) / (2 * (box_hi - box_lo)), 0), 1)
            gradient.putpixel((x, y), tuple(
                round(a + (b - a) * t) for a, b in zip(RING_TOP, RING_BOTTOM)))

    mark = Image.new("RGBA", (MARK, MARK))
    for colour, cover in ((gradient, ring), (DOT, dot), (STAR, star)):
        layer = (colour.convert("RGBA") if isinstance(colour, Image.Image)
                 else Image.new("RGBA", (MARK, MARK), colour))
        layer.putalpha(cover.resize((MARK, MARK), Image.Resampling.BOX))
        mark = Image.alpha_composite(mark, layer)
    return mark


def logo_masks():
    font = wordmark_font()
    _, _, right, _ = font.getbbox("AstraPod", anchor="ls")
    width = MARK + LABEL_GAP + right
    if width > LOGO_W:
        raise SystemExit("AstraPod logo exceeds its 200 px slot.")

    white = Image.new("L", (LOGO_W, LOGO_H))
    accent = Image.new("L", (LOGO_W, LOGO_H))

    word_x = MARK + LABEL_GAP
    # Keep kerning across the colour boundary as well as inside each run.
    pod_x = word_x + font.getlength("AstraP") - font.getlength("P")
    ImageDraw.Draw(white).text((word_x, BASELINE), "Astra", font=font, anchor="ls", fill=255)
    ImageDraw.Draw(accent).text((pod_x, BASELINE), "Pod", font=font, anchor="ls", fill=255)

    # Quantize once to the LCD's 16 alpha levels. Pack the inverse nibble
    # values when emitting the header.
    quantized = [(value + 8) // 17 * 17 for value in range(256)]
    white, accent = white.point(quantized), accent.point(quantized)
    # The advance includes side bearings. Centre the visible pixels.
    _, _, ink_right, _ = ImageChops.lighter(white, accent).getbbox()
    inset = (LOGO_W - ink_right) // 2
    return (ImageChops.offset(white, inset, 0),
            ImageChops.offset(accent, inset, 0), inset)


def qr_bitmap():
    png = subprocess.check_output(
        ["qrencode", "-s", "1", "-m", "0", "-l", "M", "-v", "3",
         "--strict-version", "-o", "-", URL]
    )
    qr = Image.open(BytesIO(png)).convert("1")
    if qr.size != (29, 29):
        raise SystemExit("About QR must keep the existing 29-module dimensions.")
    pixels = qr.load()
    data = []
    for y in range(qr.height):
        for x in range(0, qr.width, 8):
            bits = 0
            for i in range(min(8, qr.width - x)):
                if not pixels[x + i, y]:
                    bits |= 0x80 >> i
            data.append(bits)
    return qr.width, data


def c_bytes(data):
    return "\n".join(
        "    " + " ".join(f"0x{value:02x}," for value in data[i:i + 12])
        for i in range(0, len(data), 12)
    )


def c_pixels(pixels):
    return "\n".join(
        "    " + " ".join(f"FB_RGBPACK({r}, {g}, {b})," for r, g, b in pixels[i:i + 4])
        for i in range(0, len(pixels), 4)
    )


def packed_alpha(mask):
    pixels = [(15 - value // 17) for value in mask.tobytes()]
    return [lo | hi << 4 for lo, hi in zip(pixels[::2], pixels[1::2])]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("header", type=Path)
    parser.add_argument("--preview", type=Path, help="also write the transparent 1:1 PNG")
    args = parser.parse_args()
    white, accent, mark_x = logo_masks()
    mark = mark_image()
    mark_alpha = mark.getchannel("A").point(
        [(value + 8) // 17 * 17 for value in range(256)])
    rgb = mark.convert("RGB").tobytes()
    mark_rgb = [tuple(rgb[i * 3:i * 3 + 3]) if a else (0, 0, 0)
                for i, a in enumerate(mark_alpha.tobytes())]
    qr_n, qr = qr_bitmap()
    args.header.write_text(f"""/* Generated by tools/gen_about_art.py. Do not edit. */
#define ABOUT_LOGO_W {LOGO_W}
#define ABOUT_LOGO_H {LOGO_H}
#define ABOUT_LOGO_ACCENT LCD_RGBPACK({ACCENT[0]}, {ACCENT[1]}, {ACCENT[2]})

/* Packed 4bpp inverse alpha. The first pixel is in the low nibble. */
static const unsigned char about_logo_white[] = {{
{c_bytes(packed_alpha(white))}
}};
static const unsigned char about_logo_accent[] = {{
{c_bytes(packed_alpha(accent))}
}};

/* The mark, in colour, drawn through its own alpha at this offset. */
#define ABOUT_MARK_X {mark_x}
#define ABOUT_MARK_Y {(LOGO_H - MARK) // 2}
#define ABOUT_MARK_N {MARK}
static const fb_data about_mark[] = {{
{c_pixels(mark_rgb)}
}};
static const unsigned char about_mark_alpha[] = {{
{c_bytes(packed_alpha(mark_alpha))}
}};

/* {URL}, one bit per module, MSB first, byte-padded rows. */
#define ABOUT_QR_N {qr_n}
static const unsigned char about_qr[] = {{
{c_bytes(qr)}
}};
""", encoding="ascii")
    if args.preview:
        layer = Image.new("RGBA", white.size, WHITE)
        layer.putalpha(white)
        blue = Image.new("RGBA", accent.size, ACCENT)
        blue.putalpha(accent)
        logo = Image.alpha_composite(layer, blue)
        mark.putalpha(mark_alpha)
        logo.alpha_composite(mark, (mark_x, (LOGO_H - MARK) // 2))
        logo.save(args.preview)


if __name__ == "__main__":
    main()
