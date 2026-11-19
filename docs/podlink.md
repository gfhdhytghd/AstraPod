# PodLink

PodLink connects a running Astra application to a computer over USB CDC-ACM.
It uses RAM for messages. It does not use disk files or USB mass storage for
communication. The default USB mode still provides mass storage. The simulator
has no PodLink USB transport or PodLink menu entry.

The demo is a native screen at **System > PodLink**. The reusable message API
is `firmware/export/podlink.h`; its protocol version is 1.

## Connect

1. Unplug the USB cable.
2. Open **System > PodLink** on the iPod.
3. Connect USB. The screen shows `USB up / waiting OPEN` until a host connects.
4. Run the host client:

```sh
python3 -m venv .venv-podlink
.venv-podlink/bin/pip install pyserial websockets
.venv-podlink/bin/python tools/podlink.py ping
.venv-podlink/bin/python tools/podlink.py info
.venv-podlink/bin/python tools/podlink.py echo 'hello'
.venv-podlink/bin/python tools/podlink.py latency --samples 100
.venv-podlink/bin/python tools/podlink.py bench --bytes 1048576
```

The client finds a tty with VID `05ac` and PID `1261`. To select a port, pass
`--port /dev/serial/by-id/<identified-device>` before the subcommand.
Check its USB manufacturer and serial before selecting it; ttyACM numbering
alone does not identify a device. The OS user needs access to that tty. Each
command opens and cleanly closes its own message session.

Press Menu on the screen to close and exit. USB then re-enumerates using the
normal USB setting. With the default setting, the disk becomes available.
Charge-only selection (including the USBPOWER button) takes precedence over
PodLink, so the screen waits until a normal data connection is selected.
Unplugging while the screen is open reports a close event. Reconnect USB and
run the host again; restarting the screen is unnecessary.

## WebSocket

```sh
.venv-podlink/bin/python tools/podlink.py serve --listen-port 8765
```

Connect one client to `ws://127.0.0.1:8765/`. Text and binary messages retain
boundaries and content; each becomes one PodLink frame. The demo echoes them.
The text `INFO` returns a JSON description. The server binds only to localhost.
A second simultaneous client receives close code 1013. The server closes the
WebSocket when the USB session ends. After replug, open a new WebSocket; the
server discovers the tty again. It does not replay messages from a lost session.

`free_ram_bytes` is the firmware core allocator's currently available bytes;
`free_ram_scope` is `core_allocator`. It is not the device's physical RAM total.

## Message API

Call `podlink_open()` with USB unplugged, then call `podlink_poll(&message)`
regularly. A poll never waits for USB data. A positive result contains one whole
message; its data pointer remains valid until the next poll. `podlink_send`
copies an entire payload or returns `PODLINK_BUSY` without accepting it. Retry
busy sends after polling. At most two application frames can be queued; a
third slot is reserved for control traffic. Messages are at most 16384 bytes,
or 4096 with legacy peers.
`podlink_max_message()` returns the negotiated outgoing limit.
`PODLINK_INVALID` rejects an oversized or malformed send; drop or resize that
message instead of retrying it unchanged. The demo drops rejected replies
and sizes download blocks to the peer limit.

`podlink_state()` distinguishes waiting for USB, handshaking, open and closing.
Call `podlink_close()` and continue polling until closed. It sends CLOSE and
releases USB mode after an acknowledgement or one second. A remote CLOSE ends
only the message session; the screen waits for another OPEN. USB unplug produces
one CLOSE event with code 1006. The screen retains link mode until it exits.

## Wire v1 (internal reference)

All integers are little-endian. CRC32 uses the IEEE polynomial and standard
initial/final inversion (compatible with Python `zlib.crc32`).

| Offset | Bytes | Value |
| --- | --- | --- |
| 0 | 4 | ASCII `PLNK` |
| 4 | 1 | Type |
| 5 | 1 | Flags, zero |
| 6 | 2 | Reserved, zero |
| 8 | 4 | Sequence |
| 12 | 4 | Payload length, 0..negotiated maximum (currently 16384) |
| 16 | length | Payload |
| 16 + length | 4 | CRC32 of bytes 4 through the end of the payload |

