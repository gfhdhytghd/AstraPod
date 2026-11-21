# PodLink Rust boundary audit

This is the only PodLink firmware implementation. Native normal ipod6g builds
always link it; bootloaders and hosted tools disable PodLink.
The default remains C. `wire.rs`, `session.rs`, `service.rs`, and `tests.rs`
forbid unsafe code. Only `ffi.rs` permits it. The crate has no dependencies.
The firmware feature is `no_std`, has no allocator, and aborts through the
Rockbox `panicf` adapter. Host tests use std only as a test harness.

## What Rust owns

The Rust parser owns the bounded frame array. It validates type, reserved
bytes, length, CRC and recovery offsets before returning a message. Session
state, the three TX slots (one reserved for control), stream-generation
invalidation, negotiation, heartbeat, CLOSE and the 4096-byte RX staging area
are Rust values. Application dispatch matches the closed `Kind` enum.

The resident service owns the bounded frame/byte inbox, HR welcome, TS routing,
DA/DK one-shot token, outgoing resident/log records and foreground generation.
It does not hold a pointer to a C callback table. Its FFI adapter copies the
four callback fields into typed `Option<unsafe extern "C" fn(...)>` entries.
It checks each Option at the call site while holding the service mutex.
Registration, replacement and unregistration take that same mutex. Unregister
returns only after an in-progress callback finishes. No consumer table of raw
function addresses is used for dispatch.

The independent APL1 registration copies three typed callbacks under the
same lock. It leaves the Cider registry layout intact. The service consumes
only BINARY frames with the APL1 prefix in this lane. TX uses the same
reserved application slot and PLNK CRC path as other resident binary data.
Wire invalidation resets APL1. Foreground inbox pressure leaves APL1 open.
The firmware callbacks publish bounded data and controls under short IRQ
guards and wake an independent worker. That worker owns all descriptors
and acknowledges USB storage only after cleanup.

`podlink_open_agents` also copies a typed optional callback under that mutex.
As in C, detach flushes queued list deltas and keeps the background callback
for the persistent Agents cache. Its code and cache have static firmware
lifetime. Shutdown clears both registrations. Calling open_agents with NULL
retains the previous background callback, matching the C implementation.

## C caller obligations

These are the existing C ABI contracts, not claims that Rust can validate an
arbitrary C address. They apply to every unsafe export in `ffi.rs`:

- Every non-NULL pointer is aligned for its C type and valid for the complete
  stated range during the call. Message outputs are writable. Buffers supplied
  to the codec do not alias each other or the parser. Encode output has at
  least `length + 20` bytes, as required by the capacity-less existing ABI.
- A NULL payload with length zero means an empty payload. A NULL nonempty
  payload, oversized application send, NULL parser/output and corrupt public
  parser counters are rejected before dereference or indexing.
- CRC input may be larger than one frame: Cider uses it for complete art/cache
  objects. It is not capped at MAX_MESSAGE. Its pointer/range must still be
  valid. The frame decoder rejects wire lengths above 16384 before copying.
- A callback address, when non-NULL, names a function linked into the firmware.
  Consumer state remains valid until unregister returns. Callback tables need
  only live until set_resident returns; their addresses are never retained.
- Callbacks run synchronously under the service mutex. They must not reenter
  state/registration APIs, retain message pointers or write read-only payloads.
  Pure CRC/independent codec calls are permitted. `next` must write at most
  its supplied capacity. Foreign C memory corruption is outside this boundary.
- Init runs once from the boot/UI thread, as in C. Shutdown does not create a
  new worker. Only the resident worker calls internal session APIs in normal
  firmware. A foreground caller owns its delivered payload until next poll;
  concurrent foreground polls are not supported by the existing ABI.

`Message`, `Parser`, and `Registry` use repr(C). State exports return the
values 0..4 as c_int in the EABI scalar result register; no Rust enum object
crosses a shared-memory boundary. The target GCC layout probe found 1-byte
C enums, 16-byte Message, 12-byte Registry, and 16412-byte Parser. Host C
fixtures use their native enum layout and observe the same scalar results.
c_uint/u32 and c_ulong preserve the remaining public signatures.
Option function pointers use Rust's guaranteed nullable-function-pointer ABI.
Both compilers use AAPCS/EABI, soft float, ARMv5TE/ARM926EJ-S and interworking.
No floating-point operations occur in this layer.

