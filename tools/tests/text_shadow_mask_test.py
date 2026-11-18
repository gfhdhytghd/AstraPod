"""Compare packed text-shadow pixels against the separate-source algorithm."""
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
out = Path(sys.argv[1]).resolve()
out.mkdir(parents=True, exist_ok=True)
text = (ROOT / 'apps-ipod/draw/text_shadow.c').read_text()
expand = re.search(r'^static void expand_mask\(.*?^}', text, re.M | re.S).group()
c = out / 'mask.c'
c.write_text(r'''
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define LCD_BLEND_OPAQUE 15
#define SHADOW_COV_FULL (4 * LCD_BLEND_OPAQUE)
#define MIN(a,b) ((a) < (b) ? (a) : (b))
''' + expand + r'''
int main(void)
{
    unsigned char mask[160 * 72];
    for (int w = 1; w <= 160; w++)
        for (int h = 1; h <= 72; h++)
            for (int opacity = 0; opacity <= 15; opacity += 3)
            {
                int hh = (h + 1) / 2;
                for (int i = 0; i < w * hh; i++)
                    mask[i] = (i + h + w) % 61;
                expand_mask(mask, w, hh, h, opacity);
                for (int y = 0; y < h; y++)
                    for (int x = 0; x < w; x++)
                    {
                        int cov = (y / 2 * w + x + h + w) % 61;
                        int alpha = 15 - (cov * opacity + 30) / 60;
                        assert(mask[y*w+x] == (alpha | (alpha << 4)));
                    }
            }
    puts("FU76: packed in-place shadow equals separate-source expansion across 69,120 shapes and opacities");
}
''')
binary = out / 'mask'
subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-fno-pie', '-no-pie', str(c),
                '-o', str(binary)], check=True)
subprocess.run([binary], check=True)
