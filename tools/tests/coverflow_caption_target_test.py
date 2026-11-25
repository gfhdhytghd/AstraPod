"""Cover Flow caption: names the click's target at once, sits under the covers."""
from pathlib import Path
import re
import subprocess
import sys

root = Path(__file__).resolve().parents[2]
out = Path(sys.argv[1]).resolve()
out.mkdir(parents=True, exist_ok=True)
covers = root / 'apps-ipod/screens/covers'
carousel = (covers / 'carousel.c').read_text()
albums = (covers / 'album_covers.c').read_text()
artists = (covers / 'artist_portraits.c').read_text()
cider = (covers / 'cider_covers.c').read_text()


def fn(source, name):
    m = re.search(r'(?m)^(?:static\s+)?[\w *]+\b' + name + r'\([^;{}]*\)\s*\{',
                  source)
    assert m, name
    e = m.end()
    depth = 1
    while depth:
        depth += (source[e] == '{') - (source[e] == '}')
        e += 1
    return source[m.start():e]


def define(name):
    m = re.search(r'(?m)^#define ' + name + r'\b.*$', carousel)
    assert m, name
    return m.group(0)


def block(source, head):
    start = source.index(head)
    e = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[e] == '{') - (source[e] == '}')
        e += 1
    return source[start:e]


loop = fn(carousel, 'album_covers_loop')
owe = block(loop, 'if (captioned_index != carousel_caption_index())')
# Owed every time round, before the frame decides what to draw, rather than
# from inside the cover draw: a click owes the caption even on a frame whose
# covers do not draw.
assert loop.index(owe) < loop.index('if (caption_owed && !art_owed')
art = loop[loop.index('            if (drew_art)\n            {'):]
art = art[:art.index('cover_rows(&art_y, &art_h);')]
assert 'drew_caption' not in art
for source, name in ((artists, 'artist_draw_text'), (cider, 'text')):
    body = fn(source, name)
    assert 'carousel_caption_index()' in body and 'center_index' not in body, name
assert 'center_index' not in fn(carousel, 'carousel_album_caption')
assert 'center_index' not in fn(carousel, 'prefetch_captions')

program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))
#define DISPLAY_WIDTH 128
#define DISPLAY_HEIGHT 160
#define TAGCACHE_BUFSZ 64
''' + '\n'.join(define(n) for n in ('PF_NOMINAL_ART_H', 'PF_CAPTION_BAND',
                                    'PF_CAPTION_GAP', 'PF_ROW_MARGIN')) + r'''
struct pf_caption { int y1, y2; };
enum { pf_idle, pf_scrolling };
static int pf_state, target, center_index;
static bool pf_random_spinning;
static int pf_height = 240, pf_caption_band, pf_draw_y_shift;
struct font { int height, ascent; };
static struct font caption_font = { 15, 12 };
static int carousel_caption_font(bool bold) { assert(bold); return 2; }
static struct font *font_get(int f) { assert(f == 2); return &caption_font; }

static char drawn[TAGCACHE_BUFSZ];
static struct { struct { int year; long seek; } album_index[32];
                long album_untagged_seek; } carousel_idx;
static void get_album_name(int i, char *buf, int n)
{ snprintf(buf, n, "Album %d", i); }
static void get_album_artist(int i, char *buf, int n)
{ snprintf(buf, n, "Artist %d", i); }
static void carousel_album_caption(const char *album, const char *artist,
                                   int year, bool untagged, int revision)
{
    (void)artist; (void)year; (void)untagged; (void)revision;
    snprintf(drawn, sizeof drawn, "%s", album);
}

''' + fn(carousel, 'carousel_caption_index') + '\n' \
    + fn(carousel, 'caption_line2') + '\n' \
    + fn(carousel, 'caption_block') + '\n' \
    + fn(carousel, 'carousel_caption_layout') + '\n' \
    + fn(albums, 'draw_album_text') + r'''

static int captioned_index = -1;
static bool caption_owed_now(void)
{
    bool caption_owed = false;
    ''' + owe + r'''
    return caption_owed;
}

