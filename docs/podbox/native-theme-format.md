# Native PodBox theme format

The native interface uses design tokens for colour, font, geometry, backgrounds and motion. Screen code defines the layout. The built-in Scrim theme supplies the default tokens. The built-in Light theme uses light surfaces, dark text and solid backgrounds with the same geometry. The built-in Classic theme looks and moves like the original iPod Classic firmware, in AstraPod's fonts. The built-in Plastic theme is Classic drawn with Apple's own bitmaps from that firmware.

## Installing and choosing a theme

Store custom themes at `.rockbox/native-themes/NAME.podtheme`. Choose a theme in **Appearance → Native theme**. The theme applies immediately. The saved setting is `native theme: NAME`. The built-in names are `scrim`, `light`, `classic` and `plastic`. The default is `scrim`.

Place referenced font files in `.rockbox/fonts/`. Use PBF1 `.pbf` files for text and an RB12 `.fnt` file for icons. Firmware falls back to Scrim if a theme file is missing or malformed, or if it references an unsupported font. It records the failure in `.rockbox/native-theme.log`.

## Writing a theme

Use a text file of at most 4096 bytes. Start the file with `podbox-theme=1` after any comments or blank lines. Each subsequent non-comment line sets one `key=value` token. Keys and keywords are case-sensitive. Spaces and tabs around keys, values and separators are optional. A line whose first non-space character is `#` is a comment. Use LF or CRLF line endings. The last line can end at the end of the file.

Every token in a custom theme is optional. Any omitted token uses its Scrim value, so a file containing only `podbox-theme=1` produces built-in Scrim. Duplicate keys, unknown keys, malformed values, and combinations of dimensions that do not fit cause the entire file to fail validation.

```ini
podbox-theme=1
# Keep Scrim geometry and use a solid light list background.
color.background = f6f5f2
palette.background = none
color.foreground = 181818
color.selected_text = 181818
color.muted = 505050
palette.muted = none
color.selection = 12588e
palette.selection = none
color.status_bg = dddcd7
font.list = 18-noto-sans
metric.row_height = 32
background.list = solid
motion_scale = 100
```

### Colour and album palette

`color.<role>` sets an RGB888 fallback using six hexadecimal digits. An optional leading `#` is accepted. The display converts these values to its native colour depth.

`palette.<role>` controls whether a colour follows the current artwork. The value is `none`, or `source,min,max`. The three sources are `dominant`, `accent`, and `vivid`. The bounds are inclusive CIELAB lightness values between 0 and 100 with `min ≤ max`. The renderer preserves hue when adjusting tones and fits the result to the display gamut. Display colour depth can limit the tones available in a narrow range. If the art does not provide a colour, the renderer uses the `color.<role>` fallback. If `vivid` is absent but the art has another palette colour, the renderer uses an available colour and applies the same bounds.

Changing a fallback colour leaves its palette rule in place. Set both keys to make a Scrim palette role permanently static. For example, `color.selection=12588e` and `palette.selection=none` keep a blue selection bar on every album.

| Surface | Roles |
| --- | --- |
| Shared | `background`, `foreground`, `muted`, `accent`, `selection`, `selected_text`, `separator`, `dim` |
| List | `list_tint` |
| Status bar | `status_bg`, `status_fg` |
| Modal | `modal_bg`, `modal_fg`, `modal_border`, `modal_accent`, `modal_selected_text` |
| Now Playing | `wps_bg`, `wps_fg`, `wps_muted`, `wps_accent`, `wps_meter`, `wps_panel` |
| Handoff | `handoff_bg`, `handoff_fg`, `handoff_muted`, `handoff_unselected_text`, `handoff_selected_text`, `handoff_cover_placeholder` |

Scrim's album-driven defaults are `palette.background=dominant,0,15`, `palette.list_tint=dominant,0,40`, `palette.muted=vivid,85,100`, `palette.accent=vivid,60,100`, `palette.selection=vivid,60,100`, `palette.wps_panel=dominant,0,30` and `palette.wps_accent=vivid,60,100`.

### Fonts

Use `font.<role>=<basename>` to reference a font. The basename can contain 1–31 ASCII letters, digits, hyphens and underscores, and must start with a letter or digit. The firmware appends `.pbf` for text roles and `.fnt` for `font.icon`. `font.icon` uses the glyph mapping for the built-in status and navigation symbols. Use an icon font with the same glyph assignments to preserve their meaning.

| Role | Built-in basename |
| --- | --- |
| `list`, `body`, `lyrics`, `status` | `18-noto-sans` |
| `title` | `18-noto-sans-bold` |
| `icon` | `24x24-icons` |
| `wps_title`, `handoff_title`, `lyrics_focus` | `26-noto-sans-medium` |
| `wps_meta`, `handoff_caption` | `12-noto-sans` |
| `panel` | `14-noto-sans` |
| `status_title` | `18-noto-sans-bold` |

### Cover Flow album panel

