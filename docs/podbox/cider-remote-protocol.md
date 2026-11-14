# Cider Remote v1: resident data channel and cache

This is an internal protocol and implementation reference. FU23 uses the
system WPS, native list widgets/search dialog/header and native carousel.
The Scrim skin files are unchanged. There is no separate Cider playing page.
The transport remains owned by the existing HerdRemote session.

## Ownership and connection

HerdRemote owns `hrcider.py` and `hrcider_cache.py` in its existing resident
PodLink session. Only the computer calls Cider or downloads artwork. The player
holds cached views and native bitmaps. No Cider plugin or new service is used.

The Cider worker starts after the player advertises `ci`. Legacy firmware does
not start it. Older daemons ignore `ci`; native screens remain available.
Cider records have their own session generation and do not require an Agents
foreground `hr` handshake. Disconnect/reconnect invalidates pending requests,
credits and controls, while validated memory/disk artwork survives.

The player sends `ci<TAB>1<TAB>window`. The host selects a fresh nonzero 32-bit
session generation and sends `CA`. The legacy inbox profile allows at most
24 KiB outstanding. The resident profile advertises 128 KiB. **The larger
profile bypasses the 16-frame/32-KiB foreground inbox entirely:**
`podlink_service.c` calls the resident receiver before storing foreground
messages. Known Cider state records and binary artwork go to that receiver.
Unknown/legacy records retain their original route.

The resident receiver and its outgoing mailbox share the service owner; they
never call PodLink recursively. The UI only copies a state snapshot or a
completed bitmap. Disk reads/writes run on the low-priority Cider cache worker.

## Records and priorities

One record is one PodLink TEXT payload. Fields use tabs, UTF-8 text fields have
control characters removed and byte limits. All numeric ids are unsigned.
`gen` is the current session generation; a mismatched generation is ignored.
Request ids are unique within that generation; zero is reserved for cancel-all.

| Player -> host | Meaning |
| --- | --- |
| `ci 1 window [capabilities]` | Start/restart this capability; discard old requests/credits |
| `cg gen bytes` | Add byte credits, capped at the negotiated window |
| `cr gen req art-id tier offset priority distance` | One whole cover, starting at offset, no existence probe |
| `cb gen req art-id,art-id,...` | Pull up to five XS covers in one range message |
| `cx gen req` | Cancel queued/fetching/streaming work for one id; id 0 cancels all artwork |
| `cc gen req command value` | Explicit control; never retry a timed-out control |
| `cp gen req kind offset limit library-gen` | Cached library page, limit 1..50 |
| `ct gen` | Request a rate-limited full metadata rescan |

Queue classes, in order: control reply (0), now-playing/availability/library
revision (1), visible list page (2), visible art (3), idle prefetch (4). Within
artwork, distance from center wins; equal-priority chunks retain their original
cover order. Consecutive chunks complete a cover without per-chunk requests.
Controls/state may preempt between chunks. Queues are bounded, canceled jobs
are removed, and a result from an obsolete fetch token is discarded.

Small state records are coalesced to at most 4 Hz. Bulk frames consume their
**entire PodLink payload length**, including the artwork header, from credits.
No large data is pushed without a pull request. Credits return only after the
resident receiver has consumed the bytes. A canceled same-generation frame is
consumed and refunded; an obsolete-generation frame cannot mutate the picture.
The 128-KiB grant fits an entire L cover or three M covers without a grant round
trip. Credits do not allocate a second 128-KiB queue: chunks copy directly into
the bounded assembly buffer.

Host -> player:

```
CA gen available reason
CN gen seq host-ms duration-ms position-ms rate volume shuffle repeat track-id art-id bg title artist album
CG gen library-generation
CK gen req ok message
CL gen req library-generation offset total possibly-stale count
id title subtitle art-id duration-ms track-count
...
```

`rate` is 0 or 1000; `volume` is 0..100; shuffle is 0/1 and repeat is 0/1/2
(none/one/all; Rockbox numbering in the optional 19th field, see FU26). `CN` appends `muted` (0/1) and composer after album in FU23.
`host-ms` is monotonic milliseconds modulo 2^32. The player
anchors at reception and extrapolates position locally; no per-second position
traffic is needed. Polling compares position against the last published
anchor, sends real changes/seeks, and sends a 15-second liveness anchor.
Unavailability disables Cider controls and retains its last track as paused.
It never changes the selected playback source or starts local playback.
Now-playing is **never** loaded from the persistent cache.

List ids are preserved, with compact title/subtitle fields. A frame returns as
many rows as fit, at most 50; the next offset is `offset + count`. Each page
carries its library generation. A future list view discards pages older than
`CG`, reissues at the new generation, and prefetches the next page near its end.
FU23 sends `cq gen req offset limit route` for native Music pages. Routes are
bounded escaped tree paths, not arbitrary Apple API URLs. `CL` rows append kind,
child route, bgColor and year to their original six fields. Replies may contain
fewer than 50 rows to fit the frame; continue at offset + count. Three 50-row
slots serve the native list/carousel. Prefetch starts eight rows before the end.
A library generation change invalidates all old pages. Superseded unstarted
host requests and obsolete in-flight results are dropped. A dedicated page
worker prevents background library sync HTTP from blocking visible queries.

## Artwork identity, geometry and framing

`art-id = SHA-256(exact artwork URL template)`, lowercase hex. Cache keys also
include the tier and the native format contract. URL placeholders request
**240x240**. Existing numeric dimension variants are rewritten to 240x240;
URLs without a capped variant are refused. Downloads are bounded to 2 MiB and
images exceeding 240 pixels on either side are refused before persistence.
The host's "original" is this capped download, never a 4000-pixel original.

| Tier | Dimensions | Pixels | Native payload including header |
| --- | --- | --- | --- |
| L | 240x240 | 115,200 bytes | 115,212 bytes |
| M | 128x128 | 32,768 bytes | 32,780 bytes |
| S | 56x56 | 6,272 bytes | 6,284 bytes |
| XS | 32x32 | 2,048 bytes | 2,060 bytes |

