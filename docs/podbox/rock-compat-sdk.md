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

## Pitch Detector candidate

Set `ROCK_PITCH_DETECTOR=1` for both the build and the bundle. The default
allowlist excludes the tuner pending capture qualification on ipod6g.

```sh
ROCK_PITCH_DETECTOR=1 ./build-sim.sh
ROCK_PITCH_DETECTOR=1 PATH=/home/wilf/.cache/pocket-stack/rockbox/bin:$PATH ./build-hw.sh ipod6g
```

Open **Plugins → Applications → pitch_detector**. On a player, select the
analog source and gain in the native recording settings first. Use a passive
dock line input or a supported headset microphone. The tuner analyzes the
left channel at 22,050 Hz. It stops playback on launch. **Menu** opens
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

- A plugin enters from the UI thread and returns before another plugin launches.
  The loader holds the app scratch lease from load through unload.
  `plugin_get_buffer` exposes the aligned tail. `plugin_reserve_buffer`
  reserves space until exit and returns the remaining size. The arena also holds a copy of the
  caller's framebuffer. Codec IRAM and the playback pool remain available.
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
