# Build an upstream plugin for AstraPod

The SDK uses AstraPod's native types and the upstream source names for
`plugin_start`, `enum plugin_status` and `rb->` calls. A module must match the
target, platform, API version and SDK fingerprint of the firmware that loads it.
Use the firmware and modules from the same build.

## Build and launch

Run these commands from the repository root.

```sh
./build-sim.sh
PATH=/home/wilf/.cache/pocket-stack/rockbox/bin:$PATH ./build-hw.sh ipod6g
```

Both scripts enable the SDK for iPod Classic. An ordinary configure build
takes the explicit flag `--rock-compat`.

```sh
mkdir build-rocks
cd build-rocks
../tools/configure --target=ipod6g --type=s --appsdir=apps-ipod --rock-compat
make rocks
```

Use `--type=n` with the ARM toolchain for hardware. Host modules are shared
objects. ARM modules use the DRAM region reserved for plugins.

Find the build outputs under `rock-compat/modules/`. For a custom package, run
`make zip`, `../bundle-theme.sh` and `../bundle-rocks.sh` in that order. The
bundle installs the selected modules under `.rockbox/rocks/`.

In the main menu, open **Plugins** and choose **Applications**, **Games** or
**Demos**. Choose an installed module. You can also select a matching `.rock`
file in **Files**. If you use a custom main-menu order, enable **Plugins** in
Customize Main Menu. For a simulator fixture, add
`root menu order: plugins,files,settings` to its `.rockbox/config.cfg`.

## Add a source

1. Add a category and basename to `apps-ipod/plugins/rock-modules.txt`. Use
   `apps`, `games` or `demos` as the category.
2. In `apps-ipod/plugins/plugins.make`, add dependencies for any upstream
   `lib/` sources or generated assets that the module uses. For native bitmaps,
   pass the bitmap basenames to the module's `rock_module_bitmaps` rule.
   The build selects the files from upstream's
   `bitmaps/native/SOURCES` and generates the headers and objects in the build
   directory.
3. Build with the SDK include path first. Missing `rb->` members are compile
   errors. Add reviewed bindings to `apps-ipod/plugins/sdk/rock_api.def`. Build
   both variants and check entry, exit and each native widget used by the source.

Keep upstream inputs under `apps/plugins/` intact. For a narrow source
conversion, stage the copy in the build directory. Match the expected upstream
text exactly and stop the build if it changes. The `ROCK_STAGED` list in
`apps-ipod/plugins/plugins.make` selects sources for these conversions.
Include runtime data in the bundle when the corresponding module is enabled.
For example, Snake 2 reads the unchanged `snake2.levels` next to its module.

## Phase 2 games and demos

The allowlist includes 2048, Amaze, Brickmania, Chopper, Maze, MazezaM,
Minesweeper, Pong, Snake, Snake 2, Star and Xobox. It also includes Fire,
Fireworks, Plasma and Starfield. The games and demos have separate allowlist
commits and fit the same ARM and SDL build. Starfield uses read-only mixer
peak queries. Fire and Plasma draw directly into the RGB565 framebuffer.

The allowlist also includes Blackjack, Bubbles, Clix, Invadrox, Jewels, Pegbox,
Rockblox, Solitaire, Spacerocks, Wormlet and Demystify. Their bitmap rules use
upstream's target selections and bitmap formats. Menus and option pickers use
the native theme. The games keep their upstream playfields and help screens.

Codebuster uses native menus and option pickers. Its local settings save
on normal exit. Super Domination uses the upstream board tiles and the native
inventory list. Rocklife starts with a built-in pattern.

VU Meter reads the firmware playback channel's peaks and the mixer output
rate. The title identifies stopped and paused playback, with the meters at
rest in either state. Select opens the native display options. Menu exits.
The meter has no playback controls, volume controls, or USB audio input.

## Native diagnostics

`rb_info` presents its diagnostics through native lists. The build
uses the configured target's action and context enums and button
definitions. Raw button capture includes the repeat and release
flags. Printcell data uses a row list and a column detail page.
The adapter uses the native list voice slots for names and values.

