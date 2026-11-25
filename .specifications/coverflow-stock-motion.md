# Stock Cover Flow: measured motion and layout

Reference for re-tuning `apps-ipod/screens/covers/carousel.c` to Apple's
stock iPod Classic Cover Flow. Measured from one phone video of the stock
firmware (20.3 s, 1219 frames, 60 fps, 1080x1920 portrait); internals
document, source paths are deliberate.

Every number is tagged **M** (measured from frames), **F** (fitted to
measured frames) or **I** (inferred: consistent with the evidence, not
proven). Frame numbers `fNNNN` are 0-based video frames (16.67 ms apart).
Coordinates are LCD pixels, 320x240, origin top-left.

## 0. Method and its limits

- The LCD was located in every frame (bright component inside the dark
  bezel, convex hull, line-fitted edges) and rectified to 640x480.
  Rectification check: the battery icon centroid stays at x 300.5 (5-95 %:
  299.5-302.4), y 13.0 (11.5-13.6). **Position error about +-1.5 px.**
  Screen centre measured at x = 162 (caption and rest cover both centre
  there), so absolute x values may carry a +2 px offset; widths do not.
- Cover poses were tracked by warping a rest-pose crop of the cover into a
  quad with vertical edges `(xl, xr, hl, hr)` centred on y = 105.1 and
  maximising normalised cross-correlation (`scripts/covertrack.py`).
- The thumb was tracked on the click wheel (same homography extended to the
  wheel plane, skin segmentation in Lab a*, pad point 22 px behind the tip;
  `scripts/thumbtrack.py`). The angle is a proxy for the capacitive contact
  centroid: **+-5 deg per frame, scale error up to ~20 %** when the thumb
  lies flat over the wheel.
- The camera (60 fps) and the panel (66.96 Hz) beat, and the panel's slow
  pixel response blends consecutive images, so a single camera frame often
  shows two rendered frames superimposed.
- 77 frames have no LCD fit (glare): f0128-0160 (part), f0745-0762,
  f0895-0923, f1106-1129 (part).
- Scratch data and scripts: `/tmp/claude-1000/coverflow-ref/`
  (`thumb.txt`, `track_*.txt`, `change.txt`, `cap_events.txt`, `rect/`,
  `face/`, `sheets/`).

What the video contains: rest on the first album (f0000-0058); slow forward
scroll (f0059-0370); fast forward (f0370-0590, partly over placeholder
tiles); fast reverse (f0590-0812); reverse-to-forward turn (~f0815-0835);
forward (f0835-0895); reverse back to the first album, finger lifted, rest
(f0910-1076); fast forward and stop with the finger left on the wheel
(f1076-1218). There is no isolated single click from rest: the thumb was
already turning when the first step started.

## 1. Static layout at rest

Sources: f0005-0050 averaged (first album, right side only), f0030, f1063.
No rest frame with covers on both sides exists; the left side is taken by
mirror symmetry and checked against f1212 (0.08 item short of rest).

| Item | Stock value | Tag | Uncertainty |
|---|---|---|---|
| Title bar | rows 0-22.5, "Cover Flow" left, battery right | M | +-1 |
| Centre cover | x 100.0-224.75, y 42.75-167.5: **125 x 125** | M | +-1.5 |
| Centre cover centre | x 162.4 (= screen centre), y 105.1 | M | +-1.5 |
| Vertical alignment | every cover centred on y = 105.1; side covers grow and shrink symmetrically about it (no floor line) | M | +-1 |
| First side cover | inner edge x 225-226 (touches centre cover, gap 0), outer edge x 263.5; projected width **38.5** | M | +-1.5 |
| First side cover heights | inner edge **~72** (top y 69), outer edge **124** (y 42.3-166.3, same as centre) | M | +-2 |
| Stack spacing | outer edges at 263.5, 291, ~318: **27.5** px per cover | M | +-1 |
| Stack covers | width 27-28, inner edge ~104 tall, outer edge 125-130 | M | +-3 |
| Covers per side | 3 whole + a sliver of a 4th at the screen edge | M | — |
| Overlap | adjacent covers meet edge to edge; no visible overlap or gap | M | — |
| Side cover facing | inward (outer edge nearer the viewer and taller) | M | — |
| Dimming | none visible on side covers | M | camera exposure |
| Reflections | present under centre and side covers | M | not measured, out of scope |
| Background | pale blue-white, lighter toward the bottom | M | camera colours unreliable |
| Placeholder art | blue gradient square with a large white double-note glyph, used for centre and side covers alike | M | — |

