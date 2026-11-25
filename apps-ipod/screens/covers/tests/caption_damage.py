#!/usr/bin/env python3
"""Replay caption DMA damage with the Classic's shipped PBF glyphs.

Usage: python caption_damage.py [carousel.c] [18-noto-sans-bold.pbf]
"""
import ctypes as ct
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
SOURCE = Path(sys.argv[1]) if len(sys.argv) > 1 else HERE.parent / "carousel.c"
FONT = Path(sys.argv[2]) if len(sys.argv) > 2 else (
    ROOT / "build-sim-ipod6g/simdisk/.rockbox/fonts/18-noto-sans-bold.pbf")
W, H = 320, 240


def compile_probe(tmp, font):
    source = SOURCE.read_text()
    def region(start, end):
        return source[source.index(start):source.index(end, source.index(start))]
    template = r"""
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include <string.h>
#define MAX(a,b) ((a)>(b)?(a):(b))
#define MIN(a,b) ((a)<(b)?(a):(b))
#define LCD_WIDTH 320
#define PF_NOMINAL_ART_H 160
#define PF_CAPTION_GAP 4
#define HAVE_PODBOX_FONT
#define USEC_TIMER 0
#define UI_PERF_ART 1
#define UI_PERF_CAPTION 2
struct font { int height, ascent; struct { int file_size, ink_above, ink_below; } pbf; };
static struct font face = {FONT_VALUES};
static struct font *font_get(int id) { (void)id; return &face; }
static int carousel_caption_font(bool bold) { return bold; }
static bool flat_view(void) { return false; }
struct pf_caption { int y1, y2; };
int pf_text_y, pf_text_h, pf_draw_y_shift, pf_height = 240, pf_caption_band = 55;
int pf_vp_y, caption_rows_y, caption_rows_h;
int painted_top=INT_MAX, painted_end;
static bool caption_flush_owed;
static int caption_flush_top, caption_flush_end, caption_left, caption_right;
uint8_t fb[240][320], panel[240][320];
static void lcd_update_rect(int x,int y,int w,int h) {
    for(int i=0; i<h; i++) memcpy(panel[y+i]+x, fb[y+i]+x, w);
}
struct ui_perf_transfers;
#define ui_perf_add_transfer(a,b,c) ((void)(a),(void)(b),(void)(c))
LAYOUT
STAGE
void layout(int top, int two, int out[4]) {
    pf_draw_y_shift = top ? pf_caption_band : 0;
    update_caption_bounds();
    struct pf_caption cap;
    carousel_caption_layout(two,&cap);
    out[0]=cap.y1; out[1]=cap.y2; out[2]=pf_text_y; out[3]=pf_text_y+pf_text_h;
}
void measured(int top,int end,int rows[2]) {
    caption_rows_y=pf_text_y; caption_rows_h=pf_text_h;
    painted_top=top; painted_end=end;
    MEASURED
    rows[0]=caption_rows_y; rows[1]=caption_rows_h;
}
void submit(int ink_left, int ink_right, int rows_y, int rows_h) {
    bool drew_caption = true;
    caption_rows_y=rows_y; caption_rows_h=rows_h;
    DAMAGE
}
int present_art(int dirty_top, int dirty_end) {
    ART_OVERLAP
    if (rows) lcd_update_rect(0,dirty_top,LCD_WIDTH,rows);
    return rows;
}
int can_paint_caption(void) {
    bool caption_owed = true;
    struct ui_scene *fade = NULL;
    PAINT_DECISION
    return drew_caption;
}
int transfer(int pixels) {
    if (!caption_flush_owed) return 0;
    int rows = stage_caption(caption_left,caption_right,&caption_flush_top,
                             caption_flush_end,pixels,0);
    caption_flush_owed = caption_flush_top < caption_flush_end;
    return rows;
}
"""
    height, ascent, above, below = font
    values = f"{height},{ascent},{{1,{above},{below}}}"
    program = (template.replace("FONT_VALUES", values)
               .replace("LAYOUT", region("static int caption_line2(", "/* Switching views"))
               .replace("STAGE", region("static int stage_caption(", "\n}") + "\n}")
               .replace("ART_OVERLAP", region("unsigned parts = UI_PERF_ART, caption_rows = 0;",
                                              "if (rows)"))
               .replace("PAINT_DECISION", region("bool drew_caption =", ";\n")+";")
               .replace("MEASURED", region("int old_top = pf_text_y, old_end = pf_text_y + pf_text_h;",
                                            "if (paint->tracing)") if "int old_top = pf_text_y" in source else "")
               .replace("DAMAGE", region("if (drew_caption && pf_text_h > 0 && caption_rows_h",
                                         "if (full_flush_owed)")))
    path = Path(tmp) / "probe.so"
    subprocess.run(["cc", "-x", "c", "-", "-O2", "-fPIC", "-shared", "-o", path],
                   input=program, text=True, check=True)
    return ct.CDLL(str(path))