All classes derive from the capped source using Pillow, with a small ordered
dither before RGB565 quantization. M replaces the preliminary 140-pixel tier.
M is **column-major**, matching the native carousel. Other tiers are row-major.
No JPEG decode or dither runs on the player. L/M arrive ready to draw.
The native carousel uses pixel dimensions as physical slide dimensions, so its
XS adapter expands 32px rows into a 128px nearest-neighbor column surface on
the loader thread. This keeps the same geometry while sending only 2060 bytes;
M needs no scaling or transpose. The same native renderer draws every tier.

The payload is exactly `struct art_cache_header` (six little-endian u16 fields)
then native little-endian RGB565 pixels: magic 0x5441, version 8, width, height,
layout (M=1, others=0), zero pad. A native format change requires the matching
host renderer/cache schema to change too. Native local WPS caches are 300px
(`art_sizes.h`); that is unchanged. The system WPS passes the L240 bitmap through the same skin slots,
filter chains, dynamic-color extraction, alignment and drawing as local art.
Scrim's unchanged `%Cl` is 240 pixels. Local WPS's cached source remains 300px.
Lists downsample S56 into the native requested box (Scrim rows/header are 44px).

A normal artwork PodLink BINARY payload begins with little-endian `<4s6I>`:

```
"CAR1", gen, req, byte-offset, whole-payload-size, whole-CRC32, chunk-CRC32
<bytes>
```

The whole payload includes the native header. Maximum **frame payload** is
12,288 bytes, leaving at most 12,260 data bytes. Both CRCs are checked; offsets,
request id, generation, geometry/layout/version and total bounds must match.
An interrupted request can resume at its retained byte offset. Reconnect
drops an incomplete assembly; completed cache entries remain.

An XS range uses `"CXS1", gen, req, count` (`<4sIII>`), followed by each entry's
32-byte art-id, u32 size, u32 CRC32, and native XS payload. Five entries occupy
10,516 bytes. `bg` is an immediate solid placeholder; XS upgrades it during
fast scrolling. The resident receiver validates entries before caching them.

## Cache budgets, durability and synchronization

Host root: `$XDG_CACHE_HOME/podbox-cider/`, default `~/.cache/podbox-cider/`,
mode 0700. SQLite holds albums, songs/tracks, playlists and artists: stable ids,
names, subtitle, counts, durations, dateAdded, artwork templates and generation.
The database schema is versioned; an incompatible schema rebuilds the cache.
WAL/transactions protect metadata. Artwork uses fsynced temp files plus atomic
rename, with indexed LRU eviction. Orphan/temp recovery runs on cache open.
The default art cap is **500 MiB**, including capped originals and rendered
L/M/S/XS files. API tokens are never persisted or sent to the CDN.

Cache reads precede network fills. Cached pages and bitmaps remain usable when
Cider is slow or restarting; pages carry a possibly-stale flag. Control still
requires live Cider. Initial metadata indexing fetches 50 items per request,
at most one request per second. Incremental probes run every 180 seconds,
compare counts and first-page attribute/dateAdded fingerprints, and rescan a
changed kind. A complete background scan also runs every 15 minutes; `ct`
requests it on demand. Only a completed scan removes missing ids. Errors back
off to 300 seconds; an unsupported kind cannot starve the others. Link reads
and sync/art workers share guarded SQLite access, with no HTTP under that lock.

Player RAM: 512-KiB compact LRU arena with 24 descriptors; 115,212-byte assembly;
115,712-byte disk I/O buffer (FU25); 115,200-byte screen bitmap; bounded commands and
worker stack. Module and combined compile-time assertions cap the FU23 Cider-owned
static RAM at **1152 KiB**, including WPS data, three pages, list history/row
art and the carousel provider. The native carousel reuses its existing app
buffer and renderer; the Cider provider does not claim another app buffer. None uses
`app_buffer`. The arena can hold one L plus nine M and nine XS covers together.
Only complete validated bitmaps enter it. Generation changes retain those
entries while invalidating assembly and requests.

Persistent player art lives in the **iPod-local store** (FU25), below.
The FU23 per-cover files (`/.rockbox/cider-cache/`, `.../cider-v1/`) are no
longer read or written; they can be deleted by hand and are never migrated.
Local-library art caches are untouched.

## iPod-local cover store (FU25)

Goal: a cover the player has seen once, and in the background the whole
library at the browse tiers, loads from the player's own disk through an O(1)
hash index with no directory scan and no USB round trip. USB serves misses.

### Files

`/.rockbox/cider-art/` holds `index.cst` and `p00.pak` .. `p39.pak`.

- **Pack** (`pNN.pak`, at most 32 MiB, append only): sector-aligned records. A
  record is a 64-byte self-describing head (magic, per-session sequence, 32-byte
  art-id, tier, length, payload CRC-32, session, head CRC) then the validated
  `.aat` payload (the same `struct art_cache_header` format). L = 226 sectors,
  M = 65, S = 13, XS = 5.
