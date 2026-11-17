# Animated Now Playing lyrics (FU57)

SELECT toggles the same lyrics page for local playback and Cider. Apple Remote
Play/Pause hold also toggles it. A tap still pauses or resumes. The click-wheel
Play hold still opens the WPS context menu. MENU returns
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

`Lyrics lead` defaults to +200 ms. It accepts −1000 to +1000 ms in 50 ms
steps. The page adds it to the local or remote lyrics clock and clamps the
result to the track. A negative value delays the lyrics. Line focus, words,
and return to Auto use this clock. Playback and the progress bar use the
original time. Offsets carried by the document still apply.

The host converts lyrics for the current remote track in a worker and keeps
one track of rows in memory. WPS requests them on track change. Idle WPS
passes copy up to eight rows into the existing 32 KiB lyrics document. The
lyrics page shows completed lines while later rows arrive. A track or backend
change discards obsolete responses. The document stays warm across visits
to the same remote track. The local source releases the remote document.

Local playback looks for timed sidecars (.lrc, .lrc8, .snc) beside the audio
file and in its parent directories, then for .txt sidecars. With no sidecar,
SELECT reads lyrics directly from the audio file. MP4 uses UTF-8 `©lyr`.
FLAC and Ogg Vorbis use `LYRICS` or `UNSYNCEDLYRICS`. MP3 prefers a
millisecond-timed ID3v2 `SYLT` frame, with `USLT` as a fallback.
Text tags accept LRC, enhanced LRC and plain text. Source files open read-only.
The document keeps the prefix that fits the existing 32 KiB allocation.
ID3 frame staging uses up to one third of that allocation.

Entry/exit: 220/180 ms, cubic ease out, a 24 px slide over a progressively
dimmed live WPS snapshot. All scroll targets take 260 ms from the displayed
position, including reversal, multi-line acceleration and idle return. Bitmap
18/26 px glyphs cross-fade through a separate mask while their line heights
interpolate. Track changes cross-fade for 200 ms. The 12/18/26 Noto fonts,
including CJK, already ship in the FU55 package; a missing font uses the UI
font. Enhanced LRC word progress advances in pixels between word timestamps.

The page claims the linker's shared 3 MiB app_buffer with the owner `lyrics`.
It holds the WPS/old-track snapshot, the transition layer, the background and
the wrapped-line and glyph caches in this arena. It releases the claim before
a USB handler or another screen runs. Local lyrics use the 32 KiB
core allocation while the page is open. When another screen owns
app_buffer, entry/exit fade through the fallback colour in two halves, track
changes fade out/in in two 100 ms halves, and glyphs use interpolated colours.

The lyrics background uses the scrim theme's 320 px picture, shifted up by
40 px, with radius 12 on the decimated cover. The page copies a compatible
cached skin blur when available. On a miss it uses the same image filter
pipeline and keeps the result for the current track. The remote source uses
the WPS cover. Both paths add a dithered darkening pass and fade the top and
bottom edges. The artwork uses white lyrics. The colour mode setting supplies
the fallback when the cover or arena is unavailable.

The frame deadline subtracts render time and targets 33 fps (three ticks at
100 Hz). The page redraws and sends only changed bands when it is outside an
entry, exit or track transition. Idle waits cover the next lyric line, word
or progress-bar pixel, the manual-return deadline and a 500 ms source check.
A poll with no visible changes draws nothing.

`Debug > Lyrics perf` shows the last visit. Closing the page writes the
same lines to `/.rockbox/lyrics-perf.txt`. Frame times cover rendering and
LCD submission. The file reports sample counts and average, p95 and maximum
times per phase and per animation. P95 is a histogram upper bound. Wait is
time in the scheduler or input queue. The dropped count flags frames over the
30 fps budget for render time or for a gap between consecutive animation
frames. Entry latency starts at the dispatched WPS action and ends at the
first visible frame submission. Blur max is the longest cold background build.
Remote prefetch times start when WPS observes the track. A value of −1 marks
an unfinished milestone. The recorder samples CPU frequency, boost and audio
play state on submitted frames.

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
`lyrics_embedded_test.py` uses real audio containers to check embedded lyrics,
sidecar priority, encoding, truncation, and source file integrity.