## File viewers

Files can open `.cells` with Rocklife, `.ssg` with Super Domination and
`.ch8` with CHIP-8. The loader passes the selected path to the module
for that call. CHIP-8 reads an optional same-stem `.c8k` file to remap
keys. The firmware routes these extensions through a fixed association
table. See [the plugin guide](../../apps-ipod/plugins/README.md) for keys.

## Oscilloscope

In **Plugins > Demos**, open **Oscilloscope**. It starts in Peaks on
the first launch. Press Select and choose **Graph mode** to switch
between Peaks and Waveform. Both modes use the native options for
draw style, orientation and volume. Use **Advance** and **Speed**
in Peaks. Use **Time span** for a 2–10 ms window in Waveform. Press
Menu from the graph to return.

Waveform shows one trace for each stereo channel. A short PCM buffer
can shorten the window. On pause, stop or unavailable playback
samples, the graph shows two flat traces.

The SDK supplies a foreground-only `waveform_read` call. Pass space
for `PCM_WAVEFORM_FRAMES` stereo frames. The result is the number of
complete interleaved 16-bit frames copied from the native playback
channel. A zero result means no samples are available for this read.

## Foreground Metronome

Metronome stops music on entry. Hold Select to start or tap Select to
set the tempo. Press Play/Pause to pause and Menu to return. Adjust the
click volume with the wheel. The change applies to the current launch.
If the timer is busy, Metronome stays paused and displays a retry message.

The SDK exposes an exclusive 2 ms tick counter and click playback on the
native playback channel. The firmware counts ticks and the module consumes
them on the foreground thread. The module fits its samples to the active
sink rate. It exits if the rate changes. USB takeover, accepted shutdown,
and the native playback or recording start release the resources. The
application browser starts Metronome without a tempo-map file.

## Pitch Detector

The ipod6g package includes Pitch Detector by default. Set
`ROCK_PITCH_DETECTOR=0` to omit it from both the build and the bundle.

Open **Plugins → Applications → pitch_detector**. On a Mikey-capable player,
the tuner selects the headset microphone when a plug is present. Remove the
plug before launch to use the passive dock line input. The tuner uses the
native gain for the selected input and leaves the recorder's source setting
alone. It analyzes the left channel at 22,050 Hz. When it cannot detect a
pitch, it displays **No pitch. Check mic or line input.** Removing the plug
during microphone capture stops the tuner and reports the unplug.

The tuner stops playback on launch. **Menu** opens
**Tuner Settings** and releases capture while that menu is open. **Return to
Tuner** starts a new capture. **Quit** returns to Applications. A USB consent
prompt ends the tuner session, including when you choose **Charge Only**.
SDL displays a fixed 445 Hz preview labelled **Preview - no audio capture**.

The SDK stages a digest-checked copy of the upstream module. Its tuner
slots request a foreground session, release capture, or copy one complete
PCM block. Firmware owns the DMA buffers and callbacks. The reservation
excludes the native recorder and USB Sound Card. A playback sink switch
releases the reservation before switching.

## Phase 3 playback metadata

`playing_time` uses the native playlist inspection and list APIs. The SDK
copies the current track's metadata under the playback lock. The copy
excludes the cue sheet and remains valid until the next metadata snapshot.
To parse another entry, pass its path and either `-1` or a file descriptor
owned by the plugin. The build stages the upstream source with millisecond and byte
precision for its average bitrate and playlist duration.

## SDK boundaries

- A plugin owns the UI until its entry returns.
  The loader holds the app scratch lease from load through unload.
  `plugin_get_buffer` exposes the aligned tail. `plugin_reserve_buffer`
  reserves space until exit and returns the remaining size. Use
  `plugin_claim_scratch(minimum, preferred, &granted)` to reserve aligned
  storage and get its address in one call. It returns NULL and zero bytes
  if the remaining tail is smaller than the minimum. Reserve before
  retaining a pointer from `plugin_get_buffer`. The arena also holds the
  caller's framebuffer. The codec and playback pools keep their own storage.
