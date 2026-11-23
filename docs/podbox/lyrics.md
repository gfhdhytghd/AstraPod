# Animated Now Playing lyrics (FU57)

SELECT opens the same lyrics page for local playback, Rufin and Cider.
Holding the dock Apple Remote Play/Pause for 300 ms toggles the page in
either direction. A dock tap pauses or resumes. The click-wheel
Play hold still opens the WPS context menu. MENU returns
to Now Playing too, including entry through a context menu. The previous local
SELECT action, including the A/B marker key, is reachable with SELECT+PLAY.
Cider A/B markers also use SELECT+PLAY. The local setting still stores its
previous action under `wps select action` for configuration compatibility. The English settings
label is `SELECT+PLAY Action`.

The wheel follows list direction (clockwise advances); remote + goes to the
previous line and - to the next line. Remote reports always move one line;
fast wheel bursts accelerate to four lines per event. Timed lyrics return to
Auto four seconds after the last scroll. During that interval, SELECT seeks
to a different focused line and returns to Auto. SELECT on the current line
or in untimed lyrics returns to Now Playing. The seek accounts for the
configured lead so the target line is current at the destination.

The headphone inline remote keeps its Now Playing controls in Lyrics.
The centre button toggles playback. With Remote Track Skip on, two or
three centre clicks select next or previous. The volume buttons adjust
volume. The dock remote's +/− keys continue to scroll lyrics. Click-wheel
skip uses the WPS skip policy and held seek uses the WPS seek loop.

`Lyrics lead` defaults to +500 ms. It accepts −1000 to +1000 ms in 50 ms
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

Entry submits a loading frame using the built-in font before opening fonts
and lyrics or preparing cover art. Once ready, the lyrics slide in for
220 ms. Exit slides back over the live WPS snapshot in 180 ms. All scroll
targets take 260 ms from the displayed
position, including reversal, multi-line acceleration and idle return. Bitmap
18/26 px glyphs cross-fade through a separate mask while their line heights
interpolate. Track changes cross-fade for 200 ms. The 18/26 px Noto fonts,
including CJK, ship in the package. A missing font uses the UI
font. Enhanced LRC word progress advances in pixels between word timestamps.

The page claims the linker's shared 3 MiB app_buffer with the owner `lyrics`.
It holds the WPS/old-track snapshot, the transition layer, the background and
the wrapped-line and glyph caches in this arena. It releases the claim before
a USB handler or another screen runs. Local lyrics use the 32 KiB
core allocation while the page is open. When another screen owns
app_buffer, the ready frame replaces the loading frame directly. Exit fades
through the fallback colour in two halves, track changes fade out/in in two
100 ms halves, and glyphs use interpolated colours.

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

`Debug > Lyrics perf` shows the latest visit and the number of visits kept.
Closing the page appends a session to `/.rockbox/lyrics-perf.txt`. The header
records the session number, uptime at entry, source, timing mode and time
spent on the page. Source and timing mode describe the track at exit.
When the active file has little room for another session, it replaces
`/.rockbox/lyrics-perf.1.txt`. The two files use at most 64 KiB together.
Frame times cover rendering and
LCD submission. The file reports sample counts and average, p95 and maximum
times per phase and per animation. P95 is a histogram upper bound. Wait is
time in the scheduler or input queue. The dropped count flags frames over the
30 fps budget for render time or for a gap between consecutive animation
frames. Entry latency starts at the dispatched WPS action and ends at the
first visible frame submission. Blur max is the longest cold background build.
Entry setup excludes drawing the first frame. The recorder also reports
individual entry times for font loading, the lyrics document and cover art.
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