The album panel draws its header in `list_tint`, with the album title in
`foreground` and the `title` font and the artist line in `muted` and the
`wps_meta` font. A `list_tint` equal to `background` is lifted slightly toward
`foreground` so the header still stands apart. Track rows use the `panel`
font, `foreground` titles and `muted` durations; the selected row is a
`selection` bar with `selected_text` and `selected_secondary`. `separator`
draws the card's edge. With `background.list=blurred_cover` the rows sit on the
blurred album art, and with `solid` on `background`.

### Metrics

Set metrics with `metric.<name>=<unsigned decimal integer>`. Coordinates, offsets and sizes are in display pixels. Alpha metrics use 0–15, where 0 is transparent and 15 is opaque. Metric values must fit their associated screen and leave enough room for its contents. For example, a new list row height must fit both its icon and text cell. An increased status height can also require an increased `list_y`.

| Surface | Metric suffixes and Scrim defaults |
| --- | --- |
| List viewport | `list_x=10`, `list_y=46`, `list_bottom=10`, `row_height=30` |
| List row | `list_icon_size=24`, `list_icon_margin=2`, `list_icon_gap=10`, `list_text_padding=12`, `list_text_height=20`, `list_right_padding=4`, `list_selection_width=3`, `list_selection_alpha=14`, `list_accessory_gap=9`, `list_tint_alpha=15` |
| List art and detail | `list_art_size=44`, `list_art_row_height=48`, `list_art_top_gap=5`, `list_art_bottom_gap=3`, `list_header_height=68`, `list_header_bottom_gap=4`, `list_notes_height=62`, `list_aux_padding=3` |
| Status bar | `status_height=35`, `status_pad_left=10`, `status_pad_right=7`, `status_icon_size=24`, `status_icon_gap=2`, `status_group_gap=9`, `status_clock_width=70`, `status_clock_numeric_width=118`, `status_link_gap=10`, `status_text_y=8`, `status_title_height=20`, `status_icon_y=5` |
| Modal panel | `modal_padding=10`, `modal_edge_margin=12`, `modal_border_width=2`, `modal_button_gap=18`, `modal_button_pad_x=12`, `modal_button_pad_y=5` |
| Cover | `cover_size=240` |
| Now Playing transport | `wps_transport_x=2`, `wps_transport_y=2`, `wps_transport_width=76`, `wps_transport_height=24` |
| Now Playing metadata | `wps_title_x=5`, `wps_title_y=85`, `wps_title_width=230`, `wps_title_height=28`, `wps_subtitle_y=113`, `wps_subtitle_height=20`, `wps_deck_index_x=5`, `wps_deck_time_x=45`, `wps_deck_y=150`, `wps_deck_width=30`, `wps_deck_height=14` |
| Now Playing progress | `wps_progress_width=240`, `wps_progress_height=5`, `wps_progress_y=140`, `wps_progress_alpha=6` |
| Now Playing spectrum | `wps_meter_x=0`, `wps_meter_y=200`, `wps_meter_width=80`, `wps_meter_height=40`, `wps_meter_peak_height=2` |
| Handoff layout | `handoff_margin=16`, `handoff_cover_size=96`, `handoff_button_height=36`, `handoff_button_gap=8`, `handoff_metadata_gap=12`, `handoff_line_gap=3`, `handoff_error_offset=4` |
| Handoff decoration | `handoff_outline_width=1`, `handoff_cover_border_alpha=4`, `handoff_button_border_alpha=5`, `handoff_scrim_alpha=8`, `handoff_progress_gap=5`, `handoff_progress_height=2`, `handoff_progress_alpha=4` |
| Splash progress | `splash_progress_gap=4`, `splash_progress_radius=2` |

The `dim` colour tints the underlying page of a sheet. The `list_tint_alpha`
metric sets the opacity of the list notes panel. The parser accepts the
reserved `modal_shadow` and `modal_shadow_offset` keys. Native sheets use square
edges without shadows.

### Background and motion

`background.list`, `background.wps` and `background.lyrics` accept `solid`,
`blurred_cover` or `cover_scrim`. Scrim uses `blurred_cover` for lists and
lyrics. Its Now Playing mode is `cover_scrim`, which draws the sharp cover
over the blurred background. Light uses `solid` throughout.

Handoff inherits the Now Playing backdrop, its `wps_bg` fallback and its
contrast tint. The parser still accepts `background.handoff`, `handoff_bg`
and `handoff_scrim_alpha` in existing theme files.

`motion_scale` is a whole percentage from 0 to 300. `100` keeps the default frame count, `50` halves it, and `0` disables motion. The animation setting also controls whether motion plays. Durations are measured in frames on the 60 Hz clock.

`motion.<role>` sets one transition's duration in frames on the same clock,
from 0 to 120. The roles are `push` and `pop` (page transitions), `list`
(the selection bar and list scrolling), `sheet_in` and `sheet_out`. `0` makes
that motion a jump. An omitted role keeps the built-in duration, and
`motion_scale` applies to every role.

`motion_easing` sets the curve of slide page transitions: `cubic` (the
default), `quad`, `quint`, `sine` or `linear`, all ease-out. Transitions to
and from Now Playing, and full-screen panels, always slide at constant speed.

