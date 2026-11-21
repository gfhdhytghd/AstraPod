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
   `lib/` sources or generated assets that the module uses. Existing examples
   build `pluginlib_actions.c`, `configfile.c` and `playback_control.c`.
3. Build with the SDK include path first. Missing `rb->` members are compile
   errors. Add reviewed bindings to `apps-ipod/plugins/sdk/rock_api.def`. Build
   both variants and check entry, exit and each native widget used by the source.

Keep upstream inputs under `apps/plugins/` intact. For a narrow source
conversion, stage the copy in the build directory. Match the expected upstream
text exactly and stop the build if it changes. `tools/rock-compat/stage-usb.py`
stages the USB unwind for `stopwatch` and `dice`. It also initializes Dice's
first menu selection and leaves room for the roll string's terminator. Snow builds directly from its upstream source.

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
  the native members and language IDs exercised by the three enabled modules.

The allowlist also covers Chess Clock, Countdown Timer and Test Kbd.
Chess Clock links the existing Playback Control helper. Countdown Timer links
Pluginlib Actions and uses the generated countdown language strings and the
native beep. Additional modules need coverage for the slots they use. Worker threads, timers, resident
plugins, overlays, codec IRAM, audio-buffer leases and stock binary modules
are outside the current SDK. Load modules built from trusted sources. Plugins
execute with firmware privileges, and the host dynamic loader can run module
initializers before the SDK header check.