## Shared memory and returned payloads

`CTX: UnsafeCell<Context>` has one unsafe Sync implementation. `with` obtains
the C service mutex before creating any Rust reference into it. A guard drops
all references before unlocking. Callbacks run inside that scope; they cannot
reenter it. No context reference escapes. The firmware builds this module
only on the single-core ipod6g target.

`DELIVERY: UnsafeCell<[u8; 16384]>` is a separate static, with a separate Sync
implementation justified by the same mutex and the single foreground reader.
A poll copies the selected payload into it. Worker context borrows cannot
invalidate or mutate the C payload pointer. Send can safely read that payload
while borrowing the independent service context. Detach uses the private
session event array when flushing the queue, leaving DELIVERY unchanged.
Session ABI polls also copy into DELIVERY; internal firmware service polling
uses the safe Session method directly. Do not mix internal session ABI calls
with a foreground client.

`CLOCK: UnsafeCell<TimeMailbox>` is independent of Context and DELIVERY.
Its unsafe Sync implementation is justified by a short IRQ critical section
around every access. The safe TimeMailbox code validates and copies each TS
record. UI take_time copies at most 64 bytes and a tick; it never takes the
service mutex. Invalidation clears the mailbox; inbox pressure preserves it.
Publishing coalesces wakeups while keeping the newest record. Neither a
reference nor a pointer into CLOCK escapes the IRQ guard. The C adapter only
provides IRQ save/restore, with compiler memory barriers in Rockbox's primitive.

No CAS or spin lock is used: armv5te-none-eabi has max-atomic-width=0. The C
platform adapter publishes small aligned snapshots with IRQs disabled. State
and host-connected reads only use those snapshots and the CDC generation
check; they never take the service or serial mutex. A snapshot may reflect
the preceding completed worker step while a callback runs. USB generation
checks invalidate host-connected immediately on disconnect/reconfiguration.

The C adapter contains only thread/mutex/IRQ/tick/button/panic glue. It owns
the same 2 * DEFAULT_STACK_SIZE worker stack as C. The Rust resident-inbox
subset plus external delivery/welcome/record storage is asserted <=51000
bytes; C asserts that bound plus the actual stack is below 64 KiB. This is
the original resident subset cap, not a 64 KiB cap on the entire transport.
All static transport RAM and whole-image growth are measured separately.

## Verification and limits

`tools/tests/run_podlink_rust.py` runs the original session, wire, BENCH and
resident assertion streams through the Rust C ABI against frozen C goldens.
Private-state observations use host-test getters. It compares complete binary
traces of controller writes, returned messages, state and generation changes,
and callback deliveries. It adds 256 seeded session streams and synthetic
recorded-shape Cider/Agents/art/disk sequences. Those are not physical captures
or acceptance of MusicKit/Cider playback. C fixtures use ASan/UBSan. Rust uses
its ordinary bounds checks; it is not instrumented by C's sanitizer flags.

The deterministic fuzz target regenerates 10,000 input streams and compares
per-seed SHA256 digests with the retired C implementation. The digest covers
encoding, consumption, buffered parser bytes and decoded messages. The registry pthread fixture destroys a caller's transient
table, blocks a callback while unregister competes for the same mutex, checks
NULL fields, and verifies no callback occurs after unregister returns. The
ARM fixture executes linked Rust/FFI/snapshot instructions, samples SP, and
fails on a NULL branch. Its scheduler, USB, logger and consumer are adapters;
consumer-stack use and arbitrary scheduling are outside its bound. The
existing ARM skin/storage/fatal/PMU checks cover their documented scenarios.
`tools/tests/arm_podlink_link.py` checks that the prelinked Rust object
exports only `podlink_*` and that libc/EABI helpers resolve to libfirmware,
libarm_support or libgcc. The threaded liveness fixture
(`tools/tests/podlink_liveness.c`) runs the real worker thread and CDC ring
on pthreads with Rockbox mutex/semaphore/IRQ-mask semantics. It checks RX
drain, CH answers, UI call latency, Agents bursts, Cider pushes, art-failure
bursts, USB reconnect and host CLOSE. A USB-thread stall must make it fail.
The worker stack has a checked guard; an overflow panics.

These checks do not prove all possible schedules, safety of downstream C
consumers, absence of arbitrary external memory corruption, physical USB
behavior, actual playback, or a fix for the separate FU47 device crash.
