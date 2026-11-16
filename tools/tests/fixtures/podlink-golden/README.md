# FU49 C oracle, frozen before implementation removal

Recorded offline from revision `16f4a56945` in the FU49 worktree, using the
retired C sources identified by SHA256 in `provenance.json`. The original
assertion fixtures were instrumented before removal. The retained test sources
are the deterministic input generators; only private-field observations and
platform adapters changed when they were ported to the Rust C ABI.

`traces.json` lists the uncompressed sizes and SHA256 checksums. Gzip streams
have mtime 0. The ordinary trace format is little-endian uint32 tag, uint32
payload length, then bytes. There are no addresses or struct padding.

| Tag | Payload |
| --- | --- |
| 1 | USB controller output bytes (also records output from BENCH/resident/FU47) |
| 2 | session poll result, state, negotiated max |
| 3, 4 | returned message type/flags/sequence/length; message bytes |
| 5, 6 | background callback message metadata; message bytes |
| 7 | client state, connected, generation, session state, session revision |
| 8 | Cider resident consumed record |
| 9, 10, 11 | foreground poll result; metadata; delivery bytes |
| 12, 13 | wire return/consumption/parser counts; buffered parser bytes |
| 14, 15 | decoded wire metadata; decoded bytes |
| 16 | FU47 receive/next/reset/background callback counts |

Session inputs include the original assertions and 256 xorshift32 sessions
(initial seed `0xf048`). Resident inputs include 500 foreground switches,
80 CAR1 art frames, Agents cache/background, TS, log and DA/DK disk authorization,
plus CA/CN/CL/CK/CXS1 and an outgoing Cider command. FU47 adds 200 rounds of
partial/NULL callback tables, empty item IDs, four CN pushes, queue changes,
Agents entry, remote WPS, CLOSE and unplug/replug.

The fuzz trace is a different compact format: 10,000 records of uint32 initial
stream seed plus 32 SHA256 bytes. `podlink_rust_fuzz.c` deterministically
regenerates every input (xorshift32 starting at `0x48f00d`). Each digest covers
the seed, uncorrupted encoded bytes, corrupted input plus valid recovery frame,
each decoder return/consumption/parser state and all delivered message bytes.
All original eight mutation classes, randomized chunks, and the first 64
byte-at-a-time streams remain. There are 770,424 decode calls. Compare the
entire seed/digest file, not just a corpus-wide summary hash.

`run_podlink_rust.py` verifies the manifest and compares complete ordinary
traces (decompressed in memory) and the seed/digest corpus. It has no record or
update option. Golden changes require an explicit oracle review. The fixture
size is about 420 KiB compressed; no retired C protocol code is retained.
These synthetic sequences do not establish physical USB or playback behavior.
