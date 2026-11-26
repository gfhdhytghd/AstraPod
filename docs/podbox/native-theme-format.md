# Native PodBox theme format

The native interface uses design tokens for colour, font, geometry, backgrounds and motion. Screen code defines the layout. The built-in Scrim theme supplies the default tokens. The built-in Light theme uses light surfaces, dark text and solid backgrounds with the same geometry. The built-in Classic theme looks and moves like the original iPod Classic firmware, in that firmware's Helvetica and with its own bitmaps. The built-in Fruit Company theme looks like Classic and replaces AstraPod's menus with the original firmware's: Music, Videos, Photos, Podcasts, Extras, Settings and Shuffle Songs.

## Installing and choosing a theme

Store custom themes at `.rockbox/native-themes/NAME.podtheme`. Choose a theme in **Settings → Theme Settings → Native theme**; under Fruit Company that is **Settings → Advanced Settings → Theme Settings → Native theme**. The theme applies immediately. The saved setting is `native theme: NAME`. The built-in names are `scrim`, `light`, `classic` and `fruit-company`. A saved `plastic` selects Classic. The default is `scrim`.

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

| Role | Scrim and Light | Classic |
| --- | --- | --- |
| `list` | `18-noto-sans` | `16-helvetica-bold` |
| `title` | `18-noto-sans-bold` | `17-helvetica-bold` |
| `body` | `18-noto-sans` | `13-helvetica` |
| `lyrics` | `18-noto-sans` | `18-noto-sans` |
| `status` | `18-noto-sans` | `13-helvetica-bold` |
| `status_title` | `18-noto-sans-bold` | `13-helvetica-bold` |
| `icon` | `24x24-icons` | `24x24-icons` |
| `wps_title` | `26-noto-sans-medium` | `17-helvetica-bold` |
| `handoff_title`, `lyrics_focus` | `26-noto-sans-medium` | `26-noto-sans-medium` |
| `wps_meta` | `12-noto-sans` | `13-helvetica-bold` |
| `wps_time` | `12-noto-sans` | `14-helvetica-bold` |
| `handoff_caption` | `12-noto-sans` | `12-noto-sans` |
| `panel` | `14-noto-sans` | `14-noto-sans` |
| `pane_title` | `18-noto-sans-bold` | `17-helvetica-bold-pane` |
| `pane_text` | `14-noto-sans` | `13-helvetica-bold` |
| `list_value` | `18-noto-sans` | `13-helvetica-bold` |

The `helvetica` fonts are Apple's Helvetica from the stock firmware, at the
stock sizes, with its CJK bitmaps at 11 or 16 px. They keep the stock
layout: whole-pixel glyph positions and no kerning. Other scripts come from
the `noto` fonts' faces at the same size. `17-helvetica-bold-pane` is a
slightly larger 17 px bold for the split menu pane's headings.

`pane_title` and `pane_text` are the split menu pane's large and small text,
used only with `style.menu_pane=split`. `list_value` is a setting row's value,
shown only with `style.accessory=selected`, on the label's baseline.

`wps_time` is the elapsed and remaining time beside the progress bar, used
only with `style.wps_layout=classic`.

### Cover Flow album panel

The album panel's track rows look like a list's. They use the `panel` font,
with titles in `muted` and durations in a dimmer `muted`. The selected row
has `selected_text`, `selected_secondary` and a `selection` marker set by
`list_selection_width` and `list_selection_alpha`. The playing track has a
sign in `accent`. The header shows the album title in `foreground` and the
`title` font, and the artist, year and track count in `muted` and the
`wps_meta` font. `separator` draws the card's edge.

With `background.list=blurred_cover`, the card shows the album's blurred
cover, darkened to keep the text legible, with a darker header band. With
`solid`, or for an album without art, the rows sit on `background` and the
header band is `list_tint`. A `list_tint` equal to `background` is lifted
slightly toward `foreground` so the band still stands apart.

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
(the selection marker or bar, and list scrolling unless
`style.list_scroll=snap`), `sheet_in` and `sheet_out`. `0` makes
that motion a jump. An omitted role keeps the built-in duration, and
`motion_scale` applies to every role.

`motion_easing` sets the curve of slide page transitions: `cubic` (the
default), `quad`, `quint`, `sine` or `linear`, all ease-out. Transitions to
and from Now Playing, and full-screen panels, always slide at constant speed.
With `style.menu_pane=split`, so does a full-width list passing a split menu;
between two split menus only the rows slide, and the pane stays.

### Gradients

`gradient.<role>` replaces a solid fill with a gradient. The value is `none`,
two colours (top and bottom) or four: a one-pixel top line, the top and bottom
of the body, and a one-pixel bottom line. Rows between are blended evenly.

| Role | Fill | Replaces |
| --- | --- | --- |
| `selection` | the list selection bar, with `style.selection=bar` | `color.selection` |
| `status` | the status bar and its volume overlay; the bar then never takes the cover tint, and its title is embossed with a white shadow one pixel below | `color.status_bg` |
| `progress` | the Now Playing progress and volume fill, with `style.wps_layout=classic` | `color.wps_accent` |
| `progress_track` | the track behind that fill | |
| `scrollbar` | the scroll thumb, with `style.scrollbar=classic`, shaded left to right | the darkened thumb |

### Styles

`style.<name>` chooses between drawing styles. The first keyword is Scrim's.