The owner's 128 px target is 2.4 % (3 px) larger than the measured 125 +-
1.5. The difference is near the measurement limit and not visible at a
glance.

### Caption (f0005-0050, f1212)

| Item | Stock value | Tag |
|---|---|---|
| Lines | 2: album, artist; both centred on the screen centre | M |
| Weight | both lines bold, same size | M |
| Cap height | ~11 px (font about 14-15 px) | M +-1 |
| Line 1 | cap top 191, baseline 202, descenders to ~205 | M +-1 |
| Line 2 | cap top 208.5, baseline 219.5 | M +-1 |
| Line pitch | **17.5** px | M +-0.5 |
| Gap cover bottom to line 1 cap top | **23.5** px (the reflection sits in it) | M +-1.5 |
| Caption block | 191-~222; 18 px above the bottom edge | M |
| Too-long title | stays centred, clipped by both screen edges, no ellipsis, no scrolling within the 0.3-0.5 s a title was on screen (f0060-0068, f1175-1191) | M |

## 2. Input: wheel to target

- **The caption shows the target, and it changes on the click.** The new
  caption appears 0-3 frames before the first visible cover motion: f0060
  caption with motion first visible f0060; f0330-0331 caption, incoming
  cover starts f0333-0334; f1192 caption, cover starts f1193-1194. **M**
- **The caption swap has no fade.** Each change shows at most one blended
  camera frame (f0069, f0090, f0100, f0108, f0125, f0134, f1078, f1086,
  f1096, f1105, f1131), which is the panel response plus exposure, so any
  cross-fade is <= 17 ms. **M**
- **Degrees of thumb travel per item: 15-17.5.** f0177-0359: 10 items in
  175 deg (17.5); f1078-1192: 27 items in 402 deg (14.9); f0720-0800: 19
  items in 303 deg (16). One outlier, f0060-0134: 7 items in 62 deg, with
  the thumb lying flat over the wheel where the proxy under-reads. **F**
  - This matches 4 raw ticks of a 96-tick wheel (15 deg), the same as
    AstraPod's `WHEEL_SENSITIVITY` 4. **I**
- **No acceleration and no cap on the target.** The ratio holds from
  ~40 deg/s to ~290 deg/s (2.5 to 18 items/s). The caption keeps pace with
  the thumb in fast scroll (f0764-0807: caption 13 items/s, thumb 13-16
  items/s). **M/F**
  - In fast scroll the caption steps every 4-8 frames and skips items
    (f1131 Miku to Beyond的精彩 skips 2; f1136, f1154, f1164 skip 1;
    f0807 skips 1). The caption is redrawn only when a frame is rendered,
    showing the target at that moment. **M**

## 3. Display position: how the covers follow the target

### 3.1 The law (all regimes)

The displayed position chases the target with a **first-order lag**: speed
proportional to the remaining distance, decelerating to rest, no overshoot,
no spring, no fixed duration. **F**