- Use the native menu, list, yes/no, keyboard, option and splash slots to
  present AstraPod widgets. A list keeps its native layout and selection
  across draw and action calls. A raw LCD slot releases the list before
  drawing. Call `gui_synclist_scroll_stop` before drawing directly through
  `screens[]`. A modal widget returns to the saved raw pixels and viewport.
  Stopwatch can request native status chrome above its own viewport.
- The keyboard uses AstraPod's click-wheel editor. On cancel or USB it
  leaves the buffer unchanged and returns `-1`. A plugin can check
  `plugin_usb_taken` after a keyboard returns. A yes/no prompt reports
  `YESNO_USB`. On plugin exit, the core releases list and scroll state,
  restores the caller's viewport and status title, and repaints its page.
- Use the SDK file and directory slots. The runtime closes owned streams on
  exit and before the core hands storage to USB. Once that handoff starts,
  file and directory opens fail until the next plugin launch. Return
  `PLUGIN_USB_CONNECTED` when the event handler reports it. A plugin may use
  its CRT exit callback to save before the storage handoff.
- The compatibility generator can supply `rock_compat_tables.h` and
  `rock_compat_sync.h` in the build's `rock-compat/` directory. The SDK uses the
  generated settings view and language table when present. The runtime flushes
  settings changes before saving, entering native widgets and exiting. Generated
  fallback IDs display as text without a native voice ID. A standalone build uses
  the native members and language IDs exercised by the enabled modules.

See `apps-ipod/plugins/rock-modules.txt` for the installed applications, games
and demos. Worker threads, timer callbacks, resident plugins, overlays, codec IRAM,
audio-buffer leases and stock binary modules
are outside the current SDK. Cross-plugin dispatch is also outside this
build. Load modules built from trusted sources. Plugins
execute with firmware privileges, and the host dynamic loader can run module
initializers before the SDK header check.

## PodLink

