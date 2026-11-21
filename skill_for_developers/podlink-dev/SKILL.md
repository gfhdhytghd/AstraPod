---
name: podlink-dev
description: Developer guide to PodLink, the USB link between an iPod Classic running AstraPod and a computer, and everything built on it - the message transport, the media protocols (MS1 import, MS2 Jellyfin sync, MS3 library), Disk Mode consent, the EP0 side channels (remote keys, PodLink Shell, reboot, Debug menu), HerdRemote and the Agents screen with T3 control, the AstraPod desktop app and input plugins. Load before changing or debugging any of these, or before writing a new host tool that talks to the player.
---

# PodLink developer guide

This is an internals guide: it names source files. Read the linked document
before changing a protocol; this page is the map, the documents are the
contract. Host paths and host commands below are relative to
[AstraPod-Desktop](https://github.com/gfhdhytghd/AstraPod-Desktop). Firmware paths and build commands
are relative to this checkout.

## The parts

```
iPod (ipod6g only)                         Computer
──────────────────                         ────────
PodLink service (Rust, resident)  ◄─CDC─►  herdremote.py  (sole tty owner)
  ├ HR/W/S/A/D/Q… → Agents screen            ├ herdr, Codex, Claude, T3 readers
  ├ MS1/MS2/MS3   → podlink_media.c          ├ desktop.sock  ◄── AstraPod desktop
  ├ DH/DA/DK      → Disk Mode consent        ├ disk.sock     ◄── borrow / disk mode
  └ TS, HF, PLFG  → clock, handoff, focus    └ fallback API  ◄── IR bridge, plugins
USB core EP0 ◄──────────── control requests ── usb_remote_key, podlink_shell.py
```

Two channels share one cable:

- **CDC-ACM bulk**: PodLink frames, every text protocol below.
- **EP0 vendor requests**: small, stateless commands that work even while the
  CDC side is busy or owned by another process.

PodLink is built only for `ipod6g` (`HAVE_USB_PODLINK`). The simulator has no
PodLink transport; HerdRemote can still talk to the simulator's Agents screen
through `--port /dev/pts/N`.

## Transport

| Item | Where |
|---|---|
| C API (`podlink_open/poll/send/close`) | `firmware/export/podlink.h` |
| Implementation, Rust, `no_std` | `firmware/usbstack/podlink-rust/` (read `AUDIT.md`) |
| C platform glue | `firmware/usbstack/podlink_rust_platform.c` |
| Wire format, API rules, Rust toolchain | `docs/podlink.md` |
| Python client and frame codec | `podlink.py` (`Link`, `encode`, `find_port`) |

Rules that bite:

- Frames are `PLNK` + type + seq + length + payload + CRC32. Max message is
  16384 bytes (4096 with legacy peers); use `podlink_max_message()`.
- `podlink_send` returns `PODLINK_BUSY` without taking the payload. Poll, then
  retry. Two application frames can queue; the third slot is for control.
- `PODLINK_INVALID` means the message is wrong. Do not retry it unchanged.
- Keepalive every 5 s idle, timeout at 15 s. Unplug gives one CLOSE 1006.
- The service is **resident**: Agents, Now Playing and the PodLink demo
  attach and detach locally. Opening or leaving a screen does not reconnect
  USB or close the host session. The stable CDC serial is `2AA0000000000360`.
- Firmware needs the pinned Rust nightly with `rust-src`; configure fails
  without it. `--type=n` only; other build types disable PodLink.
- Never enable the crate's `host-test` feature in firmware.

Identify the player by USB VID `05ac`, PID `1261` and serial, never by
`ttyACM` number.

## Text protocols on the link

Every protocol is TEXT frames, tab-separated fields, one request in flight.
The firmware routes by prefix.

| Prefix | Purpose | Contract |
|---|---|---|
| `hr` / `HR`, `W S A T D Q X R U P N` | HerdRemote Agents screen | `docs/podbox/herdremote-protocol.md` |
| `MS1` | Import one audio file | `docs/podlink.md` "Import audio files" |
| `MS2` | Jellyfin sync directory: LIST/NEXT/PUT/DEL | `docs/podlink.md` "Sync Jellyfin" |
| `MS3` | Library: SUMMARY, FILES, FILE, DB walk, DETAIL, ARTBIN, ART, DEL | `docs/podlink.md` "MS3 library" |
| `DH` / `DA` / `DK` | Disk Mode consent token | `docs/podlink.md` "Resident Disk Mode" |
| `TS` | Computer clock to the RTC | protocol doc "Computer clock sync" |
| `HF` / `hf` | Continuity handoff of playback | protocol doc "Continuity handoff v1" |
| `PLFG` in a PING | Which screen is in front | protocol doc "Follow-up 8" |

### Media (MS1, MS2, MS3)

Firmware: `apps-ipod/screens/system/podlink_media.c` (MS1, MS2, dispatch at
the prefix checks) and `podlink_library.c` (MS3). Host:
`podlink.py` (`import_media`, `upload_media_track`),
`herdremote/hrmediasync.py` (Jellyfin) and
`herdremote/hrlibrary.py` (MS3 client and `LibraryCache`).

- TEXT replies are at most 500 bytes. The walk and artwork use base64
  `MS3 DATA +`/`.` chunks.
- Use `MS3 FILES`, then `MS3 FILE <name>` and `MS3 NEXT` for raw tagcache
  files. Check the `MSF1` chunk offsets, final length and CRC32, then
  compare `MS3 FILES` before and after. Fall back on `MS3 ERR command`.
- Use `MS3 ARTBIN <size> <path>` and `MS3 NEXT` for cached RGB565 pixels.
  Check the same MSF1 offsets and final length/CRC32 as for bulk database
  reads. Fall back to text `MS3 ART` on `MS3 ERR command`.
- A request that collides with another open media operation gets
  `MSx ERR busy`. `MS3 STOP` or disconnect frees an abandoned walk.
- Uploads stage in `/.rockbox/podlink-media/`, check length and CRC, then
  publish under `/Music/PodLink/` with a SHA-256 name. Jellyfin files live in
  `/Music/PodLink/Jellyfin/`. Both must be inside the database scan roots.
- `MS3 DEL` deletes only under `/Music/PodLink/`. Send `MS1 REFRESH` after.
- Artwork is little-endian RGB565 at 44, 128 or 300 px from the art cache.
- A full `MS3 DB` walk is slow: about 10 tracks per second, 3.5 minutes for
  2100 tracks. Cache by `commit_id` **and** track count; a deletion-only scan
  changes the count without a new commit.
- `MS3 ERR database changed`: drop the partial result and start again.

### Agents and T3

Firmware: `apps-ipod/screens/system/agents_screen.c` (views, keys, action
sheet) and `herdremote_model.c` (bounded model, 32 KiB). Host:
`herdremote/herdremote.py` (session), `hrproto.py` (encoders),
`hrherdr.py` (herdr), `hrsession.py`/`hrappserver.py` (Claude/Codex detail),
`hrt3.py` (T3), `hragent.py` (action sheets).

- Field text never contains tab, newline or control bytes. Every field has a
  byte budget; both sides cut on a UTF-8 boundary, never reject.
- An unknown message or a record with the wrong field count is ignored
  whole. That is how old firmware stays compatible: add a capability record
  (`tc 1`, `qx 1`, `HF 1`) instead of bumping the version.
- T3 appears after `#<TAB>T3` in `W`. IDs are `t3p<n>`/`t3t<n>`, questions
  `t3q`/`t3a`. `fp <req> <thread>` opens `t3code://threads/<env>/<thread>`
  through `xdg-open`, which switches the T3 app's thread without raising it.
- Action sheet: `ks <target>` → `K` → `act <req> <target> <action>` → `R`.
  Action IDs are opaque; drop the sheet when the target changes.
- T3 server access is Effect JSON RPC over WebSocket,
  `/ws?orchestrationProtocol=2`, Bearer token from
  `t3 auth session issue --scope orchestration:read --scope orchestration:operate`.
  Details in `herdremote/README.md` "T3 remote protocol".
- Model tests: `tests/agents_t3_model_test.c`.

## Who owns the tty

`herdremote.py` is the **only** process that holds the PodLink tty. Anything
else borrows it:

```python
import hrmediasync, hrlibrary
with hrmediasync.borrow_serial():                 # daemon releases the port
    with hrmediasync.connect_device(None) as (link, serial):
        print(hrlibrary.summary(link))
```

The lease goes over the disk socket (`hrdisk.default_socket()`) with
`borrow-serial-v1`; `enter-disk-v1` asks the daemon to put the player in Disk
Mode. `herdremote/podlink_disk.py` does that and waits for the volume.
Run with the daemon's venv:
`~/.local/share/podbox-herdremote/venv/bin/python`.

`podlink.py` subcommands open the tty themselves; stop the daemon
first or they fail with the port busy.

## EP0 side channels

All are vendor requests on the default control endpoint, handled in
`firmware/usbstack/usb_core.c` `usb_core_control_request_handler`. They need
the device configured. Host side uses PyUSB (`podbox-usbkey` venv has it).

| bRequest | wIndex | Use | Defined in |
|---|---|---|---|
| `0x5b` | key ID | Remote keys: Menu 1, Select 2, Vol± 3/4, heartbeat 5, Left 6, Right 7, Play 8 | `firmware/export/button_usb_remote.h` |
| `0x5c` | 0 | Disk Mode exit status (IN, 8 bytes) and unmount acknowledgement (OUT) | `docs/podlink.md` |
| `0x5d` | `0x4951` | PodLink Shell: wValue 1 reboot, 2 play test track, 3 seek, 4 stop; IN wValue 0 reads 16-byte status | `firmware/export/sm1-test-control.h` |
| `0x5d` | `0x4952` | Debug menu: IN reads item name by index (128 bytes), OUT opens that item on the player | same |

PodLink Shell commands need PodLink active and storage not exclusive. One
command at a time: the device stalls a second one until the first completes.
The command reaches the UI thread as `SYS_SM1_TEST_CONTROL` and runs in
`apps-ipod/system/shutdown.c` `default_event_handler_ex`. Reboot goes
through `clean_shutdown(SHUTDOWN_REBOOT)`, so settings and the database are
saved.

```sh
python3 podlink.py reboot --serial 2AA0000000000360
python3 podlink_shell.py --serial 2AA0000000000360 status
python3 podlink_shell.py --serial 2AA0000000000360 debug-list
python3 podlink_shell.py --serial 2AA0000000000360 debug "Hardware info"
```

EP0 works while HerdRemote holds the tty, so the shell needs no lease.
User guide: `docs/podbox/podlink-shell.md`.

## Desktop and plugins

| Part | Where | Contract |
|---|---|---|
| Desktop API, JSON lines on `$XDG_RUNTIME_DIR/podbox-herdremote/desktop.sock` | `herdremote/hrdesktop.py` | [`herdremote/README.md`](https://github.com/gfhdhytghd/AstraPod-Desktop/blob/master/herdremote/README.md#desktop-api) |
| AstraPod desktop, PySide6, Material 3 | `astrapod_desktop/`, entry `podlink_gui.py` (`--demo` uses fakes) | [Desktop README](https://github.com/gfhdhytghd/AstraPod-Desktop/blob/master/README.md) |
| Settings shared by desktop and daemon | `podlink_settings.py` | — |
| Input plugin host and SDK | `astrapod_plugins/` | [`docs/input-plugins.md`](https://github.com/gfhdhytghd/AstraPod-Desktop/blob/master/docs/input-plugins.md) |
| Apple IR bridge | `usb_remote_key/`, wrapped by plugin `apple-ir` | [`docs/ir-host-fallback.md`](https://github.com/gfhdhytghd/AstraPod-Desktop/blob/master/docs/ir-host-fallback.md) |

The desktop manages media only; it never plays audio. It reaches the player
only through the daemon: the desktop API for state and actions, the serial
lease for media work.

Plugins are stdio JSON-lines processes, disabled by default, with manifest
permissions `ep0`, `keys` and `voice`. A plugin reports input as
observations; the host executes an action only with the matching permission.

## Testing

See [desktop tests](https://github.com/gfhdhytghd/AstraPod-Desktop/blob/master/README.md#tests) for the daemon,
desktop, plugins and cross-repository contracts. Run transport tests here:

```sh
sh tools/tests/run_podlink.sh
cargo +nightly-2026-02-03 test --manifest-path firmware/usbstack/podlink-rust/Cargo.toml
python3 tools/tests/arm_podlink_link.py --elf build-hw-ipod6g/rockbox.elf
```

Offline tests use mock serial and synthetic frames. They prove nothing about
real USB, throughput or charging. On the device, check MS3 with the borrow
snippet above, and check that the reported firmware string carries the
commit you built.

## Flashing and the reboot loop

1. Build: `./build-hw.sh ipod6g`. If it stops at "Empty language files",
   re-run `make`, `make zip` and the bundle scripts in `build-hw-ipod6g/`.
2. Enter Disk Mode: `podlink_disk.py`, wait for the volume by UUID.
3. Copy the zip onto the volume, verify every file, write `rockbox.ipod`
   last, unmount and eject.
4. Reboot: `podlink.py reboot`, or by hand. **Writing the firmware does
   not run it**; until the reboot the old build answers.

A new request that times out right after flashing (for example
`MS3 SUMMARY` while `MS2` works) usually means the old firmware is still
running. Compare the firmware string from `MS3 SUMMARY` with the commit.
