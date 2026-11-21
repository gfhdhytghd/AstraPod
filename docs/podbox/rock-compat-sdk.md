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

Remaining entries from the ipod6g Phase 2 source inventory are listed below.
A generated bitmap dependency refers to an existing upstream asset that the
AstraPod build still needs to select and compile. The behavior checks apply
after the dependencies build.

| Entry | Build dependency or behavior to verify |
| --- | --- |
| `blackjack` | Generate `card_deck` and `card_back`. Verify dealing, help and the high-score exit. |
| `bubbles` | Generate `bubbles_background`, `bubbles_bubble` and `bubbles_emblem`. Link fixed-point and XLCD drawing helpers. |
| `chip8` | Route a selected `.ch8` file to the entry point. Verify the optional `.c8k` key mapping and emulator exit. |
| `clix` | Link high-score, help and playback helpers. Verify wheel selection and exit from the score screen. |
| `codebuster` | Supply a native `set_bool` adapter or convert its two boolean options. Verify settings save and USB exit from the nested menu. |
| `demystify` | Link action and backlight helpers. Verify trail and shape controls, then exit from the draw loop. |
| `invadrox` | Generate the target-selected alien, ship, projectile, shield, UFO and score sprites. Test viewport swaps and hold-triggered pause. |
| `jewels` | Generate `jewels`. Verify cursor input, tile swaps and save restoration. |
| `oscilloscope` | Define PCM lock and mixer hook ownership, including callback teardown. Add OSD viewport buffer adapters and native volume controls. |
| `pegbox` | Generate `pegbox_header` and `pegbox_pieces`. Verify level selection, board controls and help exit. |
| `rb_info` | Map `talk_id`, `talk_spell` and `strcasestr`. Link the printcell, action and button helpers. Verify spoken rows and their native IDs. |
| `rockblox` | Link high-score, help, playback and backlight helpers. Verify the iPod wheel, soft poweroff and resume-file lifecycle. |
| `rocklife` | Verify standalone pattern controls. A `.cells` file also needs viewer routing and plugin entry parameters. |
| `solitaire` | Generate `card_deck`, `card_back` and `solitaire_suitsi`. Verify stack selection, deals and configuration persistence. |
| `spacerocks` | Link high-score, help, playback and backlight helpers. Verify held controls, pause and game-over exit. |
| `superdom` | Generate `superdom_boarditems`. Map boolean settings and the inventory simplelist to native widgets. Review `.ssg` save-file entry parameters. |
| `vu_meter` | Map boolean options, `mixer_get_frequency` and native volume bounds and updates. Verify the peak display with playback running. |
| `wormlet` | Link configuration, backlight and playback helpers. Verify the iPod player controls and the game-over exit. |

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
and demos. Worker threads, timers, resident plugins, overlays, codec IRAM,
audio-buffer leases and stock binary modules
are outside the current SDK. Load modules built from trusted sources. Plugins
execute with firmware privileges, and the host dynamic loader can run module
initializers before the SDK header check.
