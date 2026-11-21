# ipod6g plugin compatibility tables

Include `tools/rock-compat/tables.make` in the SDK makefile. Add
`$(ROCK_COMPAT_TABLES)` to the SDK consumer prerequisites and
`-I$(ROCK_COMPAT_DIR)` to its include flags. The rule requires GNU make 4.3
or later. To generate directly from a configured AstraPod build:

```sh
python3 tools/rock-compat/generate.py --build "$BUILDDIR" --out "$BUILDDIR/rock-compat"
```

Build the ordinary fork `lang_enum.h` and `apps-ipod/genlang-features` first.
The generator runs Rockbox's genlang against both English catalogs. It checks
the fork enum against the configured build. The default upstream ipod6g text
profile includes the stock recording, pitch, video and utf8proc vocabulary.
Pass `--stock-features "$STOCK_BUILD/apps/genlang-features"` for the exact
upstream reference profile. All outputs are build products.

## SDK and loader contract

* **rock_compat_tables.h** declares a compact, typed member view. Use
  `struct rock_compat_settings *global_settings` for the plugin API slot.
  Both sides compile the same declaration with the same target flags.
  The 67 names from a raw source search include two comment-only names.
  `fg` is a fragment of the comment “fg|bg_color”, with no stock member.
  The view covers 65 executable member references plus the documented
  `statusbar` member. It has 44 native mappings and 22 fixed defaults.
  Conditional hardware EQ keeps its upstream feature guard. Lua raw-memory
  access requires regenerated offsets and a view-sized bound before use.
* **rock_compat_sync.h** provides inline functions for the runtime. Keep two
  separate views in the plugin session. Call
  `rock_compat_begin(&view, &baseline, &global_settings)` at entry. Call
  `rock_compat_flush(&view, &baseline, &global_settings)` before settings_save,
  before settings_apply, and on each exit path while the session still owns
  its state. Run these operations under the native settings owner's normal
  serialization. Flush compares each field with its baseline and refreshes
  both views. The native save/apply code owns persistence and side effects.
  A fixed field starts at zero or an empty string. Upstream assignments touch
  only the local view and flush restores its default. Exclude plugins that
  require persistence for these fields. Writes to fixed theme paths cannot
  install or delete a native theme.
* The native playback-log setting has three values. Only generic logging
  reads as stock `true`. An untouched scrobbler setting survives flush.
  Native cached album art reads as prefer-embedded. Writes to the startup
  screen setting accept the shared previous-through-settings range.
  Fixed-width strings copy back only when changed and terminated.
* The SDK table for `rb->str(id)` is `unsigned char *[ROCK_COMPAT_LANG_COUNT]`.
  Call `rock_compat_lang_init(table, language_strings)` at entry and after
  native language changes. Common IDs retain native values and translations.
  New enum entries follow the native string range. The stock ipod6g profile
  yields 817 native entries and 82 English fallbacks. Run
  `rock_compat_widget_text(text, table)` on tagged plugin text before sending
  it to native widgets. Missing voice clips need separate talk handling.
  Native iterators use `LANG_LAST_INDEX_IN_ARRAY`. An SDK iterator that must
  enumerate English extensions needs `ROCK_COMPAT_LANG_COUNT`.
* **rock_compat_input.h** converts a bare stock built-in action or context
  enum value for this target profile. The two conversion functions return
  -1 for an unsupported or unknown code. Ordinary plugin sources and
  `pluginlib_actions.c` compile directly with native action.h, including
  the native context flags and `LAST_ACTION_PLACEHOLDER` used to allocate
  PLA codes. For stored keymaps, check the producer profile, handle context
  flags, sentinels and custom codes separately. Removed pitch, radio and
  recording controls require a real input implementation to enable a caller.
* **mapping.tsv** lists individual settings, language entries, action and
  context declarations, icons, events and file attributes. All 57 named
  icons and 20 application events bind by native name. Firmware events and
  buttons come directly from the firmware headers. On ipod6g 33 of 34 stock
  button constants match exactly. AstraPod adds menu and four album/playlist
  keys to the aggregate BUTTON_REMOTE mask. The five removed file attributes
  are BMARK, FMS, RFMS, WRS and RWRS. A viewer registry, callback ownership,
  raw core bitmap access and native voice behavior require separate adapters.

## Upstream merge and validation

Run `tools/upstream-apps-port.sh` to identify upstream changes that need
a fork port. That inventory includes application files outside this SDK.
Regenerate the tables from the merged tree, compare mapping.tsv and review
settings registry units, choices and effects. Add a conversion or a
reviewed fixed policy for an unmatched field. Review new unmatched action,
context, icon, event or file IDs. Rebuild the stock reference bundle after
changes to its target or plugin build rules.

The reproducible study probe uses its configured normal stock build, captured
compiler commands, generated bitmap headers, fork build, and prototype header
SDK. Set STUDY to the scratch directory named in the compatibility study.
Run from this checkout with the reference toolchain and qemu-arm available:

```sh
python3 tools/rock-compat/generate.py --build "$STUDY/fork-build" \
    --stock-features "$STUDY/stock-build/apps/genlang-features" --out "$OUT"
python3 tools/rock-compat/validate.py --study "$STUDY" --out "$OUT"
```

The probe preprocesses the current SOURCES with the stock configuration,
checks every selected C entry against the fork and writes diagnostics to
`$OUT/validation.json`. It also compiles pluginlib_actions.c, verifies
expanded button values, and runs one freestanding table test under QEMU.
The reference manifest selects 100 C entries and three Lua scripts.
89 C entries pass full syntax checks. 99 have no settings-member, LANG,
action or context diagnostic. pitch_screen still references removed pitch
actions, its context and three missing voice IDs.

| Other diagnostic | C entry |
| --- | --- |
| O_PATH for the database path | tagcache/tagcache |
| Private native settings symbol collides with plugin settings | chessclock, dart_scorer, codebuster, pitch_detector |
| Browser context layout or callback type | file_picker, lastfm_scrobbler, rockpaint |
| Activity identifiers | properties |
| F_THEMESETTING metadata flag | settings_dumper |
| Recording PCM, gain and sample-rate API | pitch_detector |

The C count covers manifest entry files. Lua, linked helper closure, module
linking and player execution have separate acceptance gates.
