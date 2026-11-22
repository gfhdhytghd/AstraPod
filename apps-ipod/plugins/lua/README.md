# Bundled Lua compatibility

`entries.def` drives the bundle and the viewer's exact-path allowlist. The
viewer accepts the three games and six entries from the preceding milestone,
plus the sixteen entries below. Lua Scripts uses the same list. Selecting an
entry from it closes the current Lua state before starting the next script.

Native playback stays active while the viewer owns the display. The playback
event carries a track-change notification. The firmware callback only records
a sequence number. Lua receives the notification on the foreground thread at
its next sleep or yield. Multiple changes between dispatches produce one
notification. Timers and action handlers use the same foreground dispatch.
Storage takeover removes the subscription and closes the viewer's files.

The settings dump includes the reviewed list and playback fields and the
current volume, audio status and playlist length. The track snapshot copies
tags and times into Lua values. Lua never receives settings offsets,
metadata pointers or database paths. Library lookups use native tag queries.
Image loads use the checked native BMP reader or the native album-art cache.
The new scripts write fixed output names through atomic sibling files.

## Entry and device acceptance rows

Use an ipod6g with two local tagged tracks, a built Database, cached artwork
for one track, a small BMP, a text fixture and a binary fixture. Start native
playback before testing the playback rows. These rows require physical
acceptance.

| Entry | Device action and expected result |
| --- | --- |
| `dump_rbsettings` | Open the snapshot. Check boolean shuffle and list-wrap values and the numeric volume. Reopen `lua-settings.txt`. |
| `fade2sleep` | Choose one minute with music playing. Observe a decreasing volume. Press Menu and check that the sleep timer remains active. Repeat with a volume change during the fade and check that the fade releases control. |
| `file_browser` | Browse a directory under each sort mode. Select a file and check its size, modification time and extension. |
| `fileview` | Open the text and binary fixtures. Check text pagination and hex offsets. Return from each view. |
| `lua_scripts` | Select Hello World, exit, then reopen the chooser and select a game. Check that both return to the plugin browser. |
| `memchk` | Run the allocation probe. Close its memory summary and reopen the entry. |
| `playback` | Pause and resume. Skip to the other track and hold a direction to seek. Check the title and elapsed time, then return to Now Playing. |
| `print_lua_func` | Open the `rb` and `rockev` function lists. Close each list and exit. |
| `random_playlist` | Generate a playlist and check its track paths. Reopen it and choose Play now. Check Now Playing and track advance. |
| `rb_poly` | Change scale, wait for automatic rotation, press Menu and close the memory summary. |
| `rlimg` | Place the fixture at `/image.bmp`. Run Image Rotation and Save Screen. Reopen and check the saved BMP. |
| `stars` | Move, leave with Menu and reopen. Check the saved position and the restoration of backlight settings. |
| `submenu_demo` | Open both submenus and check their values. Return to the first menu and exit. |
| `tagnav` | Select a library tag and save `luatagnav-draft.config`. Check the draft and the current native tagnavi configuration. |
| `time_date` | Compare the running time with the device clock. Press a button and check the date table. |
| `track_metadata` | Compare the track dump with Now Playing. Export the cached cover and open the BMP. |

| Cross-entry gate | Device action and expected result |
| --- | --- |
| Existing entries | Launch all nine entries from their original categories. Exercise the three games and return to the plugin browser. |
| Playback continuity | Play music through the graphics demos and browse a file. Check audio continuity and return to Now Playing. |
| USB charge-only | Attach USB during Starfield and choose charge only. Check that input, drawing and playback continue. |
| USB storage | Attach USB during Playback, Starfield and a native menu. Accept storage. Check that Lua exits, that the host can mount the disk and that no incomplete sibling write replaced an output. Disconnect and reopen the entries. |
| Remote VOL | Use VOL+ and VOL− taps of at most 40 ms to verify single steps during native playback, during Lua Playback and after exit. |
| Repeated entry | Repeat Playback → Menu twenty times with music playing. Check that track updates remain single, the UI returns and other Lua entries still open. |

Lua Playback controls the native queue. Random Playlist can replace that
queue only after saving its own M3U and receiving a Play now selection. Library
Tags creates a draft and leaves activation to the owner. The file browser
sorts up to 512 entries per directory. File View reads text or hex in pages.
