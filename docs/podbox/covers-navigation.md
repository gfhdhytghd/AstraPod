# Cover Flow navigation and remote key ownership (FU43/FU45)

Triple Play on the Dock remote and the body SELECT+MENU chord are the same
Cover Flow toggle. They close an enclosing overlay/viewer before opening the
carousel, so the app buffer is released before it is reused. Cider is preferred
when available (the FU32 source rule); otherwise the local album carousel opens.

The next toggle in the carousel or its album track list returns to the cached
Agents view or WPS when that was the origin. Other origins return to WPS. Normal
MENU is separate: covers opened from WPS return to WPS, for both local and
Cider albums. Cider covers opened inside the browser return to that browser
level; MENU then ascends the browser to Home. Other entries retain their menu
parent. The cover visit owns and clears its album
and parent depths. It never leaves a synthetic Covers node in root history.
Artist Portraits uses the same shared-carousel toggle exit to WPS.

Each fresh Cider browser entry starts at its root and first row. Child-page
returns and QuickScreen keep the current selection within the same visit.
The Home Cider row follows the saved root-menu order. Customize Main Menu can
move or hide it. Availability only filters visibility; disconnect/reconnect
does not rewrite the order or unhide the row. A default/reset order still
places Cider after Music.

QuickScreen still receives Play as Select immediately. A separate physical-tap
observer recognizes the triple even if the first Select closes the overlay.
Double Play and native hold/keymaps retain their existing meanings.

## Dock and computer ownership

| Keys | Source with FU43 firmware |
| --- | --- |
| LEFT, RIGHT, PLAY | Dock, always; heartbeat never masks these keys |
| MENU, SELECT | Computer while its heartbeat is fresh; Dock after expiry |
| VOL+, VOL- | Existing computer forwarding and agents_volume.c policy |

The host forwards no EP0 key IDs 6, 7, or 8. The firmware still validates and
ACKs those IDs from old hosts, but never injects them. Deduplication uses strict
source ownership rather than a time window: a late PC copy cannot repeat or
extend a Dock press. Blocking the computer IR input does not disable Dock
LEFT/RIGHT/PLAY. A live computer heartbeat still masks Dock MENU/SELECT.
The Disk Mode long-MENU request path is unchanged. The IR receiver stays
read-only.

## Heartbeat compatibility

The FU43 host sends heartbeat profile 1 (presence), once per second. FU43 firmware
accepts profiles 1 and 2; either fresh profile masks only Dock MENU/SELECT.
The existing firmware bridge timeout is unchanged (3 seconds).

| Firmware | Host | Behavior |
| --- | --- | --- |
| FU43 | FU43 | Strict ownership above; all transport keys use Dock |
| FU43 | older profile-2 forwarder | Dock transport works; PC IDs 6/7/8 ACKed and discarded, without duplicates |
| older FU30/FU34 | FU43 | Profile 1 preserves Dock transport; old firmware cannot selectively mask Dock MENU/SELECT, so strict MENU/SELECT ownership requires FU43 firmware |
| older | older | Existing behavior, including profile-2 Dock masking |

Install the paired firmware and host for the full routing policy. This package
was built and checked offline. Physical Dock timing and device UI acceptance
were not performed under the task's no-device-access rule.