- **Index** (`index.cst`): one header sector (magic, epoch, session counter, CRC)
  and 4096 bucket sectors (2 MiB). A bucket holds 24 entries of 20 bytes: first
  8 bytes of the art-id, tier, payload CRC (the host's content version), pack,
  sector, last-use stamp. A lookup hashes (art-id, tier) to a bucket and reads
  **one sector**; a full bucket sets an overflow flag and the search continues
  in at most three following buckets, then the least recently used entry of the
  home bucket is replaced. 98,304 slots; 49,152 entries maximum (50% load).
- **Why an on-disk table plus a RAM filter** and not a resident index: a
  resident entry needs at least 12 bytes; 1 GiB of M covers is about 32,000
  entries, so a full index is 380 KiB against 80 KiB of headroom in the
  1152 KiB Cider cap. The RAM part is a 32 KiB Bloom filter (3 hashes, 2.4% false
  positives at 30,000 entries, 8% at the 49,152 maximum). It answers "could the
  store hold this" without touching the disk. A false positive costs one sector
  read (and, when the disk is asleep, one spin-up that a visible cover waits
  for anyway); a false negative cannot happen after load.

### Durability

Every read verifies head CRC, key, tier, length and payload CRC; the index is a
cache of the packs. A record is appended and fsynced before its bucket is
updated, so a power cut leaves either a durable record without an index entry
(an orphan, reclaimed with its pack) or an entry whose record fails
verification (dropped on first read). Each bucket carries a CRC. A missing or
corrupt header, a wrong-size index, or one torn bucket recreates the table and
rebuilds it from the packs (records ordered by session/sequence, newest wins),
in bounded background steps, serving misses over USB meanwhile. A torn pack
tail is never appended behind. Load never panics: an unusable directory simply
leaves the store off. USB storage handover: the worker syncs, closes and
forgets the index before acknowledging `SYS_USB_CONNECTED`, and reloads lazily
afterwards, so files copied during Disk Mode are seen.

### Capacity and eviction

Default cap **1 GiB** of pack bytes (`CIDER_STORE_CAP`). Entries carry a
per-session use stamp (updated at most once per session, on a hit). When a put
would exceed the cap, the entry maximum or the pack count, the pack whose
newest stamp is oldest is evicted through the index (scrub in 64 KiB table
steps, then the file is deleted); the cover waits dirty in the RAM arena and is
written afterwards. Compaction (copy live records of a pack that is under 30%
live, then delete it) runs one bounded step at a time, only while the disk
spins and the UI has been idle for 5 seconds.

### Lookup policy

1. RAM arena (level 1, unchanged).
2. `cs_maybe()` (RAM only). Miss: the USB `cr` goes out at once, as before.
3. Hit: the worker reads the record (one table sector, one record read) and
   fills the arena; the USB request is **held** for `HOLD_ACTIVE` = 1 s when
   the disk spins and `HOLD_ASLEEP` = 2.5 s when it must spin up, then raced
   (whichever finishes first wins; a USB request that already left is
   cancelled on a local hit).
4. The visible window and neighbours (`cider_warm_art`, XS batches) that the
   filter says are local are removed from the `cw`/`cb` host requests and are
   read by the same worker pass, so they share one spin-up. A failed read is not
   retried for 30 seconds.

Only complete USB covers are written back, in batches of at most six, and
only while `storage_disk_is_active()` and the UI is idle: **a sleeping disk is
never woken for a background write, fill or maintenance step.** The store is
loaded when the disk already spins or when a visible lookup needs it. Apple
tokens and decoded audio never enter the store.

### Album track list: pinned L cover

Opening an album's track list from any entry (Albums list, Album Covers,
Artists, search, genre...) calls `cider_pin_art(art-id)`. The L (240x240) cover
is wanted at once: local store hit, else `cr` over USB and then written to the
store. It ranks **above idle fills and below visible list covers**: a visible
cover request replaces the transfer, and the pin restarts after 5 seconds
without one. The cached slot is **pinned in the RAM arena** and is never
evicted while the track list is open, however far the list scrolls. Leaving the
list (or showing another route) calls `cider_unpin_art()`. Now Playing asks for
the same art-id with `cider_request_art(id, "L")`, finds the pinned copy and
starts no transfer.

### Negotiation

The player advertises `localstore` in `ci`. The host answers with
`CF<TAB>gen<TAB>localstore` only if it supports the feature. Without `CF` the
player sends none of `ls`, `la`, `lw`, `lv` and ignores `CO`/`CQ`; the store
still serves local hits. A host without the feature never receives them; old
firmware never advertises it, so the host sends no `CF`, `CO` or `CQ`.

| Player -> host | Meaning |
| --- | --- |
| `ls gen epoch count digest used-kib cap-kib` | Store summary after load and after eviction/compaction. `digest` is the 32-bit sum over entries of CRC-32(key8 + tier byte + CRC LE); tier byte L=0, M=1, S=2, XS=3 |
| `lv gen cursor next count` + `count` rows | One inventory page (16 rows). Row: 16 hex key prefix, tier digit, 8 hex CRC, newline. `next` below or equal to `cursor` is a failure; `next >= 131072` ends the list |
| `la gen art-id tier crc` | The cover (and its CRC) is now in the store |
| `lw gen n` | Idle player (disk spinning, UI idle, store has room) wants up to `n` offers; at most one per 4 s |

| Host -> player | Meaning |
| --- | --- |
| `CF gen localstore` | Feature accepted |
| `CQ gen cursor` | Send inventory page at `cursor` (bucket<<5 \| slot); served only while the disk spins |
| `CO gen tier art-id,...` | Offer up to five covers of one tier. The player pulls them with a normal `cr` at **priority 4** (idle), one at a time |

### Host ledger, diff and push

`hrcider_store.py` keeps SQLite tables `ipod_art` (key prefix, tier, CRC) and
`ipod_meta` (epoch). On each `ls` the ledger is compared with the player's
(epoch, count, digest): equal means no listing; different means a paged `CQ`
listing that replaces the ledger only if the finished listing matches the
announced count and digest. Offers are made only against a verified ledger.
Covers the player acknowledges (`la`) are added; evictions are discovered by
the next `ls` mismatch.

The planner (`Planner.next`) offers: **L** for now playing and the queue
neighbours first, then **XS**, **M**, **S** for every artwork id in the indexed
library (id order). A cover is offered when the ledger lacks it or holds a
different CRC than the host's rendered file (changed render); a held cover the
host never rendered is not downloaded just to compare. Offered covers are
prepared by the existing prefetch workers so the player's pull is a cache read.
Passes rescan after 60 seconds; an offer is not repeated for 60 seconds. The
host never sends art unasked: the player pulls only while it wants to write.

Player side, an offered fill is held in the arena marked `bg` (evicted first),
written to the store by the worker, acknowledged, and dropped from the arena.
Only one fill is in flight, and none starts while a cover is waiting to be
written, the UI used art in the last 3 seconds, a user request, command or
cancel is pending, the disk sleeps, or the store is full.

### Budgets

