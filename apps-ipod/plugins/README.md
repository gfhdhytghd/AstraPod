# AstraPod plugins

Build and package the supported upstream sources with the
[AstraPod plugin SDK guide](../../docs/podbox/rock-compat-sdk.md).

| Path | Purpose |
| --- | --- |
| `sdk/` | Source-facing header, module format and API slots |
| `rock-modules.txt` | Allowlist and installation paths |
| `rock_runtime.c`, `rock_files.c` | Foreground loader and ownership |
| `rock_browser.c` | Category picker backed by the native file browser |
| `plugin.lds` | Native code and codec layout, selected by flags |
| `credits.pl` | Generator for the native credits screen |

The zip packager reads Rockbox's category and viewer manifests from
`apps/plugins/`. The AstraPod bundle script installs its own allowlist after
the theme bundle clears the zip's stock plugin payload.

## Open a plugin file

Select a `.cells` pattern in Files to open it in Rocklife. Select a
`.ssg` save in Files to resume it in Super Domination. Files in the
installed plugin folders also appear in the Plugins category browser.
Selecting the `.rock` module itself starts it without a file.

Select a `.ch8` program in Files to open CHIP-8. Press Menu to return.
Left, Play/Pause and Right map to 4, 5 and 6. Wheel back and forward
send pulses for 2 and 8. Put a same-stem `.c8k` beside the program to
remap the 16 CHIP-8 keys. The first 16 characters give the physical
key mappings in hexadecimal order from 0 to F. A missing or short
sidecar uses the default mapping.

## Rockbox Info

Open Plugins → Applications → rb_info. The diagnostic pages show
paths, buffers, buttons, actions and contexts in native lists. In
Action test, select a context and then select Start test. The test
captures input until four seconds of inactivity. In Printcell test,
select a sample row to read its eight columns in a native list.

## Alarm Clock, Calendar and Shopper

Start or pause a track, then open **Plugins > Applications > alarmclock**.
The initial time is the next minute. Left and Right select the hour or
minute, the wheel changes it, and Select starts the countdown. Menu cancels
and returns to Applications with playback paused. At the alarm time, the
plugin resumes playback and opens Now Playing.

Open **Plugins > Applications > calendar**. Left and Right move by one day.
The wheel moves by one week. Select opens the events or the Edit menu for
the highlighted day. Events save in `PLUGIN_APPS_DATA_DIR/.memo`. Use Menu
to close Events and then return to Applications.

Open a `.shopper` file in Files. Use `#` for category lines, a space for
unmarked items, or `!` for marked items. Select toggles an item. Hold Select
to open Options and switch to the shopping list. Left saves and returns to
Files. To leave the file unchanged, choose **Quit without saving**.

## Phase 2 utility candidates

Run the candidate subsets in an ipod6g ARM or SDL build configured with
`--appsdir=apps-ipod --rock-compat`.

```sh
make rocks ROCK_MODULES='apps/db_folder_select apps/dict'
make rocks ROCK_MODULES='demos/credits apps/resistor'
make rocks ROCK_MODULES='apps/dart_scorer'
```

These overrides select build outputs. Packaging uses `rock-modules.txt`.
Build Dict separately when you have its data files. Use the application and
modules from the same build.

| Upstream source | Candidate behavior or integration dependency |
| --- | --- |
| `db_folder_select` | Native folder picker. The Applications entry edits the native database scan paths and saves on confirmation. The picker borrows scratch from the active plugin lease. |
| `dict` | Native keyboard and scrollable text box. Supply `dict.index` and `dict.desc` under `.rockbox/rocks/apps/`. The index uses a 32-byte word and a big-endian 32-bit description offset, matching output from the upstream converter built with 32-bit `long`. |
| `credits` | Upstream credits animation and target logo. The build generates the names from `docs/CREDITS` with the upstream script. |
| `resistor` | Native menus and keyboard, with the upstream resistor bitmap on the result screen. |
| `dart_scorer` | Native menus and confirmations, with the upstream score screen and save format. The save uses a sibling temporary file under the games data directory and renames it after closing. |
| `keybox` | Held. The stock parser trusts the saved byte count and uses unbounded NUL searches. Short or malformed saves need a bounded parser and a verified persistence path. |
| `view_text` | Held as a standalone module. Its parameter is a title/text pointer pair. The browser passes a null parameter, and the SDK has no typed plugin-to-plugin dispatch. `dict` calls the native text box via the SDK. |
| `md5sum` | Held. The root scan recurses with one open directory and one path buffer per level. The SDK caps directory streams at four, so the stock scan would silently omit deeper paths. |
| `disktidy` | Held. Recursive deletion needs a reviewed path and traversal contract. The SDK lacks `rmdir` and owned extended event callbacks for its statistics screen. Its stock rule resource is `apps/plugins/disktidy.config`. |
| `remote_control` | Held. It needs `usb_hid_send` and an active USB HID session while the plugin is foreground. The SDK exits foreground plugins during USB storage takeover. |

## Storage stats and playlist search

In **Plugins > Applications**, open **stats** to count files from the storage
root. The totals classify audio, playlists, images and videos by extension.
They include hidden folders and report the total bytes and largest folder by
file count. Press Menu to cancel. If the scan cannot read a folder or fit its
path, it reports an incomplete scan. Wide folders with many subfolders take
longer because the scan reopens the parent after each child.

In **Files**, hold Select on an M3U or M3U8 file and choose **Search in
Playlist**. Enter part of a track path in the keyboard. The search ignores
ASCII letter case and matches non-ASCII text exactly. A normal Select opens
the playlist in the native player. Search skips comments and saves only
matching paths. The result appears beside the source as
`search_result-1.m3u` or `search_result-1.m3u8`. A later search uses the next
free number. The result keeps the source extension, any UTF-8 BOM and the
original path bytes, including relative paths.

Press Menu to cancel. A failed or cancelled search leaves the source and
earlier results available. If abrupt power loss leaves a hidden
`.search_result-N.tmp` file, you can delete it.

## Settings reference and WAV waveform

Open **Plugins > Applications > settings_dumper** and confirm the export.
The report lists setting names and accepted values in
`/settings_dumper.txt`. Native theme settings appear under Other settings.
The export reads the schema and leaves the live settings alone.

Hold Select on a WAV file in Files and choose **View waveform**. WAVView
reads mono and stereo 16-bit PCM. Turn the wheel to zoom, press Left or
Right to pan, and press Play for help. Menu returns to the file. Select
from Files still plays the WAV through the native player.