Types are OPEN=1, TEXT=2, BINARY=3, PING=4, PONG=5, CLOSE=6, HELLO=7.
HELLO and OPEN contain version `u16` followed by maximum message size `u32`.
The device sends HELLO when USB configures. The host sends OPEN; the device
acknowledges OPEN with its sequence and negotiated limit. Each side uses the
smaller of its own maximum and its peer's advertised maximum. A peer advertising
a larger maximum is supported; zero is invalid. This keeps new hosts compatible
with 4096-byte firmware and new firmware compatible with 4096-byte hosts.
An outgoing payload above the session limit is rejected before writing;
an incoming payload above the limit ends the session with CLOSE 1009.
PONG retains the
PING sequence and payload. Both ends send an idle keepalive after five seconds
and time out an unanswered keepalive after fifteen seconds. CLOSE starts with
a `u16` code, followed by a UTF-8 reason. Bad CRC or magic triggers scanning for
the next valid frame. An oversized length triggers CLOSE 1009 without allocating
memory for that length. The two sides reset their byte queues on USB reconnect.

Demo throughput commands use TEXT `BENCH TX N` / `BENCH RX N`. After TEXT
`READY TX N` / `READY RX N`, exactly N bytes travel in BINARY messages. Host
uploads and demo downloads use chunks up to the negotiated limit. Ordinary
echo messages obey the same limit in either direction.
Byte at offset i is `i % 251`. Final TEXT is `OK TX N errors=0` or
`OK RX N`. A valid BENCH command replaces any active transfer. A rejected size
preserves the active transfer. Both directions verify the complete pattern.
Latency measures protocol PING/PONG and 64-byte binary echo separately.

## Host verification

```sh
PYTHON=.venv-podlink/bin/python tools/tests/run_podlink.sh
```

These checks use mock serial/controller state and a real local WebSocket. They
cannot establish physical USB, iPod startup, or real-device throughput.

The USB mode lease lasts until the screen exits. Cable removal or USB
reconfiguration resets the byte queues and message handshake. A queued close
from an older lease cannot release a newer screen's lease. Normal USB profiles
return after the current lease closes.

The CDC driver does not request a CPU boost. Legacy serial logging remains
disabled by default. Idle logging returns without taking the serial mutex;
interrupt-context log messages are not forwarded to CDC on targets that expose
`is_thread_context()`. PodLink always excludes log messages from its stream.

## Charging in PodLink

PodLink and normal storage use the same saved **Charge During USB Connection**
setting (**Settings > Battery & Power**). With `on` or `force`, both request 500 mA
(`bMaxPower` byte 250, in 2 mA units). With `off`, both request 100 mA
(`bMaxPower` byte 50) and do not enable USB battery charging, including the
unconfigured state. The default is `on`; an absent `usb charging`
line in config.cfg uses that default. Select the setting before reconnecting:
a host's existing 100 mA
grant is not raised merely because the setting changed.

Before configuration, the device restores USB input to 100 mA on every attach,
including after boot settings or unplug set SUSP. With `on`, charging is enabled
at that safe limit even before a computer configures us, with the backlight
lit or dark. Charge Only keeps the USB device stack running in the normal UI
so the computer can configure a 500 mA grant. `force`
retains the explicit 500 mA no-host override after ten seconds. This override
is not a negotiated host grant. A configured
500 mA source enables the iPod6g charger and reports a charging source to the
battery status bar, without waiting for a storage-mode handover. Suspend
disables USB input and battery charging; resume restores the existing grant.
Unplug clears the grant. A bus reset requires configuration again. The
LTC4066 still controls battery charge termination and the battery's charge
current (the 100/500 mA limit includes device consumption).

On iPod6g, an automatic weak-source guard monitors `!CHRG` every 10 ms whenever
USB battery charging is enabled, including the normal UI, PodLink, wall
chargers and the powered USB host-DAC path. A qualified dropout is 20-490 ms
high following at least one second low. Eight completed dropouts in a rolling
90-second window inhibit battery charging for 1, 2, 4, 8, 16, then at most
30 minutes on successive detections. SUSP and HPWR retain the applicable USB
budget during this back-off; the battery icon reports USB power without a
charging source. Each pause ends in an automatic retry. Unplugging or applying
the charging setting resets the guard; ten continuous minutes of low `!CHRG`
reset the next delay to one minute. Suspend stops sampling and clears partial
pulse evidence; resume observes a new trace and respects any remaining pause.

