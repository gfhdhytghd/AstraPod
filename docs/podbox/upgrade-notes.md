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
  counts, the covers follow faster the further they have to go, and the
  album name changes on the click instead of when the cover arrives.

- **Cover Flow's layout** is closer to the original iPod's: the side covers
  turn by the same angle with Parallel Slides on or off, three and the edge
  of a fourth show on each side, all at full brightness, and the caption is
  bold on both lines and closer to the covers.

- **Cover Flow's album panel** (press Select on a cover) is laid out like the
  original iPod's: a wider card from under the covers' top edge to the bottom
  of the screen, seven track rows with the times in a column, and a long
  selected title that scrolls. It takes its colours from the native theme, so
  Light shows a light panel. Each album's blurred background, on Now Playing
  and in the panel, is built again the first time it is shown.
