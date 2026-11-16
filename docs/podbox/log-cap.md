# PodBox log retention (FU41)

Each log family has a **5 MiB (5,242,880 byte) total upper bound**, including
current and backup. Each part is at most 2.5 MiB. Oldest data is discarded first.
The device constant is `LOGFILE_TOTAL_BYTES` in `firmware/export/logfile.h`;
the host constant is `TOTAL_BYTES` in `tools/herdremote/hrlogcap.py`.

## Device inventory

Paths below are relative to `/.rockbox`, except `/.scrobbler.log`.

| Family / writer | Previous policy | FU41 policy |
|---|---|---|
| `logs/tagcache.log`, `art.log`, `playback_errors.log`, `theme.log`, `tasks.log` / debug_log | 256 KiB plus four backups, checked before a batch; batch overshoot possible | Common 5 MiB cap |
| `logs/usb.log` / usb_log_file through debug_log | Same debug policy | Common cap, ring and existing flush schedule retained |
| `logs/battery.csv` / battery_log through debug_log | Same debug policy | Common cap, minute ring and storage-idle flush retained |
| `logs/mikey.log` / remote debug screen through debug_log | Same debug policy | Common cap, UI disk work relayed |
| `logs/metadata.log` / metadata parser | Direct uncapped append; only headers used debug_log | Parser callback routes all PodBox records through common cap |
| `user/playback.log` / audio playback | About 511 KiB at startup, unbounded numbered archives; no strict ongoing bound | Common cap, one `playback_0001.log` backup compatible with existing reader |
| `/.scrobbler.log` / audio playback | Unbounded | Common cap; each current/backup part carries Audioscrobbler header |
| `logs/buffer_damage.log` / buffering damage snapshot | Unbounded | Common cap on every snapshot line |
| `logs/upgrade.log` / library layout migration | Unbounded | Common cap |
| `remote-log.txt` / remote input screen | Unbounded append with descriptor held while screen open | Common cap; only actual records open/write/close |
| `logf.txt` / optional logf dump screen | Separate timestamped snapshots, at most the 16 KiB RAM ring each | One capped dump family; logf disabled in normal build |
| `rockbox_log.txt` / optional LOGDISKF | Unbounded repeated RAM-buffer flush | Common cap; LOGDISKF disabled in normal build |

`playback.log`, `tagcache.log`, `artcache.log`, `usb-log.txt` and
`buffer-damage.log` at their old locations are migration inputs, not additional
writers. `stage0.log` is obsolete and appears only in the migration's dead-name
list. Crash and hang handlers display a bounded RAM/LCD report; they do not
append disk dumps. Cider/localstore diagnostics use RAM counters/UI, the remote
LOG stream (host cap below), and the existing debug log families; cache packs,
SQLite/index files, and persisted artwork are data, not logs.

The append search also finds playlists, shortcuts, database modification
exports, resume/settings files and unreadable-file indexes. These are user data,
not diagnostic logs, and are not truncated. Tuner/SPC/GME profiling and other
non-iPod target debug writers are not enabled in the normal ipod6g build.

The first actual write normalizes old oversized files and removes old debug
`.2`--`.4` backups. For old numbered playback history, it adopts the newest
numbered archive as `playback_0001.log` and removes the older archives. No
startup sweep or periodic disk-waking rotation is introduced. Old deployment
files are normalized when their owning log next writes, not before.

## Rotation and threads

`firmware/logfile.c` serializes open/write/rotation/close with a mutex. A
512-byte static copy buffer and static path storage are shared under that lock.
A full part is copied to the single backup and synced before current is
truncated. Current's path and inode remain available throughout. If copying or
syncing fails, current is not truncated. After power loss, the latest completed
old data remains in current or the synced backup; the next write can resume.
Neither a rename gap nor a large temporary third copy is required.

Main/UI-thread operations use a synchronous queue to the existing storage thread. The UI
blocks/yields; rotation is not performed in button/statusbar code or on the UI
stack. Storage callbacks execute directly, avoiding a queue wait on a worker
that could itself need storage. No IRQ writer calls this file API; existing IRQ
and remote producers continue to use their RAM rings. The relay adds no thread, stack or thread slot. Every copy loop yields
periodically. The new event wakes storage only for an actual log operation;
USB disk ownership rejects UI opens/writes without file I/O; close still
releases sessions opened before ownership changed. Rotation occurs
only while a logger is already writing. It reads/writes at most one half-window,
once per filled part; normal records do not copy an existing log.

Unwritten buffers and failed disk operations are not power-loss durable. The
cap does not promise to retain a record whose write failed. Headers/session
state are not a substitute for the newest diagnostic lines. Generic debug
parts may begin with continuation lines; the full session header remains in
an older part until that part expires. Audioscrobbler gets its required header
in every part, including rotation within a single buffered flush.

## Host inventory and service journal

`hrremotelog.Writer` writes `~/.local/state/podbox/remote-live.log` (or its
configured override). Previously current and `.1` could each grow to 4 MiB,
with a further oversized-record gap. Both now use the common 5 MiB total cap.
`hrlogcap.append()` locks the persistent current inode with `flock`, serializes
threads/processes, uses a bounded 64 KiB copy buffer, syncs backup before
truncation, and leaves the current name present. The existing bounded background
queue and received timestamps are retained.

HerdRemote, usbkey and Cider host modules otherwise log to stdout/stderr;
Cider plugin JavaScript uses its host console. They do not create another
PodBox-owned file log. Usage JSON/lock files and Cider artwork/index stores are
data. Logs belonging to external agent applications are only read by PodBox.
No `hrcider*.py` or `cider-plugin` source is changed by FU41.

The shipped **user** services keep the shared journal. It is not a
PodBox-owned log file and FU41 does not claim a per-service 5 MiB journal quota.
The installed `systemd.exec(5)` documents these options:

- `LogRateLimitIntervalSec=` and `LogRateLimitBurst=` limit message rate, not
  retained bytes. They do not create a 5 MiB rolling window.
- `LogNamespace=` can isolate a journal store, but is supported only for system
  services, not the current per-user services. A separate journald instance
  would also require its own size policy and administrative configuration.
- `StandardOutput=append:...` redirects output but does not rotate/cap a file.
  It would need a dedicated logging pipe/collector or application handler,
  plus a deliberate stderr/error-reporting policy for both user services.

A namespace or collector/service migration is not a cheap file-cap change.
FU41 retains existing journal visibility rather than silently changing where
service failures are captured. Global journald configuration is unchanged. No
service is installed or restarted. The journal exception is the explicit
service-output exception allowed by the task card, not a claimed byte cap.