static const char *caption(void)
{
    draw_album_text();
    return drawn;
}

int main(void)
{
    /* At rest the caption names the centre cover. */
    center_index = target = 5;
    pf_state = pf_idle;
    assert(caption_owed_now() && !strcmp(caption(), "Album 5"));
    assert(!caption_owed_now());

    /* A click names the target before the covers have moved at all. */
    target = 6;
    pf_state = pf_scrolling;
    assert(caption_owed_now() && !strcmp(caption(), "Album 6"));
    assert(!caption_owed_now());
    /* Clicks faster than the covers: each one, not each arrival. */
    target = 8;
    assert(caption_owed_now() && !strcmp(caption(), "Album 8"));
    /* The covers passing the centre on their way change nothing. */
    center_index = 6;
    assert(!caption_owed_now() && !strcmp(caption(), "Album 8"));
    /* Arriving at the target owes nothing either. */
    center_index = 8;
    pf_state = pf_idle;
    assert(!caption_owed_now() && !strcmp(caption(), "Album 8"));

    /* A random spin names each cover as it reaches the middle. */
    center_index = 9;
    target = 20;
    pf_state = pf_scrolling;
    pf_random_spinning = true;
    assert(caption_owed_now() && !strcmp(caption(), "Album 9"));
    center_index = 10;
    assert(caption_owed_now() && !strcmp(caption(), "Album 10"));
    pf_random_spinning = false;
    pf_state = pf_idle;

    /* Bottom caption: the cover stays on rows 28-155 whatever the font, and
     * the caption sits PF_CAPTION_GAP rows under it. */
    pf_caption_band = PF_CAPTION_BAND;
    struct font fonts[] = { { 15, 12 }, { 14, 12 }, { 19, 16 } };
    for (unsigned i = 0; i < sizeof fonts / sizeof fonts[0]; i++)
    {
        struct pf_caption one, two;
        caption_font = fonts[i];
        int art_top = (pf_height - pf_caption_band - PF_NOMINAL_ART_H) / 2;
        assert(art_top == 28 && art_top + PF_NOMINAL_ART_H - 1 == 155);
        carousel_caption_layout(false, &one);
        carousel_caption_layout(true, &two);
        assert(one.y1 == two.y1 && two.y1 == 156 + PF_CAPTION_GAP);
        /* A descending first line clears the second line's box. */
        assert(two.y2 - two.y1 >= caption_font.height);
    }

    /* A 15 px caption font (height 15, ascent 12): rows 26-158 for the covers
     * and 160-192 for the caption, so 166 rows for the two together. */
    caption_font = (struct font){ 15, 12 };
    struct pf_caption cap;
    carousel_caption_layout(true, &cap);
    int art_y0 = 28 - PF_ROW_MARGIN, art_y1 = 28 + 128 + PF_ROW_MARGIN;
    int text_y0 = cap.y1, text_y1 = cap.y1 + caption_block(true);
    assert(cap.y1 == 160 && cap.y2 == 177);
    assert(art_y0 == 26 && art_y1 == 158 && art_y1 <= text_y0);
    assert(text_y1 == 192 && text_y1 - art_y0 == 166);

    /* Top caption: above the covers, the one-line block inside the two-line
     * one, so the reserved caption rows hold either. */
    pf_draw_y_shift = PF_CAPTION_BAND;
    struct pf_caption one, two;
    carousel_caption_layout(false, &one);
    carousel_caption_layout(true, &two);
    int top = 28 + PF_CAPTION_BAND;
    assert(two.y1 + caption_block(true) == top - PF_CAPTION_GAP);
    assert(one.y1 + caption_block(false) == top - PF_CAPTION_GAP);
    assert(one.y1 >= two.y1);
    return 0;
}
'''
c = out / 'caption_target.c'
c.write_text(program)
exe = out / 'caption_target'
subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-g',
                '-fsanitize=address,undefined', str(c), '-o', str(exe)],
               check=True)
subprocess.run([str(exe)], check=True)
print('PASS caption names the click target at once, a random spin its centre; '
      'covers 26-158 + caption 160-192 = 166 rows')