RAM (static, inside the 1152 KiB Cider cap, asserted at compile time): store
state 34,308 bytes (32 KiB Bloom filter, 40 pack descriptors, one 512-byte
bucket buffer), record buffer 115,712 bytes (was 115,212, now also the L record
sector padding), inventory/offer/ack/fill tables about 2.6 KiB. The disk worker
stack grew from 3 KiB to 6 KiB: its deepest own frames are 192 (worker) + 120
(read_store or cs_maintain) + 96 (cs_get) + 72 (index_put) + 56 (find) bytes by
`-fstack-usage`, plus the file system. The service-thread `next()` frame is
136 bytes (was 56). Disk: 1 GiB default cap plus the 2 MiB index.

Measured on the host (no device access): see the FU25 report for the host
reference numbers. Spin-up and USB timings on the iPod are unverified.

## Native Cover Flow provider

Reuse the existing renderer and slide slots. Native references:

- `metadata/art_sizes.h`: 128px `AA_COLUMNS` Cover Flow size, 300px local WPS.
- `metadata/art_cache.c`: pre-rendered `.aat` files; bounded/yielding generation;
  `aa_disk_asleep()` holds work for a sleeping disk.
- `screens/covers/carousel.c`: `read_aat()` performs one aligned whole-file
  read; `free_slide_prio()` evicts only a farther slide; `load_new_slide()`
  grows a contiguous run from center on the shorter side; its worker drains
  pending wakeups and rereads center for each load.

`cider_window()` and the host Window helper implement center-first, contiguous
nine-slide plans, with direction breaking equal-distance ties. At velocity
>=3 albums/second use XS/bg; at slower/stopped positions request M, and request
L only for Now Playing. Re-read center before selecting each missing cover.
Keep an in-flight cover if it remains in the window; otherwise cancel it.
The portable `cider_slide_slot()` admission helper refuses a farther replacement
and selects the farthest eligible slot; it does not clear the window on moves.
Never flush/re-request an unchanged cached window. Evict/cancel farthest-first
only to admit a nearer cover. Batch XS for the whole visible range and weight
the next prefetch range by direction/speed.

FU23 supplies a third `carousel_model`. Its nonblocking `read_art` callback
feeds ready M columns through `load_and_prepare_surface()` into the existing
buflib slide layout; XS is expanded to that same 128px layout and bgColor supplies
the instant placeholder. The existing renderer, animations, captions, keymap,
contiguous run and farthest eviction remain shared. Cache arrivals wake the
loader, which upgrades only changed visible slots in place, keeping the linked
run and retaining better M surfaces if the smaller resident art LRU evicts one.
Center moves cancel obsolete transfers but do not restart the native engine.
An identity/version change revalidates the affected slots; library count
changes use the native center/slot path, not nested carousel instances.

## Shuffle and repeat commands (FU26)

The player offers the native Rockbox set. Shuffle is Off/On. Repeat is, in
`repeat_mode` order, **0 Off, 1 All, 2 One, 3 Shuffle, 4 A-B**.

Commands (`cc gen req command V`), all explicit values so a retried press is
idempotent; invalid values are rejected without an RPC and answered `CK ... 0`:

| Command | Meaning |
| --- | --- |
| `shuffle V` | Cider shuffle, 0 off / 1 on |
| `repeat5 V` | Rockbox repeat mode 0..4 (see below) |
| `ab-mark MS` | A-B marker at player position `MS` (ms): sets A, then B, then clears |
| `repeat V` | legacy (first FU26 firmware): Cider numbering 0 none, 1 one, 2 all |

Cider RPC (verified in Cider 4.0.0 `app.asar`; `repeatMode = {none:0, one:1,
all:2}`, `shuffleMode = 0|1`): `PUT /api/v2/playback/shuffle {"enabled":bool}`
and `PUT /api/v2/playback/repeat {"mode":"none"|"one"|"all"}`.

`repeat5` mapping: 0 -> repeat none; 1 -> all; 2 -> one. **3 (Shuffle)**: repeat
all and shuffle on; the host remembers the earlier shuffle and restores it on
leaving. **4 (A-B)**: Cider is left unchanged; the host keeps markers A and B
for the current track. SELECT on Now Playing in A-B mode sends `ab-mark` with
the current position (first A, then B, then clear; Rockbox's single-key cycle).
Once B is set the host seeks to A, and whenever playback reaches B on the same
track it seeks back to A (polling every 250 ms while active). A track change
clears the markers; the mode stays. Modes 3 and 4 are virtual: when
Cider's own repeat or shuffle no longer equals what the host set (the user
changed it in Cider), the virtual mode ends and the real value is reported.

Reporting: if the player advertised both `cn18` and `modes5` in `ci`, `CN` has a 19th field,
the repeat mode in the numbering above. Without it (older firmware) `CN` is
unchanged, with the 10th field in Cider's numbering. A player reading a
16/18-field `CN` converts Cider numbering to Rockbox's (none 0, one 2, all 1).
`repeat5` and `ab-mark` need a host that knows them; an older host answers
`CK ... 0` and the player shows Cider's own value again.

On the player, long MENU on Now Playing opens the system quick screen with
Left = Shuffle and Right = Repeat (all five values, left/right press cycles
backwards/forwards), top and bottom empty; long SELECT opens a native list with
`Shuffle` and `Repeat` rows (SELECT cycles, MENU leaves). MENU, tapped after
the opening press has been released, also leaves the quick screen; in the
quick screen the remote's Vol+/Vol- act as the wheel (volume), Left/Right
change the values, and Select or the centre key leave. Both pages act only
while Cider is the source and available and never touch the local playlist or
settings. A requested value is shown at once, and replaced by Cider's value
when `CN` agrees, when `CK` reports failure, or after 2 seconds. Local playback
keeps its own long-press menus.

## Playing a song inside its list (FU26c)

Choosing a song in an album, playlist, Songs list, artist/genre list or search
result queues the list, like the native iPod: the queue is the list in row order,
the current track is the chosen one, and Next/Previous walk the list.

