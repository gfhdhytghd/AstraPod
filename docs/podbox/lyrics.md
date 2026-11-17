# Animated Now Playing lyrics (FU57)

SELECT toggles the same lyrics page for local playback and Cider. MENU returns
to Now Playing too, including entry through a context menu. The previous local
SELECT action, including the A/B marker key, is reachable with SELECT+PLAY.
Cider A/B markers also use SELECT+PLAY. The local setting still stores its
previous action under `wps select action` for configuration compatibility. The English settings
label is `SELECT+PLAY Action`.

The wheel follows list direction (clockwise advances); remote + goes to the
previous line and - to the next line. Remote reports always move one line;
fast wheel bursts accelerate to four lines per event. Timed lyrics return to
Auto four seconds after the last scroll. Untimed .txt and Cider now/lyrics rows
stay manual and show no Manual pill. Play/pause and skip use the playback
source; local skip keeps the WPS skip-length, cuesheet, spoken-word and A/B
policy. Seeking uses the WPS seek loop with a lyrics render callback.

Entry/exit: 220/180 ms, cubic ease out, a 24 px slide over a progressively
dimmed live WPS snapshot. All scroll targets take 260 ms from the displayed
position, including reversal, multi-line acceleration and idle return. Bitmap
18/26 px glyphs cross-fade through a separate mask while their line heights
interpolate. Track changes cross-fade for 200 ms. The 12/18/26 Noto fonts,
including CJK, already ship in the FU55 package; a missing font uses the UI
font. Enhanced LRC word progress advances in pixels between word timestamps.

The page claims the linker's shared 3 MiB app_buffer with the owner `lyrics`.
It borrows three 153600-byte RGB565 frames (460800 bytes total): glyph mask,
WPS/old-track snapshot, composition layer. It releases the claim before a USB
handler or another screen runs. This grows neither the reserved app_buffer nor
the audio buffer. The existing model retains its 32 KiB core allocation only
while the page is open. When another screen owns app_buffer, the page runs
without snapshots: entry/exit fade through the gradient in two halves, track
changes fade out/in in two 100 ms halves, and glyphs use interpolated colours.
It never overwrites the other owner's buffer or removes that owner's claim.

The frame deadline subtracts render time and targets 33 fps (three ticks at
100 Hz). The idle wait wakes for the next lyric line, the next progress-bar
pixel, the manual-return deadline or a 500 ms source check. Enhanced word
highlighting animates at the frame cadence; paused/untimed idle text sleeps.
Host/simulator cadence is not a hardware FPS guarantee.

Retained settings: colour mode, previous/next opacity, word highlighting,
backlight hold. Removed from the menu: font override, line spacing, alignment
and animation duration. Their old configuration keys remain readable; the
fixed animated layout ignores them. Lyrics settings remain under the Viewers
settings menu; holding MENU in the page no longer opens a second settings UI.

Tests: `lyrics_motion_test.py` links the actual pure C functions and rejects
eight deliberately broken variants. `lyrics_document_test.py` executes the
actual parser/model with a 32-bit ABI and ASan/UBSan. `lyrics_viewer_test.py`
executes the actual page/model against recording UI/source adapters, including
75 remote rows across a page-cache miss and arena-denied cleanup. The actual
SDL simulator is captured by `fu57_sim_evidence.py`; its PNG/GIF sequences and
capture timestamps are saved in the task evidence directory.
