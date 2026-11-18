# FU88 research firmware: shell, crash capture, sm1 hooks

The research image is the normal iPod 6G firmware plus three things:

- a **PodLink research shell** that a host drives with `tools/research/podsh.py`;
- **crash and hang capture**: a record of every panic, abort, undefined instruction, stack-guard trip or watchdog trip, then an automatic reboot into PodBox;
- **sm1 helpers** for experiments on the S5L8702 audio engine ([Chikuma](https://chikuma.ringum.net/architecture/sm1/), [x17](https://github.com/manz/x17), [sosumi](https://github.com/manz/sosumi)).

The normal UI and playback are unchanged. Research builds only: everything is compiled under `HAVE_RESEARCH_DIAGNOSTICS` / `HAVE_RESEARCH_SHELL`, which `--research` turns on.

## Build

```sh
mkdir -p build-fu88/release && cd build-fu88/release
../../tools/configure --target=ipod6g --type=n --appsdir=apps-ipod --research
make -j8 VERSION=fu88-research-<base> bin
```

Use separate build directories for normal and research images. The Rust PodLink library is built with its `research` feature in `podlink-rust-target-research`.

## Protocol

The shell rides the existing [PodLink](../podlink.md) session. The resident PodLink worker takes every TEXT or BINARY message that begins with `RS<TAB>`, so shell traffic never reaches HerdRemote's handlers or an app.

| Direction | Payload |
| --- | --- |
| host → device | `RS\t1\t<id>\t<command line>` (TEXT), or the same plus `\n<bytes>` (BINARY) |
| device → host | `RS\t1\t<id>\tOK\t<text>` or `RS\t1\t<id>\tERR\t<reason>` (TEXT); with bytes: `...\tOK\t<text>\n<bytes>` (BINARY) |
| liveness | `RS\t1\tPING\t<16 hex>` → `RS\t1\tPONG\t<same>`, answered on the PodLink worker itself |

- `<id>`: 1–16 letters or digits, echoed. A malformed request gets no reply.
- Command line: printable ASCII, at most 255 characters. Commands separated by `;` run in order in one request. Their texts are joined by `; `. The first error stops the batch.
- Binary parts are at most 12,288 bytes (`RS_DATA_MAX`). `podsh` splits larger transfers.
- Numbers: decimal or `0x` hex. `--force` anywhere in a command lifts the soft guards for that command.
- One request runs at a time. A request that arrives while one runs gets `ERR busy`.
- Commands run on a background `research` thread, not on the PodLink worker, so the UI and audio keep running. Polling commands spin for 1 ms, then yield. Every polling command has a timeout. `call` and `perf` have none of their own, so the hang watchdog ends a runaway blob (about 5 s, see below).

## Commands

| Command | Reply text / effect |
| --- | --- |
| `ping` | `pong` (through the shell thread; the `PING` frame above skips it) |
| `ver`, `clock`, `buf` | `ver=… freq=<Hz> boost=<count> up=<ticks> hz=100 buf=<scratch address> size=65536` |
| `peek <w> <addr> [n≤64]` | `n` values of width `w` (1, 2, 4), hex |
| `poke <w> <addr> <value>` | store one value |
| `dump <addr> <len> [w]` | `len=<n>` + bytes (width 4 when aligned, else 1, or as given) |
| `fill <addr> <len> <value> [w]` | store a pattern (≤16 MiB, yields every 64 KiB) |
| `memcpy <dst> <src> <len>` | copy (≤16 MiB) |
| `load <addr> [w]` + bytes | store the payload with stores of width `w` |
| `wait <addr> <mask> <value> <ms≤10000> [w]` | `hit us=<n> v=<value>` or `ERR v=<value> timeout` |
| `call <addr> [a0..a3]` | `r0=<hex> n=1 us=<n>`; bit 0 of `addr` selects Thumb. D/I caches are synced first |
| `perf <addr> <n> [a0..a3]` | the same for `n` calls, stopping after 3 s (`n=` is the count done) |
| `regs sm1\|vic0\|vic1\|dmac0\|dmac1\|i2s\|clk` | `<offset>=<value>` pairs. `sm1` reads only safe registers (no `+0x15C` mailbox pop, no `+0xC48` ack) |
| `irq` | VIC0 then VIC1: IRQ status, FIQ status, raw, select, enable |
| `boost 0\|1` | CPU boost, balanced |
| `mark <n>` | appends an `rs-mark` event to the breadcrumb log |
| `file ls` / `file get <name> [off len]` / `file put <name> [off]` + bytes / `file rm <name>` | `/.rockbox/research/<name>` only. Names are `[A-Za-z0-9._-]`, at most 32 characters, no leading dot. `put` at offset 0 truncates |
| `watchdog off\|on` | creates or removes `watchdog.off`. It takes effect at the next boot |
| `reboot` | replies, then reboots once the reply has left |
| `sm1 on` / `sm1 off` | see the sm1 section |
| `sm1 status` / `sm1 regs` | signature `+0xC80`, `+0x114`, events `+0xA98`, `+0xC10`, divider, VIC0 raw/enable bit 18, PWRCON0/1 |
| `sm1 load p\|x\|y <index>` + bytes | 16-byte P rows striped into the 4 lanes (`0x22020000 + lane*0x4000 + row*4`, 32-bit stores), X words (32-bit stores at `0x22030000 + 4a`), Y halfwords (16-bit stores at `0x22038000 + 2a`), bounds-checked |
| `sm1 param <offset> <h0> [h1…h15]` | 16-bit stores at `0x2203FF00 + 2*offset` (offset < 0x80) |
| `sm1 feed <desc> <data> <len>` | writes the 16-byte descriptor `{0, len<<16 \| 0x1F<<7, data \| 1<<31, 0}` at `desc` through the uncached view and submits `desc \| 1` at `+0x100`. Both must lie in the scratch buffer |
| `sm1 wait <ms>` | polls VIC0 raw bit 18. Replies `hit us=… v=… ev=<+0xA98>`. The line stays masked, so no ISR runs |
| `sm1 ack` | `+0xC48 = 4` |
| `sm1 slots` | `out=<0x2203FF28> ring=<n> units=<n>` and ` i:<filled>/<length>` per slot |
| `sm1 pcm <i>` | slot `i`'s PCM (`0x22038100 + i*units*2`, 16-bit reads) as bytes |
| `sm1 release <i>` | `entry+0xA = 0` |

The scratch buffer (64 KiB, `buf`) is the place for host blobs, descriptors and staged packets. It is pinned in `core_alloc` memory together with the request and reply buffers (16 KiB each) and the shell thread's 8 KiB stack. The research image therefore has about 104 KiB less audio buffer. It adds no static buffers.

## Safety guards

Guards apply to `poke`, `fill`, `memcpy`, `load` destinations and to `call` targets.

| Class | Ranges | With `--force` |
| --- | --- | --- |
| Hard: always refused | SPI0 `0x3C300000` (the NOR flash holding the bootloader), SPI1 `0x3CE00000`, SPI2 `0x3D200000` (4 KiB each); any range that wraps past 4 GiB | still refused |
| Soft | all DRAM and its uncached view (`0x00000000–0x1FFFFFFF`, `0x40000000–0x5FFFFFFF`) except the scratch buffer; IRAM0 `0x22000000–0x2201FFFF` (+alias); MIU (SDRAM controller); LCD; USB OTG (this link); ATA; NAND/ATA glue `0x38A00000`; VIC0/1; clock and power gates; watchdog | allowed |
| Open | the scratch buffer; IRAM1 `0x22020000–0x2203FFFF` (sm1 P/X/Y and parameter page); sm1 registers; DMAC, I2S, GPIO and other peripherals | — |

A `call` target must lie in the firmware's code or the scratch buffer unless `--force` is given. Reads are not guarded. Reading a peripheral whose clock is gated can stall the bus. The tick FIQ cannot run during such a stall, so only the hardware reset path or the user's forced reboot (Hold off, then Menu+Select about 8 s) recovers it. File commands cannot leave `/.rockbox/research/`. Nothing in the shell writes the bootloader, NOR or the disk outside that directory.

## Crash capture and automatic reboot

A panic, undefined instruction, prefetch abort, data abort, stack-guard panic, tick-watchdog trip or freeze-watchdog trip writes one record to uncached DRAM (`.persist`). The firmware then drains the write buffer and asks the SoC watchdog for a reset. That restarts through the installed bootloader into PodBox: no screen, no key press, no disk mode. The bootloader is never touched. The next boot writes `/.rockbox/research/crash-<boot % 16>.txt` (written to `.pending`, `fsync`, renamed). It also writes the previous boot's breadcrumb ring to `/.rockbox/research/log.txt`. A record stays in RAM until a write succeeds. A second fault before then keeps the first record.

```
FU88 1 <firmware version> boot=<n> kind=<1..6> ticks=<uptime> hz=100
pc=…  sp=…  lr=…  cpsr=…  far=…  fsr=…  text=<_textstart>
thread=<current thread>
message=<panic text / watchdog reason>
command=<last shell command, 23 chars, or "ping", or ->
r0=… … r12=…            (exceptions; r0–r7 for an FIQ-mode fault)
candidate=…              (up to 8 code-looking stack words: return-address candidates)
boot … UNCLEAN copies: DRAM
<status words and up to 32 breadcrumb events of that boot>
```

Kinds: 1 panic (`pc` = caller of `panicf`), 2 undefined instruction (Thumb-aware), 3 prefetch abort (`far` = pc), 4 data abort (`far`/`fsr` from CP15), 5 tick watchdog, 6 timer-FIQ watchdog (`message` = `Tick stalled`, `UI starved` or `Audio starved`).

`podsh collect out.json --elf build-fu88/release/rockbox.elf --nm arm-elf-eabi-nm` resolves `pc`, `lr` and the candidates to `symbol+offset`.

**Watchdog.** The timer FIQ checks that kernel ticks advance (about 3 s) and that a runnable UI thread or audio thread is scheduled (about 5 s). Queue waits do not count. On a trip it captures and reboots as above. `watchdog off` (or creating `/.rockbox/research/watchdog.off`) disables it from the next boot.

**IRAM1 is left to sm1.** Normal images keep a second breadcrumb copy in the top 2 KiB of IRAM1 (`0x2203F800`). That copy overlaps the sm1 parameter page `0x2203FF00` and status word `0x2203FF28`. The research image keeps only the DRAM copy. No path writes IRAM1: boot, `crumb()` from IRQs, the clean-shutdown mark, capture, report or watchdog. `tools/tests/arm_fu88_crash.py` checks every write in ARM emulation and checks that the image holds no IRAM1 ring address. The IRAM1/SM1 clock gate (PWRCON0[4]) is no longer touched at boot. `sm1 on` sets it.

**Log.** `rlog(id, value)` (`crumbs.h`) appends a breadcrumb event: cheap, IRQ-safe, size-capped ring. Events from the shell are `rs-ping`, `rs-mark` and `research` (a capture). `podsh log tail` reads `log.txt`, which holds the previous boot.

## Host: podsh

`tools/research/podsh.py` (with `tools/podlink.py` next to it in `tools/`) is a library and a CLI.

```sh
# direct serial (stop whatever holds the tty first, e.g. HerdRemote)
python3 tools/research/podsh.py ping
python3 tools/research/podsh.py run 'sm1 status; irq'
python3 tools/research/podsh.py run 'dump 0x38500000 0x40' --out sm1.bin
python3 tools/research/podsh.py crash list
python3 tools/research/podsh.py crash get 3
python3 tools/research/podsh.py collect crashes.json --elf build-fu88/release/rockbox.elf
python3 tools/research/podsh.py sm1-image probe-a.x17i --pass 0
python3 tools/research/podsh.py run-experiment e1-a.json --result e1-a.result.json
# or through a bridge: python3 tools/podlink.py serve, then
python3 tools/research/podsh.py --ws ws://127.0.0.1:8765/ ping
```

The serial port has one owner. HerdRemote needs no code change. Stop it for a research session (or run the bridge in its place), and start it again afterwards. `--volume <mounted root>` reads crash records and the log from a mounted disk instead of the link.

Reconnect: the `Shell` reopens the transport and re-proves the shell with `PING` whenever the link drops. That covers `reboot`, a crash reboot and a cable replug, within `--reconnect` seconds (60 by default). A request interrupted by a drop is sent once more after reconnecting. Library helpers: `peek/poke/dump/load/fill/memcpy/wait/call/perf/regs/file_*/crash_*/log_tail/reboot`, `sm1_load_image` (X17I v1), `sm1_load_section`, `sm1_param`, `sm1_cmd70(register)`, `sm1_wait_irq`, `sm1_slots`, `sm1_read_pcm`.

## Driving an sm1 experiment

`run-experiment` takes a JSON file. The result JSON holds a baseline (`ver`, crash list, `irq`, `sm1 status`), every step's text/bytes/time, reconnects, new crash records with the log tail, and a final `irq` and `sm1 status`. A new crash record fails the experiment.

Steps: `{"run": "<line>", "expect": "<exact text>" | "re:<regex>", "save": "<file>", "timeout": s}`, `{"image": "<x17i>", "passes": [0]}`, `{"blob": "<file>", "addr": "scratch+0x1000"}`, `{"mark": n}`, `{"sleep": s}`. `{scratch}` / `{scratch+0x40}` in a line expands to scratch addresses. Paths are relative to the spec file.

FU87 E1 (probe A), as specified in `FU87-REPORT.md` §6:

```json
{"name": "E1-probe-A", "steps": [
  {"run": "sm1 status"},
  {"run": "sm1 on", "expect": "re:sig=004e7309"},
  {"run": "regs sm1"},
  {"image": "fu87-probe-a.x17i", "passes": [0]},
  {"run": "peek 2 0x22038C00", "expect": "a55a"},
  {"run": "poke 4 0x38500984 0; sm1 wait 50", "expect": "re:hit us=\\d+ v=\\w+ ev=\\w+"},
  {"run": "sm1 ack; sm1 regs"},
  {"run": "peek 2 0x22038C00", "expect": "4f87"},
  {"run": "ping"}
]}
```

How E0–E3 map to the shell:

- **E0** baseline: `ping`, `crash list`, `clock`, `irq`, `regs clk|vic0|dmac0|i2s`, `sm1 status` (gates first), `peek 4 0x38500C80`, `dump 0x38500FD0 0x30`. The IRAM1 audit is the ARM test above. VIC0 line 18 stays masked (polled raw), so no IRQ handler needs to exist.
- **E1**: gate snapshot+set `sm1 on` (`sm1 off` restores the exact PWRCON0/1 values found), loads `sm1 load p 0` / `sm1 load y 0x600` (or `sm1-image`), start `poke 4 0x38500984 0`, completion `sm1 wait 50` then `sm1 ack`, result `peek 2 0x22038C00`. Timing is in `us=`. `+0xA98` and `+0xC10` are in `sm1 regs`.
- **0x70 sequence** (load handshake): `sm1_cmd70(register)` sends `poke <reg> 4; wait 0x38500114 7 7 10; poke <reg> 0x10; poke <reg> 0x60` as one batch. The command register offset is not published, so the caller supplies it. Mailbox drain: `peek 4 0x38500160` (count), then `peek 4 0x3850015C` (one pop per read). The 18-word table goes in with `poke 4 0x38500938…`. Ownership hand-back: `poke 4 0x3850100C <value>`.
- **E2**: upload each packet with `load {scratch+0x1000}`, then `sm1 feed {scratch+0x40} {scratch+0x1000} <len>`, then `sm1 wait 100`, `sm1 ack`, `sm1 slots`, `sm1 pcm 0` (save it), `sm1 release 0`. Compare the saved PCM with the reference on the host.
- **E3** (playback) needs firmware codec integration and is outside this shell.

Notes: the P/X/Y/parameter widths follow Chikuma and x17. Byte stores into engine memory are never used. The parameter page `0x2203FF00` is also Apple's bootloader reboot-reason page (Chikuma). PodBox's own bootloader does not depend on it. Do not include Apple firmware or Apple sm1 programs in experiments or in the repo.

## Tests

`python3 tools/tests/run_fu88_research.py --elf build-fu88/release/rockbox.elf` (with `unicorn` and `pyelftools` importable) runs:

1. `research_shell_test.c`: parser, framing, guard list, every command, sm1 helpers and file confinement against a RAM-only port.
2. `podlink_resident_test.c -DFU88_PROTOCOL_TEST`: host frames through the real Rust PodLink service into the shell and back (TEXT/BINARY, 4 KiB binary round trip, guard refusal, an sm1 batch). Shell frames never reach the app.
3. `test_podsh.py`: podsh against the real command core (built as a shared library) with a fake transport: chunked transfers, X17I loading, an E1-style experiment, reboot and drop reconnects, the CLI.
4. `arm_fu88_crash.py`: in ARM emulation of the real image, the Thumb shell core answers. Each fault kind is captured, reset and exported, with retry after a short write. Watchdog captures work. The IRAM1 audit passes.