The player sends `cc gen req native-context INDEX|LIST-ROUTE` (`LIST-ROUTE` is the
page route the row came from, `INDEX` its row number) immediately before the usual
`native-play play/<kind>/<id>`. The host keeps the context for 15 seconds and uses
it once. A host that does not know `native-context` answers `CK ... 0` for it and
plays the song alone, as before. Any failure while queueing also leaves the song
playing alone.

- **Album and playlist routes** (`album/ID`, `album/catalog-REGION/ID`, `playlist/...`):
  `POST /api/v2/playback/play-collection {"type":"library-albums|albums|library-playlists|playlists","id":ID}`
  queues the whole collection (Cider loads the first 100 tracks and the rest in
  the background). The host waits until the row exists, finds the song **by track
  id** in the queue (Cider drops unplayable tracks, so row numbers can shift) and
  calls `POST /api/v2/queue/jump {"index":N}`. The first track of the collection
  is audible for a moment before the jump; Cider has no start-index option.
- **Any other list** (Songs, filters, artist songs, history, search `search?q=`):
  the song starts at once with `play-item-href`, then a background worker
  appends the next 20 rows with `POST /api/v2/queue/add-later {"type","id"}`, one
  at a time, waiting for each to appear (concurrent adds arrive out of order),
  then appends the previous 5 and moves them in front with
  `POST /api/v2/queue/move {"from","to"}` (Cider keeps the playing position). A new
  play cancels the worker. Rows further than that window are not queued.

Verified on Cider 4.0.0 (`queue` GET/`position`, `jump`, `add-later`, `move`,
`play-collection`). Cider's `/previous` already restarts the track when more than
3 seconds in and goes to the previous track otherwise, as the native iPod does.

## System source and native Music menu

There is one system Now Playing route (`GO_TO_WPS`). `GO_TO_CIDER` now opens
only the Home Cider Remote menu. Source is RAM state, Local after reboot.
Starting a Cider song/album/playlist/random/shuffle pauses the local queue
without clearing it and selects Cider. Native `audio_play`/`audio_resume`
switch to Local; the host checks live Cider play state and pauses it once if
playing. PLAY on a paused local queue outside the WPS selects Local too.
Home Now Playing, list PLAY shortcuts and Agents' double/chord all open the
same system WPS. Disconnect retains Cider metadata and source, with a paused
icon and inactive controls. Reconnect refreshes it; it never auto-starts Local.

Home shows Cider Remote as its second visible row only while the connected
host reports Cider available. The row has the theme's music icon. It is
excluded from the editable Home rows. Its native list adds Album Covers first,
Playlists second, and the native Now Playing/Resume action, then enumerates the
actual parsed tagnavi main rows using their native names and hidden mask.
Native menu order/filtering is not duplicated in a second firmware table.
Every directory reuses one native list instance with 12 bounded history slots.
Album tracks feed the native `%LH` header and the skin's list art callback.
Search uses the native edit-line/voice search dialog, with async result refresh.
Search-by filters reuse the native keyboard; ranges ask start/end separately.

The host SQLite index supplies albums/songs/playlists and derives album artists,
artists, genre, release year and composers. Album membership uses Apple's
stable library-album relationship, with name fallback when none is available.
Album artist falls back to artistName when Apple omits albumArtistName.
Missing composer/release data produces native empty lists. Featured Artists
ports the native byte/marker/bracket/known-band rules; parity tests compare
against the real native parser. Filename maps to title. Search-by Filename,
Score and Comment are omitted. Search-by rating maps Apple's loved/disliked
values to 1/2 (0 unrated); the background index reads catalog ratings in bounded
50-id batches. Native play counts/scores/forgotten/favourite charts cannot be
derived and are omitted. History uses recent tracks/resources; recent artists
are deduplicated from recent tracks, not a fabricated play-count chart.
Unsupported custom native queries and data without an Apple equivalent show
native empty lists. First Letter uses native Numeric, A–Z and Special Character
buckets. Track labels match fmt_title/title-with-duration; filename is title.
Search is bounded to the first 50 matches/type from Apple's three library
result types; the native search dialog shows its first 50 combined matches.

## Cider API and acceptance

The installed Cider 4.0.0 app.asar is the source for these routes. Full source
hash and bounded excerpts are in the FU22 evidence (`cider-api-proof.json`).

| Method / path | Body / result |
| --- | --- |
| GET `/api/v1/playback/now-playing` | `info` attributes and playParams |
| GET `/api/v1/playback/is-playing` | `is_playing` |
| GET `/api/v2/playback/time` | `data.currentTime`, seconds (legacy `currentPlaybackTime` also accepted) |
| GET `/api/v1/playback/repeat-mode`, `shuffle-mode` | `value` |
| POST `/api/v1/playback/playpause` | `{}` |
| POST `/api/v1/playback/next`, `previous` | `{}` |
| POST `/api/v1/playback/seek` | `{"position": seconds}` |
| GET `/api/v2/queue?offset=&limit=`, `/api/v2/queue/position` | `data.items[].track`, `data.position`/`total` |
| POST `/api/v2/queue/jump`, `/queue/add-later`, `/queue/move` | `{index}`, `{type,id}`, `{from,to}` |
| POST `/api/v2/playback/play-collection` | `{type,id}` |
| PUT `/api/v2/playback/shuffle` | `{"enabled": bool}`; replies `{"data":{"enabled":bool}}` |
| PUT `/api/v2/playback/repeat` | `{"mode": "none"\|"one"\|"all"}`; replies `{"data":{"mode":...}}` |
| POST `/api/v1/playback/play-item` | `{"type": kind, "id": stable-id}` |
| POST `/api/v1/amapi/run-v3` | `{"path":"/v1/me/library/...?..."}` |

The optional token header is **`apptoken`**, supplied via `CIDER_API_TOKEN`.
`CIDER_API_URL` defaults to `http://127.0.0.1:10767`; only loopback RPC URLs are
accepted, and RPC ignores HTTP proxy environment settings. Socket.IO broadcasts
`API:Playback` with `{type,data}` messages (item/state/time changes); the time
event is approximately once per second. Step 1 uses the allowed <=1-second
REST polling fallback, with change detection, avoiding another socket dependency.
Pillow is already present in the build/test venv; no package installation is
needed for this work. A deployed daemon needs Pillow in its own environment.