| Key | Keywords | Effect of the second keyword |
| --- | --- | --- |
| `selection` | `marker`, `bar` | A full-width bar behind the selected row instead of a thin marker. Selected text uses `selected_text`, other rows `muted`. A label too long for a row that does not scroll ends in `...`. |
| `list_icons` | `auto`, `off` | No icon column in lists. The Agents screen and multi-select marks keep theirs. |
| `accessory` | `all`, `selected` | The chevron shows on the selected row only. Setting rows show their value as text in `accent`, right-aligned. |
| `status_clock` | `on`, `off` | No clock in the status bar; the title takes the room. |
| `status_title` | `left`, `center` | The title is centred and the clock moves right. |
| `page_motion` | `cascade`, `slide` | Pages slide side by side under a fixed status bar, at full brightness. |
| `wps_layout` | `cover`, `classic` | Now Playing with a status bar, a tilted cover and its reflection on the left, the track text on the right, and the progress bar with both times at the bottom. Turning the wheel slides the volume bar in from the right in place of the progress bar. The layout is fixed; colours, fonts and gradients apply. |
| `scrollbar` | `plain`, `classic` | A 4-pixel thumb with rounded ends and no track, laid over the rows at the right edge. It shows when the list opens or the selection moves, stays 270 ms, then fades out over about 480 ms. |
| `menu_pane` | `none`, `split` | Menus take the left half of the screen, and their status bar spans it. The right half shows the selected row: a slow pan across the library's covers for music rows, otherwise its icon and name, a settings row's title, icon and value, the playing track, or the free space. A new row's pane follows half a second after the selection. Lists of the library's contents stay full width. The pane is drawn with the stock firmware's backdrop, shadow, tiles and icons from `.rockbox/themes/stock-art.bin`. |
| `art` | `none`, `plastic` | Apple's bitmaps from the stock firmware replace the drawn status bar, battery, play and pause icons, the selected row's chevron, the scrollbar, the Now Playing progress and volume bars, its shuffle icon and its speakers. The selection gradient becomes Apple's exact rows, whatever the theme sets for it. A long selected row scrolls as on the original iPod: after about 1.3 s it moves left 2 pixels every 34 ms and loops round, and the scroll settings do not apply. The bitmaps are sized for the Classic layout and come from `.rockbox/themes/stock-art.bin`; without that file they are not drawn. |
| `list_scroll` | `glide`, `snap` | The rows jump a whole row at a time and only the selection marker moves; the ends of the list do not stretch. See *Native rendering and animation*. |
| `list_rows` | `pitch`, `fill` | The rows fill the list's height: the pixels left below the last whole row go one each to the top rows, so nine 24-pixel rows in 220 pixels become four of 25 and five of 24. |
| `menu_tree` | `astrapod`, `stock` | The main menu and everything under it are the original iPod Classic's: Music, Videos, Photos, Podcasts, Extras, Settings and Shuffle Songs, with Menu going back one level. AstraPod's own settings are under **Settings → Advanced Settings**, its Files and System menu under **Extras**, and its Playback Report is a main menu row that **Settings → Main Menu** starts with off. A row the original firmware has and AstraPod does not either does nothing, as an empty list does there, or says it is not available. Choosing a theme with `stock` asks first, and choosing a theme that changes this goes back to the main menu. |

Light keeps Scrim's styles.
Classic sets the second keyword of every style except `list_scroll` and
`menu_tree`. Fruit Company is Classic with `style.menu_tree=stock`. To
start a custom theme from Classic, copy its values from the table below.

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
font.list = 16-helvetica-bold
font.title = 17-helvetica-bold
font.body = 13-helvetica
font.status = 13-helvetica-bold
font.status_title = 13-helvetica-bold
font.wps_title = 17-helvetica-bold
font.wps_meta = 13-helvetica-bold
font.wps_time = 14-helvetica-bold
font.pane_title = 17-helvetica-bold-pane
font.pane_text = 13-helvetica-bold
font.list_value = 13-helvetica-bold
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
style.art = plastic
metric.status_pad_right = 5
metric.status_icon_gap = 5
```

Classic also changes the list and status paddings and the album-row size.
A custom theme keeps Scrim's values for those unless it sets them.

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
35% background dimming. Lists keep selection and scroll position separately.
The selection moves over 8 f, and a new detent starts from wherever the
selection is drawn. The rows glide too, over the same 8 f. The wheel can
expose at most half a row beyond the viewport, and when input stops the list
settles over 10 f with one row of context where available. With
`style.list_scroll=snap`, the rows instead jump a whole row at a time and only
the selection marker moves. A step that scrolls the list keeps the marker on
its screen row, and the ends of the list do not stretch. On the iPod Classic a
full-width list moves the panel's own scroll and sends only the rows that
change, so every frame reaches the screen, at each refresh; a list drawn over
the album backdrop, and a split menu, send each frame whole, as does a step
that scrolls a list under `style.list_rows=fill`. Status icons and
changing playback values use 8 f. Lyric line motion uses 16 f.

Set `/.rockbox/ui-perf.enable` before starting the firmware to record frame
timing. Each animation writes its samples and summary to
`/.rockbox/ui-perf.txt` when it exits. The reported work combines renderer and
synchronous LCD submission time. Submission intervals are recorded separately.
They include scheduling and any skipped visual updates. Each row's
`te_slots` counts the panel refreshes since the previous submission began.
`present_slots` counts the refreshes between the ones that first show this
frame and the previous one, as the LCD driver scheduled their transfers: a
small rect sent early in a refresh shows in it, a large one in the next. A
motion that shows a new frame at every refresh reads `present_slots` 1 after
its first row; the summary's `present_gaps` and `present_repeats` count the
rows above and below 1, and `te_gaps` and `te_repeats` do the same for
`te_slots`. `hw_offset` is the panel's scroll offset after a frame that moved
the panel's scroll, and -1 for a frame sent whole. `band_rows` is how many
screen-wide rows of pixels such a frame sent. The log cannot see the panel
scanning out the pixels.
