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

## Phase 2 utility candidates

Run the candidate subsets in an ipod6g ARM or SDL build configured with
`--appsdir=apps-ipod --rock-compat`.

```sh
make rocks ROCK_MODULES='apps/db_folder_select apps/dict'
make rocks ROCK_MODULES='demos/credits apps/resistor apps/alarmclock'
make rocks ROCK_MODULES='apps/dart_scorer'
```

These overrides select build outputs. Packaging uses `rock-modules.txt`.
The bundle includes the folder picker, credits, resistor, and dart scorer.
Build Dict separately when you have its data files. Alarm Clock remains a
validation build while its timed resume path is checked. Use the application
and modules from the same build.

| Upstream source | Candidate behavior or integration dependency |
| --- | --- |
| `db_folder_select` | Native folder picker. The Applications entry edits the native database scan paths and saves on confirmation. The picker borrows scratch from the active plugin lease. |
| `dict` | Native keyboard and scrollable text box. Supply `dict.index` and `dict.desc` under `.rockbox/rocks/apps/`. The index uses a 32-byte word and a big-endian 32-bit description offset, matching output from the upstream converter built with 32-bit `long`. |
| `credits` | Upstream credits animation and target logo. The build generates the names from `docs/CREDITS` with the upstream script. |
| `resistor` | Native menus and keyboard, with the upstream resistor bitmap on the result screen. |
| `alarmclock` | Foreground countdown using the native clock and playback resume state. Start or pause a track before opening it. |
| `dart_scorer` | Native menus and confirmations, with the upstream score screen and save format. The save uses a sibling temporary file under the games data directory and renames it after closing. |
| `keybox` | Held. The stock parser trusts the saved byte count and uses unbounded NUL searches. Short or malformed saves need a bounded parser and a verified persistence path. |
| `rb_info` | Held. The stock source includes private `open_plugin.h`. Its printcell helper draws directly into stock list geometry and overrides list-separator settings. It needs a native table adapter. |
| `view_text` | Held as a standalone module. Its parameter is a title/text pointer pair. The browser passes a null parameter, and the SDK has no typed plugin-to-plugin dispatch. `dict` calls the native text box via the SDK. |
| `md5sum` | Held. The root scan recurses with one open directory and one path buffer per level. The SDK caps directory streams at four, so the stock scan would silently omit deeper paths. |
| `disktidy` | Held. Recursive deletion needs a reviewed path and traversal contract. The SDK lacks `rmdir` and owned extended event callbacks for its statistics screen. Its stock rule resource is `apps/plugins/disktidy.config`. |
| `shopper` | Held. The stock viewer expects a `.shopper` path parameter and writes back to that path. It needs an explicit file association and a checked save path. |
| `remote_control` | Held. It needs `usb_hid_send` and an active USB HID session while the plugin is foreground. The SDK exits foreground plugins during USB storage takeover. |