Cider volume uses host `wpctl get-volume @DEFAULT_AUDIO_SINK@` and
`wpctl set-volume -l 1.0 @DEFAULT_AUDIO_SINK@ N%+` / `N%-`. Fallback when
wpctl is absent: pactl get-sink-volume/get-sink-mute/set-sink-volume on
`@DEFAULT_SINK@`, bounded to 100%. Accepted reports contribute ±1%, combined
in one pending delta instead of a command backlog. No Cider volume endpoint
is called, even for reads. Host volume/mute are polled with state about once
per second and coalesced. Local WPS volume and transport behavior stay local.
Native hold seek uses the existing acceleration/step settings and preview path,
sending one Cider seek on release and never seeking the paused local audio.
Unsupported local file/queue actions are no-ops; track information uses the
current source. Status-bar play mode, WPS tags, volume overlay and Home's
Now Playing label follow the source. Quick Settings' setting editors remain
local settings; they do not gain remote source semantics.

Control endpoint tests use an ephemeral loopback mock Cider server. The
permission update also permits direct Cider changes, with per-file backups
before mutation. Real read-only state/API and capped-CDN checks can supplement
the mocks; playback is not changed by those checks. The FU22 benchmark uses
real framing/CRC, a mock callback link and the 20-ms host pump, plus a host
filesystem one-open/one-read `.aat` reference. It does **not** measure USB,
iPod disk DMA, physical IR latency, audio or actual Cider rendering. Device
acceptance and native disk timings require separately authorized installation.


## Diagnostic capture

Enable **System > Debug > Remote log stream** after reboot. HerdRemote then
writes device `LOG CIDER` records to `~/.local/state/podbox/remote-live.log`:
request id/offset/tick, granted/outstanding credits, chunk bytes/result and
completion tick/size. The host journal also records fetch/cache-ready latency.
The existing bounded diagnostic ring reports loss; logging never holds up art
or foreground keepalives. No token, decoded audio or artwork pixels enter logs.


### Artwork prefetch and confirmed selection

`cw<TAB>generation<TAB>tier<TAB>comma-separated-art-keys` replaces the host's
visible artwork window (at most seven keys). Four bounded workers prepare the
window concurrently. They share one download per key across native tiers.
Queued work outside the newest window is removed; each original remains capped
at 240 pixels. An empty window cancels queued visible prefetch.

`CP<TAB>generation<TAB>current-track-id<TAB>previous-art-key<TAB>next-art-key`
publishes artwork neighbors from Cider's actual playback queue, including its
shuffle order. Missing neighbors use `-`. The receiver accepts only its current
generation and track ID. The WPS loads the current L240 first, then retains and
preloads the previous and next L240 covers in device RAM. No local playlist is
used to guess the Cider queue. Host queue reads do not control playback.

Library selections resolve complete resources through Cider's `play-item-href`
route. The ID-only `play-item` route can silently reject tracks without
`attributes.playParams`. For songs, the host confirms that now-playing matches
the selected library/catalog ID before sending successful `CK`. The iPod waits
for this ACK before pausing Local and selecting Cider/opening WPS. Failure or
six-second timeout keeps the list and playback source; controls are never
retried automatically. A full command queue rejects selection explicitly.

MENU uses the native list's Menu action as Back. The cached return chain is
WPS -> album tracks -> Cider Cover Flow -> Cider Remote -> Home. Cover Flow
opened from WPS by a screen shortcut exits to Home; local source routes retain
their existing rules. WPS and Cover Flow shortcuts unwind before opening a new
Cider browser. The native Scrim skin is unchanged.

Artwork identity normalizes CDN templates and fixed-size URLs to the same
240-pixel URL, so a prefetched queue image is the next WPS image. Schema 3 is
migrated to schema 4 without discarding the library index or compatible native
files. The cache keeps atomic, fsynced art writes and WAL transactions; NORMAL
SQLite sync avoids flushing each derived-cache metadata change. Full-directory
orphan scans run at startup/eviction, not for every new cover.

Cover Flow preserves the remote-triple sentinel separately from MENU. A triple
returns to its WPS/Agents origin (or the Cider list); an ordinary MENU uses the
back chain. Returning to WPS by triple keeps the remote MENU-to-Cover rule.

## Combined local store and controls (FU27)

The player advertises `cn18,neighbors,warm,play-ack,localstore,modes5`.
`localstore` does not change the CN field count: legacy CN has 16 fields,
`cn18` enables 18, and `cn18` plus `modes5` enables 19.

Player output order is queued controls (including the adjacent
`native-context` / `native-play` pair), artwork cancellation, library requests,
credit, visible/pinned artwork requests, warm hints, then store records
(`ls`, `lv`, `la`, `lw`). Idle fills use artwork priority 4; visible/pinned
requests use priority 3. A queued control prevents an idle fill from starting.

## FU28: inferred loading and the transport slot

`loading` in `ci` opts into a **20-field CN**. Fields 1..19 keep the FU27
meaning (including muted/composer/native repeat); field 20 is `loading`, `0|1`.
Advertising `loading` alone implies that complete prefix, so field counts remain
unambiguous. Without it, the old 16/18/19-field negotiation is unchanged.
Firmware accepts all four counts and clears loading on an older CN, unavailable
CA, or session reset. New firmware advertises
`cn18,neighbors,warm,play-ack,localstore,modes5,loading`.

The host polls at 250 ms. It infers loading; Cider provides no buffering flag.
Before a track-changing command's RPC it publishes loading immediately. It
waits for an ID different from the pre-command ID, then forward progress within
that new ID. Seek waits for a sample within 2 s of the requested point, then
forward progress. Pending operations expire after 8 s. Independently, playing
with no position change for 1 s is a stall. Progress, pause or stop clears a
stall. A failed pending command cancels its inference. Each loading transition
forces a state publication past the usual 250 ms throttle. A sample started
before a command cannot complete that command's inference. This does not alter
play-ack's separate selected-song check.

