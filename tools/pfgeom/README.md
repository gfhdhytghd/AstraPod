# pfgeom — checking what the carousel does and does not draw

The album-covers carousel makes two claims about its own geometry, and both are
the sort whose failure shows up as a stripe of stale framebuffer that nothing
later repaints — on one album shape, at one point in one scroll, on hardware.
`pfgeom` decides them on the host instead. It mirrors the carousel's projection,
cull and draw order and renders frames across the whole settings space.

**The cull.** The carousel does not draw the parts of a cover that a nearer one
already hides (`cull_side()` and `slide_covers()` in
`apps-ipod/screens/covers/carousel.c`), which is worth about a third of the
pixels in a scrolling frame. `pfgeom` renders each frame twice — once with the
cull and once without — and compares **which slide finished on top of every
pixel**. That is the exact test: the topmost writer is what picks the colour, so
if the two renders agree everywhere then the cull removed only pixels that were
going to be painted over anyway.

**The clear and the flush.** Both are sized to the rows the covers can reach,
worked out up front by `slide_rows()`, rather than to the whole viewport.
`pfgeom` checks that band against the rows a render really wrote. The bound
carries `PF_ROW_MARGIN` because `dy` dips just under `PFREAL_ONE` at the outer
edge of a cover as wide as `DISPLAY_WIDTH`, and the lower loop's ceiling turns
any shortfall into a whole extra row.

**The divisions.** The render's column walk divides by multiplying, and a
quotient one out moves a row or a column. Before the sweep, `pfgeom` checks both
helpers against plain division over every value the render can reach and well
beyond it.

**The scene.** The render is a painter: it draws the covers furthest from the
middle slot first and the middle one last. `pfgeom` views each cover from
above as a segment and counts, per scenario, the refreshes where two covers
cross and the columns where the cover the painter left on top is not the
nearest one. No draw order can show crossing covers correctly, so the
counts say where the motion itself lets that happen. Neither fails the run.

The poses come from the 3D view's motion, mirrored and driven by clicks: a
single click, steady clicks at 3, 10, 20 and 48 a second and the stop after
them, a reversal, and random clicks at full and half rate. The motion does
not depend on the settings, so it is recorded once and played back under
each combination of the settings that move the slots (centre margin, slide
tuck), the two things that resize the viewport (status bar height and
caption band), and covers of different shapes, since art keeps its aspect
ratio and a tall or wide cover does not decode to the same size as a square
one. A division by zero in the column walk fails the run: on the device it
is a crash.

Nothing here is built or shipped. It has no Rockbox headers and no dependencies.

```sh
gcc -O2 -W -Wall -Wextra -std=gnu11 -o pfgeom tools/pfgeom/pfgeom.c -lm
./pfgeom          # the sweep; exit status is 0 only if nothing went stale
./pfgeom -x       # compare dy at every column instead of sampling three
./pfgeom -v       # print every frame, not just the failures
./pfgeom -l       # where each cover sits at rest with the first-boot settings
```

`-x` is the one worth knowing about. `slide_covers()` decides whether one slide
hides another by comparing how compressed each is at three columns — the two
ends of the overlap and the middle — rather than at all of them, because it runs
per frame on the device. `-x` compares every column, so running both says
whether the sampling ever gets a different answer from the exhaustive check. It
does not, across the whole sweep.

**Update this file's mirror when `carousel.c`'s geometry or motion
changes.** The banner in `pfgeom.c` marks the copied region: the fixed-point
helpers, `fsin`, `recalc_offsets`, the motion (`flow_chase`, `flow_lead`,
`flow_rest_turn`, `flow_turn_of`, `flow_turn_step`, `flow_click`, and
`flow_update` as `flow_refresh`), `flow_cx`, `flow_place`, `coverflow_place`,
`coverflow_idle`, `slide_x_range`, `slide_dy_at`, `slide_covers`,
`cull_side`, and the column walk and draw order out of
`render_slide_clipped`/`coverflow_render`. A mirror that has drifted still
passes -- it is checking itself -- so the answer it gives is only worth what
the copy is. `tools/tests/coverflow_motion_test.py` checks the motion copied
from `carousel.c` itself, then runs this sweep.
