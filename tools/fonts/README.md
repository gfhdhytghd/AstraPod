# PodBox bitmap fonts

`python3 tools/fonts/compile.py OUTPUT` builds the fonts and emits
`sources.json` with the source of every glyph. The compiler requires
Fontconfig, Python with fontTools, a C compiler, FreeType and HarfBuzz
development files. Install the regular faces named in `compile.py` in
Fontconfig, or pass `--font FAMILY FILE FACE_INDEX` for an explicit input.
The compiler discovers the Simplified Chinese face of the Noto collection
by name and index. Keep the inputs and the generated files outside Git.

The existing `NN-noto-*` basenames retain the theme and saved setting keys.
They select a merged HarmonyOS Sans face at the indicated pixel size.
HarmonyOS Sans SC supplies CJK. Noto supplements missing glyphs at the
same size. Each bitmap has a fixed common baseline. Index and active lyrics
each load a separate native size. The UI reads alpha samples at 1:1.

## Stock firmware fonts

The `NN-helvetica*` fonts of the Classic and Plastic themes start from
Apple's Helvetica and its CJK bitmap strikes in the stock iPod Classic
firmware. `python3 tools/fonts/stock.py FONT_DIR` converts the stock Fonts
folder into the SGL1 glyph sets in `apps-ipod/fonts/stock/`, which are
committed; the stock files stay outside Git. `compile.py` then builds one
PBF1 font per set: the stock Helvetica glyphs first, then the CJK strike
named in the set, then the faces above at the set's whole pixel size. Every
glyph of these fonts has one phase and a whole-pixel advance, and none is
kerned, as on the stock firmware.

An SGL1 file is a little-endian header (`SGL1`, the scale in 1/64 px, the
fallback weight, line height, ascent, CJK strike ppem or 0, glyph count),
one entry per glyph (32-bit codepoint, 16-bit advance in 1/64 px, 16-bit
record size), then the PBF1 bitmap records in the same order.

| Basename | Stock face | Scale |
|---|---|---|
| `13-helvetica` | Helvetica | 13.125 px |
| `13-helvetica-bold` | Helvetica Bold | 13.125 px |
| `16-helvetica-bold` | Helvetica Bold | 16.25 px |
| `17-helvetica-bold` | Helvetica Bold | 17 px |
| `17-helvetica-bold-pane` | Helvetica Bold | 17.4375 px |

## PBF1 layout

All multibyte fields are little-endian. The header contains the magic
`PBF1`, six unsigned 16-bit fields, and five unsigned 32-bit fields.

| Width | Fields in order |
|---|---|
| 16 | Line height, ascent, maximum advance in pixels, maximum glyph record size, anchor count, replacement ordinal |
| 32 | Glyph count, kerning row count, index byte offset, bitmap byte offset, file size |

An anchor is a 32-bit codepoint for the first glyph in each page of 64
index records. The resident kerning table has one 16-bit pair ordinal per
row, plus a final end ordinal. Row entries contain an 8-bit column and a
signed 16-bit advance adjustment in 1/64 px. Equal rows and columns share
one class. Class zero has no kerning.

Each sorted 14-byte index record contains a 32-bit codepoint, a 32-bit
offset relative to the bitmap section, a 16-bit record size, a 16-bit
advance in 1/64 px, and two 8-bit kerning classes. A bitmap record starts
with the phase count, 1 or 4. Each phase contains signed 8-bit left and
top bearings, unsigned 8-bit width and height, and a tightly packed
row-major alpha mask. Each byte holds two pixels, low nibble first.
Alpha 0 is opaque and 15 is transparent. Bit 7 of the phase count marks
an RGB glyph. Each phase of an RGB glyph appends a second packed mask
with three samples per pixel, in R, G, B order. The first mask remains
available for grayscale drawing. Readers that support RGB also accept
older records with bit 7 clear.

The 8 and 10 px faces use native Fusion Pixel bitmaps for covered Han
characters and the usual outline fallback for other characters. The
12 px face stores ClearType-style RGB masks. The compiler downloads
pinned BDF archives to `~/.cache/podbox-font` on first use. Pass
`--fusion-cache` to use a different cache.

An optional six-byte trailer starts with `INK1`. Its last two bytes give
the maximum ink beyond the top and bottom of the logical line cell.
Lyrics uses these bounds for its native raster, row spacing and repaint
bands. The header's file length includes the trailer. The trailer does not change the index or bitmap records.

An optional `FIX1` descriptor precedes `INK1`. It stores seven unsigned
32-bit values: absolute directory, hot and cold offsets, cell stride, hot
cell count, total cell count and variable record count. The directory starts
with one 16-bit cell rank per codepoint in U+3400–U+9FFF, U+AC00–U+D7A3 and
U+F900–U+FAFF. Rank 0xFFFF means no fixed cell. The remaining directory
contains the sorted index entries for the variable records. A cell has the
usual bitmap record with a six-byte prefix (size, advance and kerning classes)
and transparent padding. The sorted PBF1 index still addresses every bitmap.

The hot cells are contiguous in GB2312 level 1 order, followed by Big5 primary
ideographs outside that set. ASCII, kana, punctuation and Hangul precede the
remaining cold fixed cells. Every output face retains its own repertoire.
Missing codepoints use that face's U+FFFD glyph.

Native lyrics, lists and carousel pages resolve their visible and coming
text from the resident directory before drawing. The worker sorts the records
by disk offset, then reads forward from the first aligned 4 KiB block to the
last required block. The screen thread publishes complete records and paints
only ready text in a cache-only scope. A 16-set, four-way lookup uses the
existing LRU pool for bitmap storage. The three lyric faces lease hot blocks
from memory reserved before audio initialization. Longer lyric documents
prioritize the initial viewport and request more glyphs as focus advances.
Readers can still load older PBF1 files containing only the sorted index.

## Verification

Run the compiler twice in separate output directories. Run
`python3 tools/fonts/check.py compiler OUTPUT_A OUTPUT_B` for reproducibility,
glyph metrics, Noto masks and pair positioning. The SDL probe instruments
scratch source copies and uses the firmware's normal font loader and LCD
drawing calls. See `python3 tools/fonts/check.py --help`.
`arm_probe.py` uses Unicorn and pyelftools to count ARM instructions in
the release ELF with in-memory file reads.