The native Scrim WPS disables the built-in status bar (`%wd`). Its `%mp`
conditionals select 24x24 font glyphs, rather than an image strip. FU28 splits
the original `(2,2,76,24)` icon viewport into a transport viewport `(2,2,24,24)`
and the unchanged adjacent icons at `(26,2,52,24)`. In the transport viewport,
`%?pZ<%pZ|normal-glyph-conditionals>` replaces only the transport glyph while
Cider is active, available and loading. FF/RW keep priority. The false branch
preserves both play-pause preference choices and local playback.

`%pZ` marks an isolated slot. Its loading-only refresh traverses only marked,
visible viewports, clears the line to the existing backdrop, draws the spinner
after the clear, and flushes only that viewport. The existing WPS action loop
uses timed waits to the next 125 ms frame boundary (12/13 ticks at HZ=100).
It draws eight clockwise positions with four visible dots at 100/75/50/25%
opacity, quantized to the LCD's 4-bit alpha (15/11/8/4). Boxes under 7 px use
four corners. No worker, pixel cache, or allocation is added. A theme using an
image strip can use the same isolated viewport and `%?pZ<%pZ|%xd(...)>`;
loading is overlaid procedurally and consumes no image-strip frame. Themes
without `%pZ` continue their original display. Hidden/auto-hidden slots and the
volume overlay retain their existing visibility. The compact transport status
is published as one aligned word; UI readers take no mutex.

## FU28b: loading accuracy, current-song navigation, and diagnostics

This supersedes FU28's inference rules above. `native-context` stores only a
list context and never starts loading. Before `native-play` or `play-item`, the
host reads an atomic `/api/v2/playback` snapshot and compares library/catalog
aliases. Selecting the current playing song performs no playback RPC. Selecting
the current paused song sends explicit `play`, with no seek, queue replacement
or loading. Firmware compares decoded song-route IDs against the current CN ID
first; it opens WPS directly, or queues only `resume` for a paused current song.
The host guard covers old firmware and catalog/library aliases.

For a different song, firmware sets its compact loading flag at the action and
opens WPS immediately. This is a display preview: local audio ownership still
moves only on a successful selected-song ACK. Next, previous and seek also set
the local flag before transport. A failed matching command clears that flag.
CN receive and loading-draw transitions are recorded in the existing remote log.
The WPS loop checks transitions at most every 20 ms while keeping spinner head
animation at eight frames per second. It does not wait for a one-second skin
refresh, create a thread, or spin without a timed wait.

The host observes Cider's public `API:Playback` Socket.IO events through the
existing `websockets>=13` dependency. An independent `pactl subscribe` listener
reads only Cider's sink-input state, stream IDs and `corked`; it never opens a
source/monitor or reads PCM. Streams are matched by Cider's process binary or
application name, excluding other applications. Missing sources leave position
polling available. Injected Client instances disable these external monitors by
default; an integration can opt in with `realtime=True`.

A pending play has an expected song-ID set. A collection's first-track blip
cannot complete a jump to another song, including when the first track restarts
the previous ID. Sending the final queue-jump resets the completion gate. A
matching stream start can clear loading without waiting for Cider's ~1 Hz clock.
Without a stream edge, track completion requires a reset/start clock and forward
progress for the expected ID. A new metadata label plus the previous audio's
continuing clock cannot clear loading. A same-ID restart followed by progress
can complete an operation whose target permits that ID. Seek uses the requested
point (250 ms tolerance), then a stream edge or forward progress. Timeout remains
8 s. While Pulse reports running, stall detection allows 2.5 s for Cider's coarse
clock; with a corked/missing stream it retains the 1 s threshold.

INFO timelines include monotonic timestamps, command/epoch, RPC send/return,
selected-ID changes, first progress, every loading transition and every CN push
(including loading=0). Stream transitions and public Cider event names are logged
without audio data. Loading transitions force CN publication past the usual
250 ms throttle. CN format and capability negotiation remain FU28's 20 fields.

Accuracy boundary: Cider can keep a stream running across pauses, skips and
seeks, and can change public now-playing metadata while the audio clock still
belongs to the prior item. Such a window has no independently observable audio
start in Pulse state. The clock fallback is an inference, not proof of a 250 ms
end offset. FU28b live evidence separates these cases from observed stream
starts; the report records the remaining acceptance gate.

## FU30: native context, cover diagnostics and explicit library fill

The Cider WPS uses the native menu widget and native strings/order for Shuffle,
Repeat, Rating, Track Info, Go to Album, Go to Artist, Lyrics, Sleep Timer,
Speed, Current Playlist (View, Playing Time), Theme options and More (View Album
Art, Sound Settings -> Volume). Local file/playlist, bookmark, cue, chapter,
Sound Mix, Spike, EQ and playback-settings operations are omitted. The timer is
native; its external-source expiry callback queues the idempotent Cider pause.
Track Info uses native field labels with Cider title/artist/album, disc/track,
year, genre, duration and audio-quality data. Main-list rating/library status is
read anew before an explicit mutation. Queue selection sends an absolute jump.

Read-only paged routes `now/info`, `now/album`, `now/artist`, `now/lyrics`,
`now/queue`, `now/time` use the existing `cq`/`CL` transport. The host obtains
current attributes, quality and queue data through Cider's read-only RPCs.
The installed Cider 4.0.0 `app.asar` and SPA RPC source expose these operations:

- POST `/api/v2/library/now-playing/love`; DELETE `/api/v2/library/now-playing/rating`.
- POST `/api/v2/library/now-playing/add`.
- GET `/api/v2/playback/audio-quality` and `/api/v2/playback/rate`.
- PATCH `/api/v2/playback/rate` with `rate` in 0.25..4.
- GET `/api/v1/lyrics/:id` (plain text or TTML is normalized to paged lines).
- GET `/api/v2/queue`, GET `/api/v2/queue/position`, POST `/api/v2/queue/jump`.
- POST `/api/v2/playback/pause` (also used by sleep expiry).

