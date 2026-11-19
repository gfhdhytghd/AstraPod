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
Alpha 0 is opaque and 15 is transparent.

The firmware keeps anchors, kerning rows, one index page and an LRU
glyph cache in the existing core font allocation. A cache miss reads
only the relevant page and record. Missing codepoints use the merged
face's U+FFFD glyph. Native text does not load another face on a miss.
Use the reserved icon fonts for icon tracks.

## Verification

Run the compiler twice in separate output directories. Run
`python3 tools/fonts/check.py compiler OUTPUT_A OUTPUT_B` for reproducibility,
glyph metrics, Noto masks and pair positioning. The SDL probe instruments
scratch source copies and uses the firmware's normal font loader and LCD
drawing calls. See `python3 tools/fonts/check.py --help`.
`arm_probe.py` uses Unicorn and pyelftools to count ARM instructions in
the release ELF with in-memory file reads.