Fits (`scripts/fitsettle.py`; `d` in items, taken from the arriving
cover's left edge, 125 px = 1 item, valid for d < 0.7). Three laws were
tried: pure exponential `v = k*d`; sum `v = k*d + v0`; floor
`v = max(k*d, v0)`.

| Settle | d0 | Exponential k (RMS) | Sum k, v0 (RMS) | Floor k, v0 (RMS) |
|---|---|---|---|---|
| f1041-1064, finger lifted, reaches rest | 0.53 | 6.7 (4.5 px) | 4.5, 0.65 (2.8 px) | 6.5, 1.15 (3.3 px) |
| f1199-1217, finger on wheel, video ends first | 0.49 | 5.2 (3.6 px) | 4.0, 0.45 (2.2 px) | 5.3, 1.15 (2.6 px) |
| f0336-0358, slow scroll, next click at f0359 | 0.69 | 6.1 (2.1 px) | 5.25, 0.30 (1.7 px) | 6.0, 0.95 (1.8 px) |

- **The sum law fits best: k = 4.6 +- 0.6 /s, v0 = 0.5 +- 0.2 items/s.**
  This is AstraPod's own structure (a floor plus a distance term). **F**
- A pure exponential needs k = 6 +- 0.8 /s (time constant ~165 ms) and
  misses the end. f1041-1064 ends abruptly (d 3.3 px at f1060, 0.9 at
  f1062, rest at f1064), where an exponential would still be creeping.
- Both predict for a 1-item step: 90 % at 0.35-0.38 s, rest at 0.48-0.51 s.
- **Large distances are not measured.** All fits use d < 0.7. The lag in
  fast scroll (1.5-2 items at ~16 items/s, from f0443, low confidence) is
  shorter than the sum law predicts (`(16 - 0.5) / 4.6` = 3.4). The gain
  may rise with distance, as AstraPod's curve does. **I**

Per-frame left edge of the arriving cover, f1041-1064 (rest = 98.3 in this
segment's rectification):

```
f1041 31.7  f1043 47.5  f1046 48.5  f1047 62.4  f1049 70.5  f1051 78.9
f1053 79.5  f1054 86.3  f1056 90.2  f1057 89.5  f1058 92.3  f1060 94.9
f1061 95.3  f1062 97.3  f1063 97.6  f1064 98.0
```

### 3.2 Regimes

**(a) Single click from rest.** Not isolated in the video. The fitted
law predicts 90 % of the step in 0.35-0.38 s and rest in 0.48-0.51 s.
**I** The first step of the video (click ~f0059) moved the
outgoing cover's projected centre 0.19 item in the first rendered frame
(f0060), 0.37 by f0062 and 0.48-0.58 by f0064-0065, consistent with
k = 6-8 /s. A second click landed at f0068-0069. **M**

**(b) Slow scrolling** (clicks slower than one settle, 2-7 items/s,
f0100-0370). Each click restarts nothing: the target moves on and the same
law continues from the current position. The display never comes to rest
between clicks (the front cover is never frontal during f0100-0280), so
the motion reads as continuous. **M**

**(c) Speeding up.** The display speed is the input rate, with a lag that
grows with the rate: ~0.6 item at 3 items/s (sum law), 1.5-2 items at
~16 items/s (f0443: the near-frontal cover is 1.5-2 items right of centre;
identifying it as the target is uncertain). No separate speed multiplier
or skip logic is visible. **F/I**

**(d) Sustained fast scrolling** (13-18 items/s, f0425-0460, f0764-0812,
f1135-1190):

- Full-detail covers, no blur, no simplification. **M**
- No frontal centre cover at any moment. In forward scroll the left stack
  runs past the centre (to x ~200 at f0443) at the normal ~27 px spacing,
  and the covers turn on the incoming side, 60-100 px off centre (f0443:
  x 200-268; reverse f0778: x 45-100). **M**
- Rendering drops to **~15 Hz** (a new image every 3-5 camera frames,
  median 4), against **~30 Hz** in slow motion and settling (a new image
  every ~2 camera frames, f1199-1217, f1041-1064). **M** This is a stock
  limitation, not a target.

**(e) Stop.** When the clicks stop, the display finishes the remaining lag
with the law of 3.1: f1191 last click with the display ~1 item behind; the
arriving cover crosses the slot boundary in ~5 frames (f1193-1198) and then
decays (d 0.61 item at f1198, 0.08 at f1216, still moving when the video
ends). No overshoot, no snap. The fast-scroll look ends at once: by f1194
the arriving cover is near frontal. **M**

- Lifting the finger is not needed to settle (f1041-1064 and f1199-1217
  follow the same law). **M**

**(f) Reversal.** Thumb turns at ~f0812-0818. The caption keeps counting
down to Bandari at ~f0825-0830 and then counts up. The displayed row
decelerates and reverses smoothly over ~10-15 frames (f0825-0835 in the
column kymograph), with no snap. **M** The 10-frame delay before the
target turns is within the thumb proxy's error plus the 4-tick hysteresis.
**I**

### 3.3 Rotation leads translation

In the first step (f0060-0066) the outgoing cover's pose progress, from
the height of its inner edge (125 to ~72), runs ahead of its position
progress (projected centre x 162.7 to ~85):

| Frame | position | pose |
|---|---|---|
| f0060 | 0.19 | 0.28 |
| f0062 | 0.37 | 0.57 |
| f0064 | 0.48 | 0.72 |
| f0065 | 0.58 | 0.74 |

- For two first-order chases, this gives a pose rate about **1.6-1.9 times**
  the position rate. **F**, one event, low confidence.
- The incoming cover is near frontal while still ~0.5 item off centre
  (f1198-1200), then slides the rest of the way. **M**
- **I:** the fast-scroll look in 3.2 (d) follows from this lead. The pose
  tracks the target more tightly than the position. At 15 items/s the pose
  runs ~1 item ahead, so every cover between the displayed position and
  the target is already turned toward its destination stack, and the
  turning zone sits on the incoming side.

## 4. AstraPod as built (reference)

From `carousel.c`, default settings (Transition Speed 325 %, Scroll Speed
175 %, Centre Margin 20 from `default-config.cfg`, Parallel Slides on).

- Wheel: 24 events per turn (15 deg), `button-clickwheel.c:64,72,176-179`.
- Click: `target = center+1`; while animating, `target = min(center+2, n-1)`
  and further clicks are dropped (`carousel.c:3732-3758`).
- Per frame: `v = 512*TS/100 + accel*SS/100` = `1664 + 1.75*16384*(1+sin(ia))`
  at the defaults, in 1/65536 item per 30 fps frame, `d` clamped at 2 items
  (`update_scroll_animation`, `carousel.c:4321-4432`). Effective
  `k = v/d`: 13.9 /s at d = 1, 9.2 /s at d = 0.5 and 0.1; floor 0.76
  items/s; ceiling 27 items/s.
- Single step: 50 % at 50 ms, 90 % at 250 ms, rest at 367 ms.
- Fast wheel: `interval < 4 frames` (>= 15 events/s) steps instantly with
  no animation (`f157f65bdc`, `carousel.c:3675-3702`). `flow_mix` ramps a
  hand-made fast pose over 7 frames (`carousel.c:2541-2627`).
- Caption: follows `center_index`, so it changes when the step **arrives**
  (~367 ms after the click), no fade (`carousel.c:1537-1625`).
- Layout: cover 128 x 128 at rows 28-155; side covers 62.4 px wide, inner
  edge 89.4 px, tilt 55 deg, spacing 19.6; side brightness 256, 256, 128,
  4th hidden.
- Caption fonts: line 1 `18-noto-sans-bold`, line 2 `18-noto-sans`
  (height 19, ascent 16); line pitch 24; `y1 = 176`, `y2 = 200`
  (`carousel_caption_layout`, `carousel.c:2802-2826`; `PF_CAPTION_PAD` 6).
- Flush: covers rows 26-158 (132 rows, 12.9 ms); covers plus caption rows
  26-219 (193 rows, 18.9 ms).

## 5. Stock vs AstraPod, with proposed changes

| # | Item | Stock | AstraPod now | Proposal | Needs |
|---|---|---|---|---|---|
| 1 | Degrees per item | 15-17.5 (F) | 15 | keep | — |
| 2 | Target lead | uncapped, every click counts (M) | max 2 ahead, rest dropped | remove the cap (or raise to >= 4) | small code change |
| 3 | Fast-wheel instant step | none; same chase, render rate falls (M) | instant step at >= 15 events/s | remove; the chase covers this regime | code change |
| 4 | Speed law | `k*d + v0`, k = 4.6 +- 0.6 /s, v0 = 0.5 +- 0.2 items/s for d < 0.7 (F); large d not measured | sin curve: effective k 9.2 /s (small d) to 13.9 /s (d = 1), floor 0.76, d clamped at 2 | `v = 1100 + 10000*d` in the existing unit (1/65536 item per 30 fps frame: 0.5 items/s = 1092, 4.6 /s = 10049); clamp d at >= 4; raise the gain above d = 1 only if fast scroll lags visibly | formula plus constants |
| 5 | Single step | 90 % 0.35-0.38 s, rest 0.48-0.51 s (I from F) | 90 % 0.25 s, rest 0.37 s | follows from row 4 | — |
| 6 | Pose vs position | pose leads, rate ~1.7x (F, low confidence) | pose is a function of position | tilt and depth from a second position `q` chasing the target with k ~8 /s; x from the displayed position | new state, small |
| 7 | Fast-scroll look | emerges from row 6 (I) | `flow_mix` pose over 7 frames | retire `flow_mix` once row 6 lands, or keep it as the approximation | — |
| 8 | Caption timing | target, on the click (M) | centre index, on arrival | show the target at the next frame | code change |
| 9 | Caption transition | instant (M) | instant | keep | — |
| 10 | Caption in fast scroll | redrawn at render rate (~15 Hz), skips items (M) | follows instant steps | redraw at most every other refresh (~33 Hz) | see row-cost note |
| 11 | Centre cover | 125 x 125 +- 1.5 (M) | 128 x 128 | keep 128 (owner's choice; 3 px larger) | — |
| 12 | Cover vertical centre | y 105 under a 23 px title bar (M) | y 91.5, no title bar | keep | — |
| 13 | First side cover | touching, 38.5 wide, inner edge 72/125 (M) | touching, 62.4 wide, inner 89.4/128 | Parallel Slides tilt 70 deg: 224.1-260.5 (36.4 wide), inner 85.5 | setting value |
| 14 | Stack spacing | 27.5 (M) | 19.6 (margin 20); auto 36.6 at 70 deg | ~27.5; the auto spacing needs an override or a tuck that lands there | small code change |
| 15 | Side covers shown | 3 + sliver, all full brightness (M) | 3, third at half brightness | draw the 4th at the edge; drop the half-brightness step | constants |
| 16 | Reflections | yes | no | out of scope (owner) | — |
| 17 | Placeholder | blue tile with white double note (M) | dark `EMPTY_SLIDE_COLOR` slab | optional: a note-tile placeholder | new art |
| 18 | Caption size | ~14-15 px bold, both lines; pitch 17.5 (M) | 18 px bold + 18 px regular; pitch 24 | see section 6 | theme fonts |
| 19 | Long title | centred, clipped at screen edges (M) | 280 px box, scrolls after 1.5 s | keep (stock never showed a title long enough to tell) | — |
| 20 | Render rate | 30 Hz slow, 15 Hz fast (M) | up to 60 Hz | do not copy | — |

Row 13's residual: AstraPod's inner edge stays ~15 % taller than stock
(85.5 vs ~74 px at 128 scale). The depth of a side cover's inner edge comes
from `MAXSLIDE_LEFT_R` and the camera distance, not from the tilt.

## 6. Caption geometry and the 150-row flush

**A single full-width rect over a 128 px cover and two caption lines cannot
be 150 rows or fewer.** The cover alone is 128 rows (132 with
`PF_ROW_MARGIN` 2), which leaves 18-22 rows for the gap and both lines. Two
lines of any readable font need at least ~26 rows (12-px font, no gap).

Minimum rect, `(art_top - 2) .. (y2 + line_height)`, with art rows 28-155:

| Fonts | Gap art to line-1 box | Pitch | y1 | y2 | Rect rows | Time |
|---|---|---|---|---|---|---|
| current 18 px (h 19) | 20 (now) | 24 (now) | 176 | 200 | 26-219 = **193** | 18.9 ms |
| current 18 px | 20 (= stock gap) | 20 | 176 | 196 | 26-215 = 189 | 18.5 ms |
| current 18 px | 4 | 20 | 160 | 180 | 26-199 = 173 | 17.0 ms |
| current 18 px | 0 | 19 | 156 | 175 | 26-194 = 168 | 16.5 ms |
| ~14 px (h 15), stock-like | 4 | 17 | 160 | 177 | 26-192 = 166 | 16.3 ms |
| 12 px (h 13) | 2 | 14 | 158 | 172 | 26-185 = 159 | 15.6 ms |

- AstraPod's current gap equals stock's: the line-1 box top sits 20 rows
  below the art, and stock's line-1 box top sits ~20.5 rows below its art.
  Moving the caption closer departs from stock. Only stock's line pitch
  (17.5) and font size are tighter.
- Ways to keep each refresh at <= 150 rows:
  1. **(Recommended)** On a frame whose caption changes, flush covers and
     caption in two consecutive refreshes (132 + 43 rows). Cover motion
     runs at ~33 Hz on those frames only. Stock itself renders at 15-30 Hz.
  2. Move the caption up (fourth row of the table, 168 rows) and accept a
     2-refresh frame whenever the caption changes; the rect is still
     over 150.
  3. Shrink the art to <= ~110 px (150 - 2 - 4 - 34): conflicts with the
     owner's 128.
- Code points: `PF_NOMINAL_ART_H` (`carousel.c:2766`), `PF_CAPTION_PAD`
  (`carousel.c:2771`), `caption_line2` pitch (`carousel.c:2789-2792`),
  `carousel_caption_layout` (`carousel.c:2802-2826`), caption band
  reservation (`carousel.c:4559-4565`), flush union
  (`carousel.c:6750-6795`).

## 7. Model to implement

```
state: target T (int), p (display, float), q (pose, float)

on wheel event (every 15 deg):
    T = clamp(T + dir, 0, n-1)          # no lead cap, nothing dropped
    caption_item = T                    # drawn at the next frame

each frame, dt seconds:
    p += step(T - p, k_p = 4.6, v0 = 0.5, dt)
    q += step(T - q, k_q = 8.0, v0 = 0.5, dt)
    cover i: x and order from (i - p); tilt and depth from (i - q)

step(d, k, v0, dt):
    s = min(|d|, (k*|d| + v0) * dt)     # never passes the target
    return sign(d) * s
```

| Constant | Value | Basis |
|---|---|---|
| k_p | 4.6 /s (4.0-5.25) | F, three settles, d < 0.7 |
| v0 | 0.5 items/s (0.3-0.65) | F |
| k_q | ~8 /s (1.6-1.9 x k_p) | F, one event; tune by eye |
| Degrees per item | 15 | F |
| Lead cap | none | M |

Expected behaviour: single step 90 % in ~0.35 s, at rest in ~0.48 s;
display speed equal to the input rate; no overshoot; smooth reversal. If
fast scroll trails the caption by more than ~2 items, raise k_p for
d > 1 (section 3.1, large distances).

## 8. Open questions

- No clean single click from rest was recorded. A 5 s clip with three
  isolated clicks would pin k, v0 and the start of a step.
- The pose-lead ratio (row 6) rests on one step. A slow single step filmed
  head-on would confirm it.
- Whether stock flushes the caption in the same panel refresh as the covers
  cannot be seen at 60 fps.
- The left side layout is taken by symmetry; no both-sided rest frame
  exists.