Firmware advertises `localdiag` in addition to FU27's six capabilities. The host
must confirm `CF <generation> localdiag` before firmware sends the extensions
below. Without confirmation firmware retains the eight-field `cr` and sends no
`ld`, `lr` or `lf`. This preserves basic art transport with an older host.
Separately, `CF <generation> localstore` starts store initialization even when
Cider is the only playback source and the disk is asleep.

Tab-separated records:

- `cr gen request key tier offset priority distance store_miss`: optional ninth
  field, 1 for a negative/missing local result, 0 when USB raced a pending local
  lookup. An old eight-field request logs `store_miss=unknown`.
- `lr gen request bytes`: complete payload consumed/CRC-checked by the device;
  distinct from durable `la`. Host INFO logs request->ready, request->ACK and
  ready->ACK wall times. Generation/request scope prevents cross-session ACKs.
- `ld gen loaded entries bloom_hits bloom_false_positives sectors hits misses
  writes idle_deferrals loads last_read_us max_read_us last_error`: best-effort,
  at most once per five seconds, below visible traffic. Times are measured on
  the device (USEC_TIMER on ipod6g), including lookup/read/CRC. Sector count
  includes index loading, inventory and data reads. This is independent of the
  optional Remote Log stream.
- `lf gen enabled`: explicit full-library fill start/stop from Cider > Cover
  cache or System > Debug > Cider cover store. This mode permits disk spin-up.
  Idle offers are polled every 100 ms while enabled; default background offers
  retain their four-second interval and already-active-disk policy.
- `CB gen library_revision done total state`: host progress; state 0 checking,
  1 incomplete, 2 complete, 3 cannot complete (missing album art or capacity).
  Counts include M+XS+S for each distinct library art key. Completion requires
  matching rendered CRCs in the device inventory or durable `la`, plus a
  completed library scan. Merely offered/transmitted images never count.
  Firmware rejects a different session/library revision; library invalidation
  clears its completion state. Completion/error stops explicit fill.

USB CAR1 and batched XS covers are dirty and wake the worker. Normal foreground
covers flush after a one-second idle deferral even if the disk was asleep.
Index creation and load run on the disk worker, never a button/statusbar path.
The first foreground request can wait for bootstrap before issuing USB. A loaded
store hit is looked up before USB; a complete-cache hit does not race USB.
Store reads no longer write LRU sectors. Pack recency remains resident.
Writes sync pack and index before a durable acknowledgement. Inventory status
is throttled and follows pending ACKs, avoiding a re-list on every fill.

Direction-aware M/S read-ahead is bounded by the 512 KiB arena, pinned L art,
slot count, a two-frame floor and the measured worst read time. Nine visible M
covers allow at most six additional M records without a pin. A stored M cover
is not newly rendered from the XS fallback; native slide caches and host-only
metadata paging remain limitations for the strict FU30 continuous-scroll gate.

**The strict zero-placeholder/zero-missing gate is not met by this candidate.**
`tools/tests/cider_scroll_bench.py --require-zero` replays 1,200 albums with the
production store/cache, injected disk timings and 30 Hz frame deadlines. It
passes the explicit flash model and fails the rotating-disk model. This is
store-side simulation, not device timing or a renderer/device acceptance claim.
An ordered per-tier strip and a local ordered album manifest are proposed in
FU30-REPORT.md; no scroll speed cap is introduced by this change.

WPS entry/reentry and session/availability changes seed the volume baseline.
Only a volume/mute change during consecutive visible WPS updates stamps `%mv`.

### FU31: independent session liveness and disconnected lists

The combined firmware advertises eight capabilities:
`cn18,neighbors,warm,play-ack,localstore,modes5,loading,localdiag-live`.
`localdiag-live` replaces the FU30 `localdiag` token and enables both its
unchanged diagnostic records and the following challenge. Keeping eight tokens
lets older hosts accept the handshake and ignore the unknown combined token.
New hosts still accept `localdiag` from older firmware without requiring a CH
response. The wire-level probe works with both firmware versions.

Host `CH\t<generation>\t<nonce>` has a fresh 16-character lowercase hexadecimal
nonce. The resident Cider client validates the generation and syntax and sends
`ch\t<generation>\t<nonce>` before art, page or control traffic. These records do
not spend bulk credits. A host sends at most one challenge per second and holds
only one pending challenge. Only its exact generation/nonce response clears the
three-second deadline. CN writes, pages, credits, foreground PINGs and old ACKs
cannot clear or postpone it. An idle browser therefore remains healthy without
page/art/control requests. After a valid CH, the device also treats the host as
disconnected if four seconds pass without another CH, resets the resident
session and sends a fresh ci. A changed generation resets pages and challenge
state; legacy hosts that never send CH keep their legacy timeout behavior.

HerdRemote separately sends a wire PING every second, even with incoming
traffic, and requires the same sequence and 16-byte random payload in PONG
within three seconds. A new peer HELLO/OPEN or a repeated ci in a negotiated
live session invalidates the old session immediately. Either deadline raises
the daemon's normal reconnect condition: close the old tty, reset session
state, rediscover and reopen (retry interval at most one second). This is a
protocol deadline, not a bound on OS open/write delays or physical enumeration.
The standalone podlink CLI retains its 5/15-second probe defaults and explicit
ping replies remain available to the CLI.

The browser shows `Cider not connected`, `Cider unavailable`, `Loading...`, or
`Cider list unavailable` for disconnected, unavailable, pending or stale-empty
results. `No File` is reserved for a confirmed zero-length, non-stale result.
History and the query survive reconnect so pages can be requested again.

FU31 storage target clarification: this user's 256 GB iPod uses flash storage.
The required host replay is `cider_scroll_bench.py <output> --model flash-assumed
--require-zero`. Its injected 0.15 ms sector latency and 12 MiB/s throughput are
assumptions, not device measurements. FU30's HDD-model failure is historical
characterization, not the target gate. A packed strip-file redesign is deferred.
Zero placeholders/missing covers on the physical device remains an open gate;
measure `read_us`/`max_read_us` and repeat all three views after installing and
fully building the library store.