class Pbf:
    def __init__(self, path):
        self.raw = path.read_bytes()
        header = struct.unpack_from("<4s6H5I", self.raw)
        assert header[0] == b"PBF1" and self.raw[-6:-2] == b"INK1"
        self.metrics = (*header[1:3], *self.raw[-2:])
        self.index = {}
        for i in range(header[7]):
            cp, off, _, advance, _, _ = struct.unpack_from("<IIHHBB", self.raw, header[9]+14*i)
            self.index[chr(cp)] = header[10]+off, (advance+32)//64

    def paint(self, fb, text, line):
        # A fixed glyph phase isolates vertical bearings from kerning.
        x = (W - sum(self.index[ch][1] for ch in text)) // 2
        for ch in text:
            offset, advance = self.index[ch]
            left, bearing, width, height = struct.unpack_from("<bbBB", self.raw, offset+1)
            top = line + self.metrics[1] - bearing
            for r in range(height):
                for c in range(width):
                    b = self.raw[offset+5+(r*width+c)//2]
                    ink = 15 - ((b >> (4*((r*width+c)%2))) & 15)
                    px, py = x+left+c, top+r
                    if ink and 0 <= px < W and 0 <= py < H:
                        fb[py*W+px] = ink
            x += advance


def compare(buf, ref):
    return [i for i, (a, b) in enumerate(zip(buf, ref)) if a != b]


def exercise(lib, face, top, fast):
    pixels = ct.c_uint8 * (W*H)
    fb, panel = (pixels.in_dll(lib, s) for s in ("fb", "panel"))
    failures = []
    ct.memset(fb, 0, W*H)
    ct.memset(panel, 0, W*H)
    ct.c_int.in_dll(lib, "painted_top").value = 0x7fffffff
    ct.c_int.in_dll(lib, "painted_end").value = 0
    previous = (0, W)
    names = [("A", "Çç mm"), ("Middle name", "A"), ("Çç", ""),
             ("A", "〱"), ("B", "A")]
    for frame, (title, artist) in enumerate(names):
        bounds = (ct.c_int*4)()
        lib.layout(top, bool(artist), bounds)
        y1, y2, first, end = bounds
        ref = bytearray(W*H)
        face.paint(ref, title, y1)
        face.paint(ref, artist, y2)
        ink = [i%W for i, pixel in enumerate(ref) if pixel]
        left, right = min(ink), max(ink)+1
        for y in range(first, end):
            ct.memset(ct.byref(fb, y*W+previous[0]), 0, previous[1]-previous[0])
        face.paint(fb, title, y1)
        face.paint(fb, artist, y2)
        rows = (ct.c_int*2)()
        ink_rows = [i//W for i, pixel in enumerate(ref) if pixel]
        lib.measured(min(ink_rows), max(ink_rows)+1, rows)
        lib.submit(min(left,previous[0]),max(right,previous[1]),*rows)
        previous = left,right
        if frame == 2:
            # Include an art transfer that overlaps the upper caption rows.
            lib.present_art(max(0, first-8), first+6)
        # A full slot is skipped while art owns the staging buffer.
        # The next available transfer can end halfway down the artist line.
        if frame % 2 == 0:
            lib.transfer((y2+6-first)*max(1,right-left))
        if not fast or frame == len(names)-1:
            while lib.transfer(7*W):
                pass
            for label, got, expected in (("display",panel,fb), ("clear",fb,ref)):
                bad = compare(got, expected)
                if bad:
                    failures.append(f"{label} frame {frame}: {len(bad)} pixels at rows "
                                    f"{bad[0]//W}..{bad[-1]//W}")
    print(f"{'top' if top else 'bottom'}, {'fast' if fast else 'slow'}: "
          + ("; ".join(failures) if failures else "all pixels match"))
    return len(failures)


def rapid_replacement(lib, face):
    pixels = ct.c_uint8 * (W*H)
    fb, panel = (pixels.in_dll(lib, s) for s in ("fb", "panel"))
    ct.memset(fb, 0, W*H)
    ct.memset(panel, 0, W*H)
    bounds = (ct.c_int*4)()
    lib.layout(0, 1, bounds)
    y1, y2, top, end = bounds
    # Equivalent to 138 art rows: leave 12 full-width caption rows per TE.
    # Each replacement is a valid new caption. Full-width damage makes
    # this independent of a particular font's horizontal side bearings.
    for frame in range(48):
        if lib.can_paint_caption():
            ct.memset(ct.byref(fb, top*W), 0, (end-top)*W)
            face.paint(fb, "Album" if frame % 2 else "A", y1)
            face.paint(fb, "Artist Y" if frame % 2 else "Artist Z", y2)
            lib.submit(0, W, top, end-top)
        lib.present_art(25,163)
        lib.transfer(12*W)
    starved = not any(panel[y2*W:(y2+face.metrics[0])*W])
    while lib.transfer(12*W):
        pass
    # After the last button event, give the final pending label a turn to
    # paint, then drain its full damage exactly as the resting loop does.
    if not lib.can_paint_caption():
        raise AssertionError("pending label still blocked after drain")
    ct.memset(ct.byref(fb, top*W), 0, (end-top)*W)
    face.paint(fb, "Album", y1)
    face.paint(fb, "Artist Y", y2)
    lib.submit(0, W, top, end-top)
    while lib.transfer(12*W):
        pass
    final = len(compare(panel,fb))
    print(f"48 consecutive updates, 12 rows/frame: artist "
          f"{'never reached panel' if starved else 'reached panel'}; "
          f"{final} differences after drain")
    return starved or final


def full_flush(lib):
    pixels = ct.c_uint8 * (W*H)
    fb, panel = (pixels.in_dll(lib, s) for s in ("fb", "panel"))
    failed = False
    for first, end in ((0, 48), (180, H), (180, 220)):
        ct.memset(fb, 42, W*H)
        ct.memset(panel, 0, W*H)
        lib.submit(13, 120, first, end-first)
        rows = lib.present_art(0,H)
        while lib.transfer(7*W):
            pass
        bad = len(compare(panel,fb))
        print(f"full-screen flush with caption [{first},{end}): {rows} full-width rows,"
              f" {bad} differing pixels after drain")
        failed |= bool(bad)
    return failed


if __name__ == "__main__":
    face = Pbf(FONT)
    with tempfile.TemporaryDirectory() as tmp:
        lib = compile_probe(tmp, face.metrics)
        failed = sum(exercise(lib, face, top, fast) for top in (False, True)
                     for fast in (False, True))
        failed += bool(rapid_replacement(lib, face))
        failed += full_flush(lib)
        sys.exit(bool(failed))