The `rb->podlink_*` slots are available in the native SDK. The
[APL1 wire reference](../podlink.md#plugin-channel-extension-apl1) defines
the channel and transfer limits. On SDL, commands return
`-ROCK_PODLINK_UNSUPPORTED` and close is a no-op.

| Slot | Foreground use |
| --- | --- |
| `podlink_open(name)` | Queue one named channel on the established resident link. Wait for `ROCK_PODLINK_OPENED`. |
| `podlink_close()` | Invalidate the channel and release its queued work. The loader also calls it on module exit. |
| `podlink_send(binary, data, length)` | Queue one complete message. Use `false` for UTF-8 and `true` for binary. Copy up to 2,048 bytes. |
| `podlink_poll(event, timeout_ms, progress, context)` | Copy the next event into your `struct rock_podlink_event`. Pass a callback or `NULL`. |
| `podlink_push(path, object)` | Queue a file from an absolute device path to a named desktop object. |
| `podlink_pull(path, object)` | Queue a download from a named desktop object to an absolute device path. |
| `podlink_cancel()` | Abort the active transfer before the receiver commits it. Wait for terminal completion. |

Commands return zero when queued or a negative result from
`enum rock_podlink_result`. On `-ROCK_PODLINK_BUSY`, poll and retry.
Poll regularly to drain the receive mailbox. If a second incoming message
overflows it, the channel closes with `ROCK_PODLINK_BUSY`.
The SDK owns copies of accepted names, paths and payloads. The file worker
owns file handles and temporary files. Close invalidates the channel
immediately, and the worker closes owned handles before acknowledging a
USB storage handoff.

Poll returns a positive `enum rock_podlink_event_type`, zero if pending,
or a negative error. It never sleeps. A nonzero timeout starts a deadline
the first time a poll finds no event. Continue polling with a nonzero timeout
until an event or `-ROCK_PODLINK_TIMEOUT`. Polling with zero clears that
deadline. The poll timeout does not cancel a transfer. An optional callback
runs inside that same poll call for `ROCK_PODLINK_PROGRESS` and
`ROCK_PODLINK_COMPLETE`. Progress events can coalesce. The caller owns the
event buffer and callback context. Test `event.result` on completion or
close. Read text using `event.length`. The text is not NUL-terminated.
For a completed transfer, `event.bytes`, `event.total` and `event.crc32`
describe the file.

### Run PodLink Echo

1. Install the firmware and `podlink_echo.rock` from the same ipod6g package.
2. In Disk Mode, create the fixture on the player volume. Replace
   `DEVICE_ROOT` with the volume mount path.

   ```sh
   python3 - 'DEVICE_ROOT' <<'PY'
   from pathlib import Path
   import sys
   path = Path(sys.argv[1]) / ".rockbox/podlink-echo.in"
   path.write_bytes(bytes(i % 251 for i in range(5000)))
   PY
   ```

3. Eject Disk Mode. Start HerdRemote and enable a desktop companion
   registered for `astrapod.echo`. Set its working directory to a writable
   folder. Connect the iPod with **USB Mode** set to **Mass Storage**.
   At **Computer connected**, press **Menu** to leave the resident PodLink
   session available. **Select** hands storage to the computer.
4. Select **Plugins > Demos > podlink_echo**. The module automatically sends
   `AstraPod echo` and the binary bytes `00 01 ff 80 00 7f`. The companion
   echoes both messages. The module pushes the fixture to `from-ipod.bin`
   and pulls that object back to `/.rockbox/podlink-echo.out`.
5. Read the result. **All checks passed** displays
   **5000 bytes, CRC c1607408**. **Check failed** displays the reason.
   Press **Menu** to cancel and exit or to leave the result screen.

### Run the offline peer fixture

From the repository root, run `sh tools/tests/run_podlink.sh`. The driver
builds `build-fu49/host/podlink_apl`, or use
`${PODLINK_TEST_BUILD}/host/podlink_apl` with a custom build directory.
Run the binary without arguments to test valid and rejected transfers.
The fixture uses the real SDK, PLNK codec and resident service with a
simulated FAST IN controller.

To connect an external peer, create a temporary volume directory with
`.rockbox/` and use the fixture-generation command above. Run
`build-fu49/host/podlink_apl --peer DEVICE_ROOT`. Read one raw APL1 payload
per stdout line and write replies to stdin. Each line is a hex string
without spaces or PLNK wrapping. The driver supplies PLNK framing and its
CRC internally. The first output is OPEN for `astrapod.echo`. The driver
follows the same echo and file sequence as the device module. Its result
goes to stderr. Exit status 0 confirms the downloaded file matches the
fixture byte for byte.

## Dictionary and View Text

Open **Plugins > Applications > dict** and enter an English word or
phrase. Use the wheel to scroll through the WordNet senses and press
Menu to return to Applications. The build packages Princeton WordNet 3.0
under the WordNet redistribution license. The license is in
`/.rockbox/rocks/apps/dict.LICENSE`. For an offline build, set
`WORDNET_ARCHIVE` to a local copy of the Princeton archive. The packager
verifies its SHA-256 before reading it.

Open **Plugins > Applications > view_text** and select a UTF-8 `.txt`
file up to 32 KiB. View Text displays the filename and text in the native
text box. Press Menu to return to Applications. The Files browser still
uses the native document viewer when you open a text file there.

## File picker and module handoff

Call `rb->pick_file` with a path buffer of at least `MAX_PATH`, a start
directory and a title. Use a comma-separated list such as `.txt,.cfg`
to filter by extension. Matching ignores ASCII case. The call returns a
positive value after selection, zero on cancellation, and a negative
value on error or storage takeover.

Call `rb->queue_handoff` to open another module. Return
`PLUGIN_GOTO_PLUGIN` after a successful request. The loader copies the
paths and releases the current module before opening the successor.
A launch permits one handoff. Native viewers start after the plugin
arena has been released.

## Foreground overlays

Compile the overlay CRT with `ROCK_OVERLAY_IMAGE`. Install the result as
`/.rockbox/rocks/overlays/<name>.ovl`. Call `rb->overlay_request` from a
launcher and return `PLUGIN_CHAIN_OVERLAY` after success. Start the name
with a lowercase ASCII letter or digit. Use up to 32 lowercase letters,
digits and underscores. Supply a path of up to `MAX_PATH - 1` bytes or NULL.

The launcher unloads before the overlay starts. The overlay uses the
same DRAM arena and fingerprint with a distinct header magic. It cannot
start a second overlay. The linker rejects plugin and overlay IRAM sections.

## USB Remote Control and resident services

Open **remote_control** with USB unplugged, then connect to the computer
to control. Select a command from Desktop, Presentation or Media Player.
Menu stops new reports. Unplug before returning to ordinary PodLink or
Disk Mode.

Battery Benchmark writes `/battery_bench.txt`. Start and stop logging
from Applications. Logging pauses during Disk Mode and resumes after
eject. Announce Status speaks during the current session. Enable Voice
Menus for speech. The native track announcement takes precedence.
Both services release the module on exit.

## File and playback tools

Use the Files context menu to open Lrcplayer, export CUE, edit or sort
plain text, speed-read, convert WAV, repair an MP3 index, or edit a bitmap.
Ordinary Select retains the native file action.

Open Lrcplayer from Applications while playing a track with a timed
sidecar. The native lyrics player handles embedded lyrics. CUE export
picks an unused sibling filename.

The Lua viewer opens the packaged BoomShine, Picross and Pixel Painter
scripts from Games.

## Desktop companion

Set `device_channels` in the desktop manifest to the channel names
requested by the device. The host routes the first registration for each
name and releases it when its companion exits. The bundled Echo companion
uses `astrapod.echo`. See the desktop Input Plugins guide for setup.

## Workers, timers and audio

Create up to two workers with `rock_worker_start(run, context, stack, bytes)`.
The returned ID is zero on failure. Give each worker a non-overlapping
long-aligned stack that stays valid until `rock_worker_join(id)` returns.
Workers run on the foreground core. Use them for computation over owned
memory and call `rock_worker_stopping()` in each loop and each retry.
Return when it becomes true. Let the foreground thread own the UI, file
handles and PCM calls. Workers may sleep or yield. A worker must not call
the plugin CRT's `exit` or wait for the foreground thread during shutdown.
If the foreground joins a worker early, signal the worker's own finish
condition first. The loader requests stop and joins outstanding workers
on every exit and before USB storage takeover.

Use `rock_timer_start(cycles, callback, context)` to claim the hardware
timer when available. The firmware counts ticks. Call
`rock_timer_dispatch(limit)` from the main loop to deliver up to that
many ticks. Dispatch invokes the callback on the foreground thread.
SDL rounds the requested period up to whole milliseconds. Applications
that need interrupt latency must use a firmware service. Call
`rock_timer_stop()` to discard queued ticks and release the timer.
Callbacks may stop their timer. Keep callback execution short.

Use `rock_stream_open()` for copied PCM playback. It returns the built-in
sink's rate or zero. Stop native playback before launching a player
that requires PCM. Modules can fall back to silent operation if the
sink is busy. Submit whole stereo signed 16-bit frames in native byte
order. Each submission accepts up to `ROCK_STREAM_BLOCK_BYTES` bytes.
The queue has `ROCK_STREAM_QUEUE_SIZE` blocks. Reuse the source buffer
after `rock_stream_submit()` returns. Poll `rock_stream_pending()` for
the count, including the current block, and wait outside the callback.
A negative count indicates that playback is unavailable. Native playback,
recording, USB takeover and sink changes revoke the stream. Pause with
`rock_stream_pause(true)` and resume with `rock_stream_pause(false)`.
The PCM mixer can finish its current output frame after pause or close.
Call `rock_stream_close()` to discard the queue. The loader also revokes
it before unload. The mixer calls firmware callbacks only.

The alternate CRT built with `ROCK_REQUEST_LARGE_STACK` requests an
80 KiB foreground stack. It runs the entry and CRT cleanup on the same
stack and rejoins the caller before unload. This profile is for modules
such as Solo that have deep synchronous call stacks. It uses the same
UI and lifecycle APIs. Use a standard-stack launcher for an overlay
handoff.

## Tracker and MIDI playback

Open **Plugins > Applications > mikmod** and choose an uncompressed
four-channel ProTracker `M.K.` MOD of up to 512 KiB. Use Play to pause
or resume, Left or Right to change position, and Menu to return.
Selecting a MOD in Files still opens it in the native music player.

Open **Plugins > Applications > midi** and choose a Standard MIDI File.
The player accepts format 0 and 1 with ticks per quarter note, up to
32 tracks and 512 KiB. It accepts tempo changes, running status, notes,
program changes and channel volume. The built-in instruments use four
original synthesized waveforms and a synthesized noise drum. The
instruments ship under GPL-2.0-or-later with the player source. The
player does not require a Gravis patch set. Use Play to pause or resume
and Menu to return.

## RockDoom

The ipod6g package includes RockDoom by default. Set `ROCK_DOOM=0` to
omit both its launcher and overlay. Place
`rockdoom.wad` and a compatible base WAD, such as `doom1.wad`, in
`/.rockbox/doom/`. Open **Plugins > Games > doom**. If either file is
missing, the launcher displays the data directory and waits for Menu.
Supply the WAD files separately. The archive contains the launcher and
DRAM overlay.

The module mixes sound effects and DMX MUS tracks into the same foreground
PCM queue. **Options > Sound** controls that queue. MIDI-format music lumps
are silent. The module rejects embedded DEHACKED patches. Freedoom 0.13.0
permits redistribution under BSD-3-Clause, but its IWAD uses an embedded
DEHACKED patch and MIDI-format tracks. It stays out of the archive.

## Key Remap

Open **Plugins > Applications > keyremap** to edit Left and Right on
the iPod or its remote while the Now Playing screen is open. Changes
affect short taps. The editor offers previous track, next track,
play/pause and volume controls. **Save for next boot** replaces
`/.rockbox/keyremap.kmf` after writing and syncing a temporary file.
Restart to apply it. **Restore defaults** followed by **Save for next boot**
clears the overrides. The editor preserves keymaps created by other tools.
To recover from an external mapping, back up and remove that file over
USB storage and restart.

The Lua viewer also accepts **print_buttons.lua** and **splashscroller.lua**
under Demos, and **return2WPS.lua** under Applications. The button viewer uses
named button constants and the scroller uses the native text viewer. Exit
the button viewer with Menu.

## H.264 video on iPod Classic

Open **Plugins > Applications > h264_player** and choose an MP4 or M4V.
Use progressive H.264 Baseline up to Level 3.0, at most 30 fps and a
maximum of 640×480. The video tables and decoder must fit the available
plugin scratch space. A 320×240 clip leaves room for the index. Use Play
to pause or resume, Left or Right to seek to a keyframe, and Menu to
return to Applications. USB storage stops the decoder before the plugin
releases its workspace.

The VPU backend is available on ipod6g. The SDL entry reports that the
decoder is unavailable. This player displays video only. It leaves the
native audio and codec thread available for music.

## Rockboy

Open **Plugins > Games > rockboy** and choose an uncompressed GB or GBC
ROM. A ROM must match the size in its header and fit plugin scratch.
The current maximum ROM size is 2 MiB. Game audio uses the built-in
PCM stream when available. Menu opens Resume and Quit. Use the wheel
for Up and Down, Left and Right to move, Select for A, Play for Start,
Select+Left for B and Select+Right for Select. This build keeps
cartridge RAM in memory for the game session.

## ZXBox and Lua examples

Open **Plugins > Games > zxbox** and choose a 48K `.sna` snapshot.
Use the wheel, Left, Right and Select as Kempston joystick input.
Menu opens Resume and Quit. Snapshot saves and tape formats are
outside this adapter. A busy built-in sink leaves the game silent.

The Lua viewer installs Hello World and Viewport Test in Demos.
Select either script from its plugin directory. Lua Stopwatch appears
in Applications. It uses the wheel player's physical buttons and
stores laps in `stopwatch-lua.dat`, separate from the native stopwatch.