A steady high pin, including full-battery indication or a long thermal pause,
does not trigger the guard. Repeated load/source/temperature transitions that
match the same short-pulse signature can cause a temporary false trigger;
`!CHRG` does not identify a DAC or measure battery current. A configured MFi DAC
can receive a 500 mA grant just like a PC, so enumeration never bypasses the
guard. The separate USB host-audio path is known via `usb_host_probe_active()`;
its external-VBUS 500 mA allowance is not a computer's negotiated grant.
**System > Debug > Battery** shows C1/C2, SUSP, HPWR, host grant, USB device
state (0=Default, 1=Address, 2=Configured), host presence, stack enabled,
dropout/trigger counts, remaining back-off, next delay and last trigger time
in seconds since boot. No physical charging result follows from these fields.

Run the production USB core and iPod6g power code against RAM-backed hardware:

```sh
cc -std=gnu11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
  -ffunction-sections -fdata-sections -Wl,--gc-sections \
  -Itools/tests/charging_stubs -Ifirmware/export -Ifirmware/usbstack \
  tools/tests/usb_charging_test.c -o /tmp/podlink-usb-charging-test
/tmp/podlink-usb-charging-test
```

Physical acceptance remains separate. With a partially discharged battery,
select `on`, unplug, enter **System > PodLink**, and reconnect.
Read the host descriptor using `lsusb -v -d 05ac:1261`: expect `bMaxPower 500mA`,
one configuration, and CDC interfaces in link mode. Find the matching USB
device directory under `/sys/bus/usb/devices` by its `idVendor=05ac` and
`idProduct=1261`; read its `bMaxPower` and `bConfigurationValue` (expect `500mA`
and `1`). Record the directory and serial. Do not select a tty by number.

Confirm the battery animation while the link is active. For a separate
diagnostic run, select Settings > System > Show Debug Menu = Yes, then open
System > Debug > Battery. Scroll down once from the voltage graph to the status
page. Record `USB current limit`, `Input: USB / dock`, `Host grant`,
`Charge enabled` (C1/C2 gates), `USB setting`, `SUSP`, `HPWR`, battery mV,
and `Pwr status` (the hardware !CHRG pin). `Host grant` is zero before host
configuration or during suspend, reset and unplug; force/DAC allowances are
shown only in `USB current limit`. These are policy and GPIO observations,
not a battery current meter. Opening that
screen requires leaving PodLink, so do not label it a simultaneous observation
of PodLink charging. A full battery may correctly stop charging.
Repeat after the screen/backlight has been off for at least 15 seconds.
Check unplug/replug without leaving PodLink. Press Menu to return to storage.
Confirm storage enumeration and charging.
Repeat with `off` (100 mA, no host battery charging) and `force` (500 mA after
configuration). For each setting, unplug/replug before reading descriptors.

Run a separate host suspend/resume check.
During suspend, confirm charger input/charge stops with a USB power meter;
after resume, confirm the prior grant, battery animation and link recovery.
Keep the normal storage volume unmounted for its suspend comparison.
Do not infer physical charge current or link throughput from host-only tests.


### Resident Disk Mode extension (`disk-mode-v1`)

These TEXT records do not alter the six-byte wire OPEN/HELLO negotiation:

| Direction | Record | Meaning |
| --- | --- | --- |
| Device to host | `DH\t1\tdisk-mode-v1` | Resident hello/capability for this OPEN |
| Host to device | `DA\t1\t<32 lowercase hex>` | Install a session-scoped random bearer token after HR hello |
| Host to device | `DK\t1\t<same token>` | One-shot consent to enter Disk Mode |

The resident service intercepts authorization and disk records before any
foreground/background consumer. Invalid length, token, version, disconnected
session, pre-HR authorization and replayed requests have no effect. A transport
revision invalidates the bearer token. This extension trusts physical USB
possession; it does not add pairing or encryption.