### Gradients

`gradient.<role>` replaces a solid fill with a gradient. The value is `none`,
two colours (top and bottom) or four: a one-pixel top line, the top and bottom
of the body, and a one-pixel bottom line. Rows between are blended evenly.

| Role | Fill | Replaces |
| --- | --- | --- |
| `selection` | the list selection bar, with `style.selection=bar` | `color.selection` |
| `status` | the status bar and its volume overlay; the bar then never takes the cover tint | `color.status_bg` |
| `progress` | the Now Playing progress and volume fill, with `style.wps_layout=classic` | `color.wps_accent` |
| `progress_track` | the track behind that fill | |
| `scrollbar` | the scroll thumb, with `style.scrollbar=classic`, shaded left to right | the darkened thumb |

### Styles

`style.<name>` chooses between drawing styles. The first keyword is Scrim's.

| Key | Keywords | Effect of the second keyword |
| --- | --- | --- |
| `selection` | `marker`, `bar` | A full-width bar behind the selected row instead of a thin marker. Selected text uses `selected_text`, other rows `muted`. |
| `list_icons` | `auto`, `off` | No icon column in lists. The Agents screen and multi-select marks keep theirs. |
| `accessory` | `all`, `selected` | The chevron shows on the selected row only. Setting rows show their value as text in `accent`, right-aligned. |
| `status_clock` | `on`, `off` | No clock in the status bar; the title takes the room. |
| `status_title` | `left`, `center` | The title is centred and the clock moves right. |
| `page_motion` | `cascade`, `slide` | Pages slide side by side under a fixed status bar, at full brightness. |
| `wps_layout` | `cover`, `classic` | Now Playing with a status bar, a tilted cover and its reflection on the left, the track text on the right, and the progress bar with both times at the bottom. Turning the wheel slides the volume bar in from the right in place of the progress bar. The layout is fixed; colours, fonts and gradients apply. |
| `scrollbar` | `plain`, `classic` | A 4-pixel thumb with rounded ends and no track, drawn at the right edge. |
| `art` | `none`, `plastic` | Apple's bitmaps from the stock firmware replace the drawn battery, play and pause icons, the selected row's chevron, and the Now Playing shuffle icon and speakers. The status, selection, `progress` and `progress_track` gradients become Apple's exact rows, whatever the theme sets for them. The bitmaps are sized for the Classic layout. |

Classic sets the second keyword of every style except `art`. To start a
custom theme from Classic, copy its values from the table below.

### The Classic theme

Classic uses Light's colours as its base, with these changes:

```ini
color.background = ffffff
color.foreground = 000000
color.muted = 000000
color.accent = 3185d0
color.selection = 0089de
color.selected_text = ffffff
color.status_bg = cecfce
color.status_fg = 000000
color.wps_muted = 3d3d3d
gradient.selection = 96c5da,09a4e1,0065d3,004386
gradient.status = fcfcfc,f8f8f8,9f9e9e,5b5b5b
gradient.progress = 6483f0,6789f2,56aeff,4e8fce
gradient.progress_track = dbdbdb,fdfdfd,e2e2e2,bababa
font.list = 18-noto-sans-bold
font.status = 14-noto-sans
font.status_title = 14-noto-sans
font.wps_title = 18-noto-sans-bold
metric.row_height = 24
metric.list_x = 0
metric.list_y = 20
metric.list_bottom = 0
metric.status_height = 20
background.status = solid
motion.push = 18
motion.pop = 18
motion.list = 0
motion_easing = quad
```

Classic also changes the list and status paddings and the album-row size.
A custom theme keeps Scrim's values for those unless it sets them.

### The Plastic theme

Plastic is Classic with these additions:

```ini
style.art = plastic
metric.status_pad_right = 5
metric.status_icon_gap = 6
```

## Native rendering and animation

The native screen code owns the layout and draws to a canonical RGB565
framebuffer. Retained views represent geometry, clip, opacity and brightness.
A frame accumulates damage from changed views and submits the union to the LCD.
Transitions own temporary RGB565 surfaces from `core_alloc` while the current
page keeps its input loop. If a transition has insufficient free memory, the
destination is drawn immediately.

Page transitions update on the shared 60 Hz `USEC_TIMER` timeline. The standard
push uses 18 f and the pop uses 15 f. The page behind moves by up to 30% of the
screen width and reaches 70% brightness. Sheets use 16 f in and 12 f out with
35% background dimming. Lists keep selection and scroll position separately and
retarget over 8 f. The wheel can expose at most half a row beyond the viewport.
When input stops, the list settles over 10 f with one row of context where
available. Status icons and changing playback values use 8 f. Lyric line
motion uses 16 f.

Set `/.rockbox/ui-perf.enable` before starting the firmware to record frame
timing. Each animation writes its samples and summary to
`/.rockbox/ui-perf.txt` when it exits. The reported work combines renderer and
synchronous LCD submission time. Submission intervals are recorded separately.
They include scheduling and any skipped visual updates. The log measures
submissions to the LCD driver. It cannot see the panel scanning out the pixels.
