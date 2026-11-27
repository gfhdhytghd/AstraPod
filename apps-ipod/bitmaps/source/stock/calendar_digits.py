#!/usr/bin/env python3
"""Write AstraPod_CalendarDigits_Image.png, the calendar page's day number.

The stock calendar page shows the day in Helvetica Bold at 48.21875 px, a
size no PodBox font carries. Its digits share one 27-pixel advance, so they
fit a strip of ten 27x36 cells, "0" to "9", each glyph drawn at its own
whole-pixel origin, the cell's left edge, with the strip's top 35 rows above
the baseline. The levels are the glyphs' 8-bit coverage, rendered as
tools/fonts/stock.py renders the stock fonts. FONT_DIR is the stock
firmware's Fonts folder; run gen_stock_art.py afterwards:

    python3 apps-ipod/bitmaps/source/stock/calendar_digits.py FONT_DIR
"""
import sys
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[3] / 'tools/fonts'))
import stock  # noqa: E402

OUT = HERE / 'AstraPod_CalendarDigits_Image.png'
SIZE64 = 3086
ADVANCE, HEIGHT, BASELINE = 27, 36, 35


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    source = Path(sys.argv[1]) / 'HelveticaBold.ttf'
    strip = np.zeros((HEIGHT, 10 * ADVANCE), np.uint8)
    with tempfile.TemporaryDirectory(prefix='podbox-raster-') as temp:
        lib = stock.build_raster(Path(temp), stock.FUSION_CACHE)
        glyphs = stock.outlines(lib, source, SIZE64,
                                {ord(str(d)) for d in range(10)})
    for d in range(10):
        record, advance = glyphs[ord(str(d))]
        left, top, width, height = (int.from_bytes(record[1:2], 'little',
                                                   signed=True),
                                    int.from_bytes(record[2:3], 'little',
                                                   signed=True),
                                    record[3], record[4])
        if advance != ADVANCE * 64:
            sys.exit('digit %d advances %d/64 px, not %d' % (d, advance,
                                                            ADVANCE))
        x, y = d * ADVANCE + left, BASELINE - top
        if left < 0 or left + width > ADVANCE or y < 0 or y + height > HEIGHT:
            sys.exit('digit %d does not fit its cell' % d)
        pixels = np.frombuffer(record[5:], np.uint8).reshape(height, width)
        strip[y:y + height, x:x + width] = pixels
    Image.fromarray(strip, 'L').save(OUT)


if __name__ == '__main__':
    main()
