- **Lyrics lead** now defaults to 500 ms instead of 200 ms. If you use
  the default, lyrics appear 300 ms earlier than with the old 200 ms
  lead. If you saved a custom value, your player keeps it.

- **LCD TE sync** starts on when an iPod Classic with LCD type 1 detects a
  steady refresh pulse. Compare scrolling with the toggle in **Debug**.

- **Animations** on the iPod Classic now step with the screen's refresh,
  about 67 times a second, so they play about 10% faster than before.
  Cover Flow and full-screen transitions show every second refresh, about
  33 frames a second, evenly spaced.

- **Cover Flow's Transition Speed setting is gone.** Scroll Speed now sets
  how fast the 3D covers move, and its default, 175%, matches the original
  iPod. If you changed Scroll Speed, try it again. The line Transition Speed
  leaves in `config.cfg` is ignored.

- **Cover Flow moves like the original iPod's.** Every click of the wheel
  counts, up to 11 covers ahead, and the covers follow faster the further
  they have to go. The album name changes on the click instead of when the
  cover arrives. A cover turns to face you before it reaches the middle,
  and the covers fan out while you scroll. Covers no longer pass through
  each other: while you scroll fast, a cover between turned neighbours
  sits back behind them for a moment.

- **Cover Flow's layout** is the original iPod's: a wider view, with the
  side covers standing edge-on to the front one, three on each side, all at
  full brightness. The caption is bold on both lines and closer to the
  covers. **3D Parallel Slides is gone**, and the line it leaves in
  `config.cfg` is ignored. 3D Centre Margin and 3D Slide Tuck still move the
  side covers out and in; at 20 and 32, the first-boot settings, the layout
  is the original iPod's.

- **Cover Flow's album panel** (press Select on a cover) is laid out like the
  original iPod's: a wider card from under the covers' top edge to the bottom
  of the screen, seven track rows with the times in a column, and a long
  selected title that scrolls. Its rows look like the theme's lists, with the
  same marker beside the selected track, and the header adds the album's
  year. On Scrim the card shows the album's blurred cover; Light shows a
  light panel. Each album's blurred background, on Now Playing and in the
  panel, is built again the first time it is shown.

- **Scrim's lists glide again**, at every refresh of the screen. The wheel
  can pull a list half a row past either end, and it settles back when you
  stop. For lists that jump a whole row, set `style.list_scroll=snap` in a
  custom theme.

- **Classic and Plastic show the original iPod's split menus**, with the
  list on the left half and a picture of the selected row on the right, in
  the original iPod's Helvetica.
  The right half, the status bar, battery, scrollbar and Now Playing bars
  now use the original iPod's own pictures, under Classic as well as
  Plastic, which are now the same theme. The pictures arrive in
  `.rockbox/themes/stock-art.bin`: update from the whole zip, or those themes
  show an empty pane and no battery.

- **Lists scroll like the original iPod's.** The wheel speeds up as the
  original's does, starting a little later than before. A fast spin that
  leaves the letter you started on jumps a letter a click, with the letter
  in the middle of the screen until you lift your finger. Classic and
  Plastic show the original iPod's letter tile; Scrim and Light show a
  square of their own. Genre lists have no letter jump.