While configured, EP0 `0xc0/0x5c`, value/index 0 and length 8 reads
`[version=1, exclusive_storage, exit_pending, warning, request_id_u32_le]`.
Only body long-MENU creates an exit notification. EP0 `0x40/0x5c` with length 0
acknowledges a completed host unmount, with `wValue=request_id & 65535` and
`wIndex=request_id >> 16`. Stale, unsolicited, wrong-direction and malformed
acknowledgements stall. The USB thread owns these states. It detaches before
local remount on acknowledgement or after the three-second timeout. See
`docs/podbox/herdremote-usage.md` for host installation and safety limits.

## Rust firmware build requirement

PodLink firmware is Rust-only. Native normal ipod6g builds always link
`firmware/usbstack/podlink-rust` and `podlink_rust_platform.c`. There is no
implementation switch. Other players do not enable `HAVE_USB_PODLINK`.
Bootloader, simulator, CheckWPS, database and Warble tool builds disable
PodLink and do not require Cargo. This also applies to advanced simulator
and bootloader configurations; advanced native firmware still requires Rust.

Install the exact toolchain pinned by the crate's `rust-toolchain.toml`:

```sh
rustup toolchain install nightly-2026-02-03 --profile minimal --component rust-src,rustfmt,clippy
mkdir -p build
cd build
../tools/configure --target=ipod6g --type=n --appsdir=apps-ipod
make -j8
```

`nightly-2026-02-03` is rustc 1.95.0-nightly
`f60a0f1bc (2026-02-02)`, the compiler used by FU48. Both configure and make
require Cargo through rustup. Configure rejects a missing pinned toolchain
or missing `rust-src` before writing its generated files. The library is built
with `--locked --release --features firmware --target armv5te-none-eabi
-Zbuild-std=core`. This tier-3 target requires building core from source;
`rustup target add` alone is insufficient. Firmware uses ARM926EJ-S, EABI soft
float, strict alignment, no_std, fixed buffers, no allocator and panic=abort.
Never enable the `host-test` feature for firmware packaging. The Rust archive
is prelinked into one relocatable object whose only global definitions are
`podlink_*`. Otherwise the weak `memset`/`memmove`/`memcmp`/`strlen` and EABI
helpers from `core`/`compiler_builtins` would replace the firmware's own for
every caller. `tools/tests/arm_podlink_link.py` checks this on the linked ELF.

The public C ABI and wire protocol are unchanged. Shared public headers remain
for C callers and the platform/test adapters. Rust copies nullable callback
fields into typed Option entries under the service mutex. Unregistration uses
that same mutex. State and host-connected queries use IRQ-protected snapshots.
Time-record consumption uses an independent IRQ-protected mailbox. Read
`firmware/usbstack/podlink-rust/AUDIT.md` for ownership and validation limits.

Offline checks (put temporary files under your build directory):

```sh
PYTHON=.venv-podlink/bin/python tools/tests/run_podlink.sh
cargo +nightly-2026-02-03 test --manifest-path firmware/usbstack/podlink-rust/Cargo.toml
python3 tools/tests/run_podlink_rust.py --build build
python3 tools/tests/arm_podlink_rust.py --elf build/rockbox.elf
python3 tools/tests/arm_podlink_link.py --elf build/rockbox.elf
python3 tools/tests/podlink_ram.py --build build --baseline saved-c-baseline.elf
```

The runner compares Rust C-ABI results with frozen C goldens in
`tools/tests/fixtures/podlink-golden/`; it never compiles a second protocol
implementation. All original assertions still execute. The 10,000-stream fuzz
corpus stores seeds and SHA256 hashes of encoded bytes, decoder results,
consumption, parser bytes and delivered messages. The session/resident goldens
include USB output, state transitions and callback/front-end events. These are
synthetic protocol-shaped inputs, not device captures.

Host C-ABI tests need a C compiler, OpenSSL development headers (`libcrypto`),
and the pinned Cargo toolchain. Python checks need pyserial and websockets;
ARM/RAM scripts need Unicorn and pyelftools. `PODLINK_TEST_BUILD` chooses the
host artifact directory (default `build-fu49`). The native32 Agents integration
fixture also builds Rust core for `i686-unknown-linux-gnu` and needs multilib C
libraries. RAM comparison uses a saved C baseline ELF; retired C objects are
not required. Offline acceptance does not establish device, playback or live
USB acceptance.
