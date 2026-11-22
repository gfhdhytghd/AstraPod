# Executable TSR modules on ipod6g

From `plugin_start`, call `rb->rock_tsr_register(callback, period)` once
with a positive period in Rockbox ticks. Return `PLUGIN_OK` to request
residency. Registration returns false outside a TSR entry, from another
thread, or when the callback does not belong to the loaded image.

The callback receives `ROCK_TSR_TICK` on a firmware-owned thread once per
period after the preceding callback finishes. Keep callback state in the
module's data or BSS. The file parameter, foreground stack, plugin scratch,
UI, file handles, timers, PCM and foreground workers belong to the entry
session and expire when it returns. A callback must finish promptly. It
may use its own data, pure SDK helpers and bounded waits. Keep async device
APIs and calls that depend on the foreground session in `plugin_start`.
In particular, CRT `exit` and `atexit` belong to the entry session.

| Terminal event | When it arrives |
| --- | --- |
| `ROCK_TSR_REPLACED` | Another `.tsr.rock` launch, including a relaunch of the same file. |
| `ROCK_TSR_USB` | Storage handoff after USB consent. |
| `ROCK_TSR_POWER` | Accepted orderly poweroff or reboot. |
| `ROCK_TSR_ABORT` | The entry returned an error or redirected, or the worker could not start. |

An active TSR receives one terminal event after the last admitted tick on
the same thread. Its return acknowledges cleanup. Firmware joins that thread
and closes the image before proceeding with USB acknowledgement, orderly
shutdown or replacement. A canceled registration never runs ticks. If its
entry returns, the terminal call runs on the UI thread before image closure.

A charge-only USB session leaves an active TSR running. A regular plugin
or overlay can run while the TSR keeps its slot. Launching a new TSR takes
the single slot before attempting to load its image, so even a rejected
replacement stops the old instance.

## Building a module

Use `rock_link_tsr` in [tsr-compat.make](../tsr-compat.make) with the
module path stem and its object prerequisites. Keep the `.tsr.rock`
suffix. This rule selects the TSR CRT and header magic. ARM images use a
64 KiB DRAM region that includes code, data and BSS. Firmware reserves the
region even when the slot is empty. Link images against the matching
firmware ELF and compile callbacks in ARM mode. The runtime checks the
address independently of the SDK fingerprint. SDL retains the shared
object handle until stop.

The SDK touch points are [plugin.h](plugin.h), [rock_api.def](rock_api.def)
and the typed interface in [rock_tsr.h](../rock_tsr.h). Rebuild all modules
with the merged SDK fingerprint. The loader uses the storage hook in
[rock_runtime.c](../rock_runtime.c) and the orderly power hook in
[shutdown.c](../../system/shutdown.c). Battery Benchmark and Announce
Status still use the firmware services in
[rock_resident.c](../rock_resident.c).

The focused probe images are outside the default package. Configure with
`--appsdir=apps-ipod --target=ipod6g --rock-compat`. Use `--type=n` for ARM
or `--type=s` for SDL. Build both paths explicitly from the build directory.

```sh
make "$PWD/rock-compat/modules/demos/tsr_probe.tsr.rock" \
     "$PWD/rock-compat/modules/demos/tsr_foreground_probe.rock"
```

For ARM, check the resident image's `plugin_start_addr` against the
firmware ELF's `rock_tsr_arena`, and check `plugin_end_addr` against
the symbol size. The foreground image must start at `pluginbuf`.
For SDL, also build `rockboxui`. Run `python3 tools/tests/rock_tsr_sim.py`
with the SDL build directory and an empty evidence directory. Run
`python3 tools/tests/rock_tsr_test.py` with a host scratch directory to inject
worker-start failure and a tick blocked during USB stop.

## Device acceptance

| Target | Actions | Pass criteria | Status |
| --- | --- | --- | --- |
| ipod6g with matched firmware and probe images | With native audio playing, open the TSR probe and press Menu. Open the foreground overwrite probe, press Menu, then relaunch the TSR. Repeat with Select to abort after registration. Repeat with USB Connect and with orderly poweroff. Observe the TSR probe's `ticks`, `last_stop` and the firmware worker slot in a diagnostic build. | The tick counter advances during foreground overwrite. Reentry starts with fresh module data. The expected stop reason is recorded exactly once and the worker slot clears before USB acknowledgement or shutdown. Audio and the native UI survive each return. | Pending physical device run. |
