# Stock Cover Flow: measured motion and layout

Reference for re-tuning `apps-ipod/screens/covers/carousel.c` to Apple's
stock iPod Classic Cover Flow. Measured from two phone videos of the stock
firmware, 60 fps, 1080x1920 portrait: **v1** (20.3 s, 1219 frames, mixed
scrolling) and **v2** (19.1 s, 1148 frames, isolated single clicks from
rest). Internals document, source paths are deliberate.

Every number is tagged **M** (measured from frames), **F** (fitted to
measured frames) or **I** (inferred: consistent with the evidence, not
proven). **(v2)** marks a number that v2 changed or first measured; the old
value follows in brackets. Frame numbers `fNNNN` are 0-based v1 frames,
`gNNNN` 0-based v2 frames (16.67 ms apart). Coordinates are LCD pixels,
320x240, origin top-left.

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

What v1 contains: rest on the first album (f0000-0058); slow forward
scroll (f0059-0370); fast forward (f0370-0590, partly over placeholder
tiles); fast reverse (f0590-0812); reverse-to-forward turn (~f0815-0835);
forward (f0835-0895); reverse back to the first album, finger lifted, rest
(f0910-1076); fast forward and stop with the finger left on the wheel
(f1076-1218). There is no isolated single click from rest: the thumb was
already turning when the first step started.

What v2 contains (all frames rectified, no glare losses): five isolated
single clicks from rest, each followed by >= 1 s of rest: g0365 forward
(Faded to Ignite), g0626 reverse (Spectre to Sing Me), g0766 reverse (Sing
Me to Ignite), g0918 forward (Ignite to Sing Me), g1065 forward (Sing Me to
Spectre). It also has two double clicks, ~20 frames (0.33 s) apart:
g0127/g0147 and g0488/g0510. These were not analysed. The owner asked for
three clicks; the video has five singles. Both side stacks are visible at
rest. v2 data: `/tmp/claude-1000/coverflow-ref/v2/` (`t?_out.txt`,
`t?_in.txt` per step; `scripts/stepfit2.py`, `jointfit.py`,
`jointfit_q.py`, `resbin.py`).

## 1. Static layout at rest

Sources: v1 f0005-0050 averaged (first album, right side only), f0030,
f1063; v2 g0570-0615 averaged (Spectre, both sides), with g0330-0360
(Faded), g0700-0760 (Sing Me) and g0850-0910 (Ignite) for the centre cover.

| Item | Stock value | Tag | Uncertainty |
|---|---|---|---|
| Title bar | rows 0-22.5, "Cover Flow" left, battery right | M | +-1 |
| Centre cover | **(v2) 125.5-127 wide, 125.0-125.5 tall; use 126 x 126** [v1: 125 x 125]. Spectre g0570-0615: W 126.8, H 125.5 (y 41.8-167.2) | M | +-1.5 |
| Centre cover centre | x 160.6-162.4 (= screen centre), y 105.1 | M | +-1.5 |
| Vertical alignment | every cover centred on y = 105.1; side covers grow and shrink symmetrically about it (no floor line) | M | +-1 |
| First side cover | inner edge touches the centre cover (gap 0); projected width **(v2) 40.3** [v1: 38.5] | M | +-1.5 |
| First side cover heights | inner edge **(v2) 65-69** [v1: ~72], outer edge 124 (same as centre) | M | +-3 |
| Stack spacing | **(v2) 28.2** px per cover (v2 outer edges L 28.5, 57.0; R 264.5, 292.5) [v1: 27.5] | M | +-0.5 |
| Left/right symmetry | **(v2) symmetric**: outer edges of L1/R1 sit 103.6/103.9 from the centre cover's centre, L2/R2 132.1/131.9; top-edge slopes L1 0.861, R1 -0.806, L2 0.638, R2 -0.618 | M | +-0.5 |
| Stack covers | width 27-28, inner edge ~104 tall, outer edge 125-130 | M | +-3 |
| Covers per side | 3 whole + a sliver of a 4th at the screen edge | M | — |
| Overlap | adjacent covers meet edge to edge; no visible overlap or gap | M | — |
| Side cover facing | inward (outer edge nearer the viewer and taller) | M | — |
| Dimming | none visible on side covers | M | camera exposure |
| Reflections | present under centre and side covers | M | not measured, out of scope |
| Background | pale blue-white, lighter toward the bottom | M | camera colours unreliable |
| Placeholder art | blue gradient square with a large white double-note glyph, used for centre and side covers alike | M | — |

The owner's 128 px target is 1.6 % (2 px) larger than the measured 126 +-
1.5 (v2). The difference is near the measurement limit and not visible at
a glance. The two videos' rectifications differ by ~1 px vertically
(battery icon), which is the size of the v1-v2 change.

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
| Too-long title | **(v2) left-aligned** in a box from x ~10 to ~309, clipped at the right edge, no ellipsis [v1: "centred, clipped by both edges", from 0.3-0.5 s glimpses; wrong]. Short titles are centred | M +-2 |
| Long-title scroll | **(v2)** starts **1.28 s** after the caption appears (77-78 frames: g0365 to g0442, g0766 to g0843-0844); text moves left at **~40 px/s** (39.5-40.8) in ~2 px steps every ~3 frames (~20 Hz). Scrolled >= 49 px without reaching the end; the end and any wrap were not recorded (the next click came first) | M |

## 2. Input: wheel to target

- **The caption shows the target, and it changes on the click.** The new
  caption appears 0-3 frames before the first visible cover motion: f0060
  caption with motion first visible f0060; f0330-0331 caption, incoming
  cover starts f0333-0334; f1192 caption, cover starts f1193-1194. **M**
  - **(v2)** In all seven v2 clicks the new caption and the first cover
    motion appear **in the same camera frame** (g0126, g0364, g0488,
    g0626, g0766, g0918, g1065). The fitted motion start lies 0-5 ms
    before that frame (section 3.1). So the caption and the first moved
    cover come from one rendered frame. **M**
  - **(v2) The delay from the physical click to that frame is not
    measurable.** The thumb rolls slowly before each tick, and the thumb
    proxy (+-5 deg) cannot place the 15 deg tick within ~100 ms.
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

#### (v2) Single steps from rest: one position, two slot shapes

**(v2) Result: one displayed position `p` drives every cover.** It obeys
the sum law `dp/dt = k*d + v0` (d = T - p, in items) with

- **k = 5.7 +- 0.4 /s, v0 = 0.2 +- 0.1 items/s.** **F**, five steps
  [v1: k = 4.6 +- 0.6, v0 = 0.5 +- 0.2].

Each cover's screen position and pose are fixed functions of its offset
`u = i - p`. These **slot shapes are not linear in u**: a cover leaving
the centre moves out and turns early, and a cover arriving moves in and
turns late. **F**

How it was fitted:

- Each step was tracked on two covers, the outgoing one (centre to first
  side slot) and the incoming one (first side slot to centre). Each track
  gives two proxies:
  - position: the projected centre `cx`
  - pose: the height of the inner edge, the edge nearer the screen centre
- Each proxy was normalised to its rest values (tracker units):
  - centre: cx 161.0, inner edge 123
  - first side slot: cx +-73.6, inner edge 57.5
- The template fit reads narrow side covers ~8 px shorter than the edge
  measurement in section 1. Both rest values come from the same tracker,
  so the normalisation is self-consistent.
- **Fitted separately, the two covers disagree.** The outgoing cover's
  centre settles at k ~ 6.9 /s with no floor. The incoming cover's centre
  settles at k ~ 4.4 /s with v0 ~ 0.4. Its time to 50 % is 133-167 ms,
  against 67-100 ms for the outgoing cover.
- **One `p` with a concave slot shape explains both** (`jointfit.py`):
  - outgoing progress = f(p)
  - incoming progress = 1 - f(1 - p)
  - f(0) = 0, f(1) = 1
  - With f(x) = x^g: g_x = 0.83 +- 0.02 for position and g_h = 0.68 +-
    0.02 for pose.
  - The exponential family `(1-e^(-gx))/(1-e^(-g))` fits equally well.
    The table below uses it.

Per-step joint fit (power family). RMS is in item fractions; the tracker
noise is ~0.02.

| Step | dir | t0 vs caption frame | k /s | v0 items/s | g_x | g_h | RMS |
|---|---|---|---|---|---|---|---|
| g0365 | fwd | -20 ms (white cover, weak edges) | 5.75 | 0.20 | 0.84 | 0.72 | 0.028 |
| g0626 | rev | -1 ms | 5.71 | 0.25 | 0.83 | 0.68 | 0.022 |
| g0766 | rev | 0 ms | 6.29 | 0.14 | 0.82 | 0.66 | 0.024 |
| g0918 | fwd | +1 ms | 5.71 | 0.28 | 0.80 | 0.67 | 0.020 |
| g1065 | fwd | 0 ms | 5.30 | 0.30 | 0.84 | 0.69 | 0.024 |
| mean +- sd | | | **5.75 +- 0.35** | **0.23 +- 0.06** | 0.83 | 0.68 | |

The exponential family gives k = 5.58 +- 0.33, v0 = 0.16 +- 0.06. The
error bars above (k +- 0.4, v0 +- 0.1) cover both families and the spread
across steps. Forward and reverse steps do not differ.

Slot shapes, as progress from the centre (0) to the first side slot (1)
against |u| (screen level, exponential family, g_x = 0.56, g_h = 1.15):

| abs(u) | 0.1 | 0.2 | 0.3 | 0.4 | 0.5 | 0.6 | 0.7 | 0.8 | 0.9 |
|---|---|---|---|---|---|---|---|---|---|
| screen x | 0.13 | 0.25 | 0.36 | 0.47 | 0.57 | 0.67 | 0.76 | 0.84 | 0.92 |
| inner-edge height | 0.16 | 0.30 | 0.43 | 0.54 | 0.64 | 0.73 | 0.81 | 0.88 | 0.94 |

- **The law holds over the whole step, d = 1 to 0** (`resbin.py`). With
  the sum law the mean residual in each fifth of the step is +-0.012 or
  less. With v0 = 0 (pure exponential, best k = 6.5 /s) the residual is
  biased +0.016 over d = 0.2-0.8 and -0.016 near rest. **F**
- **d > 1 is still not measured.** The double clicks reach only d ~ 1.1.
- Predicted for `p`, one item:
  - 50 % at 114 ms
  - 90 % at 349 ms
  - 98 % at 496 ms
  - rest at **567 ms**
- On screen the outgoing cover is half way at ~90 ms, the incoming cover
  at ~140 ms.
- Measured on screen, five steps:
  - 50 %: outgoing 67-100 ms, incoming 133-167 ms
  - 90 %: outgoing 317-350 ms, incoming 383-400 ms
  - within 2 % for good: 467-700 ms
- [v1 predicted 90 % at 0.35-0.38 s, rest at 0.48-0.51 s.]
- **Why v1 differed.** v1 fitted one cover at a time, mostly the arriving
  one, which lags `p` through its slot shape. A slower k with a larger
  floor imitates that lag.
- **(v2) Render rate during a single step: ~30 Hz.** A new image comes
  every 1-2 camera frames, as in v1's settles. **M**

#### v1 fits (superseded for single steps)

Fits (`scripts/fitsettle.py`; `d` in items, taken from the arriving
cover's left edge, 125 px = 1 item, valid for d < 0.7). Three laws were
tried: pure exponential `v = k*d`; sum `v = k*d + v0`; floor
`v = max(k*d, v0)`.

| Settle | d0 | Exponential k (RMS) | Sum k, v0 (RMS) | Floor k, v0 (RMS) |
|---|---|---|---|---|
| f1041-1064, finger lifted, reaches rest | 0.53 | 6.7 (4.5 px) | 4.5, 0.65 (2.8 px) | 6.5, 1.15 (3.3 px) |
| f1199-1217, finger on wheel, video ends first | 0.49 | 5.2 (3.6 px) | 4.0, 0.45 (2.2 px) | 5.3, 1.15 (2.6 px) |
| f0336-0358, slow scroll, next click at f0359 | 0.69 | 6.1 (2.1 px) | 5.25, 0.30 (1.7 px) | 6.0, 0.95 (1.8 px) |

- The sum law fits best: k = 4.6 +- 0.6 /s, v0 = 0.5 +- 0.2 items/s, on
  the arriving cover only. **Superseded by the v2 joint fit above.** **F**
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

**(a) Single click from rest.** **(v2) Measured on five steps**:

- The incoming cover does not move before the caption frame.
- In that frame both covers move, and the motion follows the joint fit
  of 3.1: `p` 90 % at ~0.35 s and at rest at ~0.57 s.
- There is no overshoot, and no wobble at rest within +-1 px.

[v1: not isolated; predicted rest at 0.48-0.51 s.] **M/F**

v1: the first step of the video (click ~f0059) moved the
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

**(d) Sustained fast scrolling** **(v3: the 3D description in section 9.4 supersedes the screen-space notes below.)** (13-18 items/s, f0425-0460, f0764-0812,
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

**(v2) In single steps the lead comes from the slot shape, not from a
faster pose chase.**

- The 1.6-1.9x pose rate below is **rejected**.
- A joint fit with a second pose state `q` and its own rate
  (`jointfit_q.py`) returns **k_q / k_p = 0.96 +- 0.05**, five steps,
  both shape families. **F**
- A faster `q` would turn the incoming cover early too. Instead the
  incoming cover turns *late*: pose 50 % at 183-233 ms, position 50 % at
  133-167 ms. **M**
- The outgoing cover's pose and position run nearly together. Pose leads
  position only through `g_h < g_x`, and the lead is largest near the
  centre. **F**
- v1's table used 72 as the side-slot inner edge. On v2's self-consistent
  scale (57.5) those pose values become 0.23, 0.46, 0.58 and 0.60. The
  v2 shapes predict 0.25, 0.44 and 0.55 at the same positions. The v1
  event agrees with v2.

v1 data, kept for reference: in the first step (f0060-0066) the outgoing
cover's pose progress, from the height of its inner edge (125 to ~72),
runs ahead of its position progress (projected centre x 162.7 to ~85):

| Frame | position | pose |
|---|---|---|
| f0060 | 0.19 | 0.28 |
| f0062 | 0.37 | 0.57 |
| f0064 | 0.48 | 0.72 |
| f0065 | 0.58 | 0.74 |

- v1 read this as a pose rate **1.6-1.9 times** the position rate.
  **Rejected by v2 (see above).**
- The incoming cover is near frontal while still ~0.5 item off centre
  (f1198-1200), then slides the rest of the way. **M** This was after a
  fast scroll, with d > 1. No single step in v2 shows it.
- **Still open: the fast-scroll look in 3.2 (d).** The single-step model
  does not explain it. With one `p` the cover nearest the centre has
  |u| <= 0.5 and is never more than ~65 % turned. Stock in fast scroll
  shows the turning zone 60-100 px off centre on the incoming side. A
  pose lead that grows with speed or distance (d > 1) could do this. A
  pose lead that is present at d <= 1 cannot: v2 bounds it at k_q / k_p
  <= ~1.05. **I**

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
| 4 | Speed law | **(v2)** `k*d + v0`, **k = 5.7 +- 0.4 /s, v0 = 0.2 +- 0.1 items/s** for d <= 1 (F, five steps) [v1: 4.6, 0.5]; d > 1 not measured | sin curve: effective k 9.2 /s (small d) to 13.9 /s (d = 1), floor 0.76, d clamped at 2 | **(v2)** `v = 440 + 12450*d` in 1/65536 item per 30 fps frame (0.2 items/s = 437, 5.7 /s = 12452) [v1: `1100 + 10000*d`]. On `t3/coverflow-stock-motion` (thousandths per second): `FLOW_FLOOR 200`, `FLOW_GAIN 5700` [now 500, 4600]. Keep d clamped at >= 4 (`FLOW_REACH`) | constants |
| 5 | Single step | **(v2)** `p` 90 % at 0.35 s, rest at 0.57 s; on screen the outgoing cover is half way at ~90 ms, the incoming at ~140 ms (M/F) [v1: rest 0.48-0.51 s, I] | 90 % 0.25 s, rest 0.37 s | follows from rows 4 and 6 | — |
| 6 | Pose vs position | **(v2) one position drives both; no faster pose chase** (k_q/k_p = 0.96 +- 0.05, F). Pose and x are concave functions of abs(u), pose more so (section 3.1 table) [v1: pose chase ~1.7x, rejected] | pose is a function of position | **(v2)** set `FLOW_POSE_GAIN = FLOW_GAIN` (5700) [was 8000], or place both from `slide_frame`. On the branch renderer (parallel, tilt ~70) keep `flow_cx` linear in abs(r); make the angle `itilt * a(abs(r))` with `a(u) = u*(3+u)/4` (section 7) | small code change |
| 7 | Fast-scroll look | not explained by single steps; needs a pose lead at d > 1 (I, open) | `flow_mix` pose over 7 frames | keep the d > 1 behaviour open; a pose lead may apply only at d > 1 | — |
| 8 | Caption timing | target, on the click (M) | centre index, on arrival | show the target at the next frame | code change |
| 9 | Caption transition | instant (M) | instant | keep | — |
| 10 | Caption in fast scroll | redrawn at render rate (~15 Hz), skips items (M) | follows instant steps | redraw at most every other refresh (~33 Hz) | see row-cost note |
| 11 | Centre cover | **(v2) 126 x 126 +- 1.5** (M) [v1: 125] | 128 x 128 | keep 128 (owner's choice; 2 px larger) | — |
| 12 | Cover vertical centre | y 105 under a 23 px title bar (M) | y 91.5, no title bar | keep | — |
| 13 | First side cover | touching, **(v2) 40.3 wide (41.0 at 128 scale), inner edge 65-69** (M); both sides symmetric within 0.5 px [v1: 38.5, 72] | touching, 62.4 wide, inner 89.4/128 | **(v2)** tilt **67-68 deg** gives 41 px (65 deg: 45.7; 70 deg: 36.4) [v1: 70 deg] | constant |
| 14 | Stack spacing | **(v2) 28.2 +- 0.5** (M) [v1: 27.5] | 19.6 (margin 20); auto 36.6 at 70 deg | `PF_STACK_PITCH` 28 (branch has 27.5; inside the error) | constant |
| 15 | Side covers shown | 3 + sliver, all full brightness (M) | 3, third at half brightness | draw the 4th at the edge; drop the half-brightness step | constants |
| 16 | Reflections | yes | no | out of scope (owner) | — |
| 17 | Placeholder | blue tile with white double note (M) | dark `EMPTY_SLIDE_COLOR` slab | optional: a note-tile placeholder | new art |
| 18 | Caption size | ~14-15 px bold, both lines; pitch 17.5 (M) | 18 px bold + 18 px regular; pitch 24 | see section 6 | theme fonts |
| 19 | Long title | **(v2)** left-aligned in x ~10-309, clipped; scrolls left after **1.28 s** at **~40 px/s** in ~2 px steps (M) [v1: "centred, no scrolling", wrong] | 280 px box, scrolls after 1.5 s | start after ~1.3 s at ~40 px/s; widen the box to ~300 px | constants |
| 20 | Render rate | 30 Hz slow, 15 Hz fast (M) | up to 60 Hz | do not copy | — |

Row 13's residual: AstraPod's inner edge stays ~25 % taller than stock
(85.5-86.6 vs **(v2)** 66-70 px at 128 scale) [v1: ~15 %, 74 px]. The depth of a side cover's inner edge comes
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

**(v3) Superseded for transitions.** One display position (below) cannot
produce the slow-to-fast fan. Section 9.6 gives the supported per-cover
form. The v2 constants remain the per-cover rate (k = 5.7 /s).

**(v2)** One chase. The pose lead is in the slot shape, not in a second
state.

```
state: target T (int), p (display, float)

on wheel event (every 15 deg):
    T = clamp(T + dir, 0, n-1)          # no lead cap, nothing dropped
    caption_item = T                    # drawn in the same frame as the
                                        # first motion

each frame, dt seconds:
    p += step(T - p, k = 5.7, v0 = 0.2, dt)
    cover i, u = i - p, a = min(|u|, 1):
        |u| <= 1: world x = sign(u) * offsetX * a          # linear
                  tilt    = sign(u) * itilt * a*(3+a)/4    # late start
        |u| >  1: first slot + (|u| - 1) * stack pitch, full tilt

step(d, k, v0, dt):
    s = min(|d|, (k*|d| + v0) * dt)     # never passes the target
    return sign(d) * s
```

The two `|u| <= 1` lines are for the branch renderer: parallel slides,
tilt ~70 deg, projected centre `cx*240/(240 + 63.5*|sin tilt|)`, inner
edge `128*240/(240 + 127.5*|sin tilt|)`. Through that projection they
reproduce the stock screen shapes of section 3.1.

The linear world x already gives the concave screen x, because a cover
recedes as it turns. Without the `(3+a)/4` factor the inner edge would
lead stock by up to 0.06 at |u| = 0.5.

Exact angle fractions against |u|:

| abs(u) | 0.1 | 0.2 | 0.3 | 0.4 | 0.5 | 0.6 | 0.7 | 0.8 | 0.9 |
|---|---|---|---|---|---|---|---|---|---|
| angle / itilt | 0.086 | 0.173 | 0.260 | 0.348 | 0.438 | 0.531 | 0.628 | 0.733 | 0.851 |
| `a(3+a)/4` | 0.078 | 0.160 | 0.248 | 0.340 | 0.438 | 0.540 | 0.648 | 0.760 | 0.878 |
| world x / offsetX | 0.105 | 0.209 | 0.313 | 0.416 | 0.518 | 0.618 | 0.717 | 0.814 | 0.908 |

| Constant | Value | 1/65536 per 30 fps frame | Branch unit (thousandths/s) | Basis |
|---|---|---|---|---|
| k (gain) | **5.7 /s** (5.3-6.3) | 12450 | `FLOW_GAIN 5700` | F, five steps, d <= 1 (v2) [v1: 4.6] |
| v0 (floor) | **0.2 items/s** (0.1-0.3) | 440 | `FLOW_FLOOR 200` | F (v2) [v1: 0.5] |
| pose chase | **same as k** (k_q/k_p 0.96 +- 0.05) | — | `FLOW_POSE_GAIN 5700` | F (v2) [v1: ~8 /s, rejected] |
| Distance clamp | d >= 4 | — | `FLOW_REACH 4<<16` | not measured |
| Degrees per item | 15 | — | — | F |
| Lead cap | none | — | — | M |

Expected behaviour: single step 90 % in ~0.35 s and at rest in ~0.57 s.
The outgoing cover moves first and fastest. The incoming cover turns to
face the viewer late. There is no overshoot. The display speed equals the
input rate. Reversal is smooth. If fast scroll trails the caption by more
than ~2 items, raise k for d > 1. If it lacks stock's off-centre turning
zone, add a pose lead for d > 1 only (section 3.3, open).

## 8. Open questions

- **(v3)** Sustained fast scroll and the reversal slats are not reproduced
  by any tested model (9.5). A clip at steady wheel speed with the wheel's
  ticks visible (high frame rate) would give the target exactly and pin
  the lag, the cap and the fast pose.

- ~~No clean single click from rest~~: answered by v2 (3.1, 3.2 a).
- ~~Pose-lead ratio~~: answered by v2; no separate pose chase at d <= 1
  (3.3).
- ~~Left side by symmetry~~: answered by v2; symmetric within 0.5 px.
- **d > 1 is unmeasured.** This covers the gain at large distance and
  the fast-scroll turning zone (3.2 d, 3.3). A clip of 2-3 quick clicks
  from rest, about 0.1 s apart, would show d = 2-3 without
  fast-scroll aliasing. v2's double clicks (g0127/0147, g0488/0510) give
  only d ~ 1.1.
- The click-to-motion delay is unmeasurable with a thumb-tracked click.
  It needs the wheel's tick and the screen in one high-speed shot.
- Long-title scroll: what happens at the end of the text (stop, pause,
  wrap) was not recorded.
- Whether stock flushes the caption in the same panel refresh as the covers
  cannot be seen at 60 fps. v2 shows them in the same camera frame.

## 9. (v3) The scene in 3D, and the fast regime

Sources: v1 stills chosen by the owner: f0395 (6.59 s) and f0901 (15.02 s),
slow to fast; f0819 (13.65 s), fast. Supporting frames: f0380, f0424, f0432,
f0446, f0560, f0700, f0790 and f1154. f0901 had no automatic LCD fit. It was
rectified with a quad interpolated between f0896 and f0908, so its
positions carry about +-3 px more error. Scripts: `scripts/topview.py`,
`segs.py`, `sim3d.py`, `run3d.py`, `cmp3d.py`. Images: `v3/tv3.png` (top
views), `v3/cmpA2.png` and `v3/cmpUA.png` (model against still).

### 9.1 Method

- Every cover is centred on the horizon y = 105.1, so the camera sits at
  cover-centre height. Each image column's cover height `h` (twice the
  distance from the top silhouette to the horizon) maps one column to one
  point in the scene. With S = 1 cover width:
  - X = (x - cx) / h
  - z = F / h
- The top-edge silhouette of a frame is therefore a **top view of the
  scene**: the nearest cover at every column. The silhouette is detected
  against the background, so white and pale covers drop out.
- Models are scored against this profile (RMS of `h`, px). Detection
  noise is ~8 px: that is the rest layout's own residual.

### 9.2 Camera (M/F)

- **One pinhole camera for the whole scene.** Stock does not project each
  cover separately as AstraPod's Parallel Slides does.
- Evidence: the top edges of the stacked covers on one side meet in one
  vanishing point (v2 rest: x ~129 left, ~188 right). Separate
  projections would put the stacked covers' vanishing points 28 px apart.
- **F = 135 +- 10 px.** Fitted to 168 unoccluded cover quads from the v2
  single steps, with each cover's length fixed at 1. Length residual 5 %.
- Principal point x = 158-160.5. The fit cannot pin it closer.
- Centre cover depth **z0 = F/126 = 1.07 S**. Horizontal field of view
  ~100 deg.
- AstraPod: cover 128 at `CAM_DIST` 240, so z0 = 1.875 S and F = 240 px.
  Stock's camera is **1.75 times closer and much wider**.

### 9.3 Rest layout in 3D (F, h-profile RMS 7.9 px)

| Item | Value |
|---|---|
| Centre cover | frontal, centre at X = 0, z = 1.07 |
| Side covers | **yaw 90 deg** (perpendicular to the screen, inner face visible because of the wide lens) |
| Near (outer) edge of side covers | on the centre cover's plane, z = 1.06 |
| First side slot | near edge at abs(X) = **0.817** |
| Stack spacing | **0.22** S per cover |

- The single-step screen shapes of section 3.1 **follow from this scene
  with no fitting**. Interpolate X, z and yaw linearly over one slot,
  then project. Screen-x progress at abs(u) = 0.1 to 0.9 comes out
  0.14 0.27 0.38 0.49 0.59 0.69 0.77 0.85 0.93, against measured 0.13
  0.25 0.36 0.47 0.57 0.67 0.76 0.84 0.92.
- The concave screen shape is perspective.
- The inner-edge proxy runs ahead of the measured curve (0.75 against
  0.64 at abs(u) = 0.5). It would match with yaw at 0.45 times the
  position rate. That contradicts the fan stills (9.5).
- The v2 tracker reads the side inner edge low: 57.5 against the model's
  65.5. So this mismatch is not counted as evidence.

### 9.4 What the stills show, in 3D (M)

- **f0395 and f0901: concave chain ("fan").**
  - The covers join edge to edge in a U.
  - The frontal centre cover sits **0.5 S behind rest** (z 1.55-1.6;
    screen height 92-97 against 126).
  - Its neighbours turn in at moderate yaw: f0395 yellow ~40-55 deg. Each
    further cover is turned more and is nearer: the outer near edges are at
    z ~1.0-1.05 and 112-135 px tall.
  - Nearer covers are drawn over farther ones: the yellow cover hides the
    left 16 px of the frontal one.
  - Both stills are moments when the target outruns the display:
    - f0395 is 0.42 s after the thumb went from ~3 to ~14 items/s.
    - f0901 is the turn from +11 to -8 items/s.
- **f0819: stack slats through the centre.**
  - Every cover is at yaw 75-90 deg.
  - Near edges lie on z = 1.02-1.09 at a **uniform X spacing of 0.21**
    from screen edge to screen edge, with no centre slot.
  - Near the centre the covers are edge-on slivers (3-10 px wide).
  - The thumb is reversing (-16 to +7 items/s), and the caption still
    counts down until ~f0825.
- **f0560, f1154, f0432-0446: forward fast scroll, shingles.**
  - The covers keep stack spacing (~0.2) across the whole width.
  - They share one moderate yaw with the **outgoing** orientation: in a
    forward scroll every cover, including those right of centre, faces as
    the left stack does.
  - The row sits at z ~1.1-1.25.
  - The incoming side shows the turning zone: a near-frontal cover at
    x 258-300 in f1154, and covers reaching back to z 1.5-2.
  - Yaw is ~20-40 deg in f0560 and f1154 and ~60 deg in f0446. These are
    short visible pieces, so the uncertainty is +-15 deg.
- **f0700: reverse fast scroll, -25 items/s.** The incoming (left) side
  is a flat, overlapping frontal ribbon at z ~1.17. The outgoing side is
  steep slats at stack spacing.

### 9.5 Models tested

All models use the 3D camera and layout above. The target T comes from the
thumb track at 15 deg per item (scale +-20 %). Each still takes the best
of +-6 frames and four target phases.

| Model | f0395 | f0901 | f0560 | f0700 | f0819 | Verdict |
|---|---|---|---|---|---|---|
| B: one display position p (v2 law), every cover from slot(i - p) (as `fb2b010a7d`) | 18.5 | 20.9 | 15.5 | 13.2 | 23.1 | **rejected**: no fan, no slats |
| A: every cover chases its own 3D slot pose slot(i - T): X, z and yaw, first order, k = 5.7 /s each | **6.3** | **8.2** | 12.4 | 10.7 | 21.3 | fans at noise level; fast look wrong |
| A2: same, critically damped spring, omega = 8 /s | **5.2** | **6.3** | 12.3 | 11.0 | 20.9 | as A |
| A with yaw at 0.45 k (from the v2 inner-edge proxy) | 15.8 | 13.8 | 17.2 | 16.2 | 23.8 | rejected |
| U: as A, but yaw only toward 0 (target) or 90 (all others), side from X | 15.1 | 14.2 | 23.2 | 22.6 | 21.0 | rejected |
| P: yaw and z per cover from T; X packed by each cover's footprint, anchored at p | 38 | 31 | 43 | 44 | 25 | rejected as built (implementation not checked further) |

- A sweep of A's separate X, z and yaw rates (2-25 /s) gives no f0819
  better than 19 px. That failure is structural.
- **Supported (F): per-cover chase (A/A2).** It reproduces both
  slow-to-fast stills from one rule with the single-step rate. Each cover
  carries its own 3D state and chases the rest slot pose for the
  *target*.
  - The fan follows from that rule.
  - The covers T has just passed are part way through their swing at the
    same moment.
  - The frontal one stays back because its z target was 0 for only 1/r
    seconds.
- **Not reproduced (failed): sustained fast and the reversal slats.** In
  A and A2 the covers in transit swing through frontal and spread out.
  Stock keeps them at stack spacing and steep or shingled, and they never
  turn frontal in mid-transit.
- What a full model must add (I, untested):
  - The centre gap opens only around a cover that is actually frontal.
    One way is X packed by each cover's current footprint.
  - Yaw flips on the incoming side, ahead of the display, so covers
    arrive at the centre already in the outgoing orientation. This
    matches 3.3's "turning zone on the incoming side".
- **The magnitudes of d > 1 were not measured.** That covers lag in
  items, display speed against wheel rate, a speed cap and gain against
  distance. The thumb proxy drifts 116 deg (~8 items) over the video, and
  the caption skips items above the render rate, so neither gives the
  target to better than a few items in fast segments.

### 9.6 For AstraPod (I)

- Plugging stock's scene into `carousel.c` / `tools/pfgeom.c` needs:
  - single-camera projection (Parallel Slides off)
  - `CAM_DIST` ~137 for a 128 cover (1.07 S) instead of 240
  - side tilt 90 deg, with the near edge on the centre plane
  - first slot near edge at 104.6 (0.817 x 128)
  - stack spacing 28.2 (0.22 x 128)
- The stack covers' screen look (near edge 126 tall, falling to ~70-90
  inside 28 px) comes from that wide lens. AstraPod's 240 camera cannot
  show it at any tilt.
- Per-cover chase, per refresh with measured dt:
  - Each slide i keeps (X_i, z_i, yaw_i) in PFreal.
  - Each refresh: `q += (slot_q(i - T) - q) * a` with `a = 1 - exp(-5.7 * dt)`
    (table, or `a ~ k*dt / (1 + k*dt/2)`).
  - Spring form (A2): `v += (w*w*(tgt - q) - 2*w*v) * dt; q += v * dt`,
    w = 8 /s.
  - When T moves, the targets change and nothing restarts.
- This reproduces the rest layout, the single step and the slow-to-fast
  fan. It will **not** give stock's sustained fast look (9.5).

### 9.7 (v3.1) The owner's hypothesis H: translation from p, flip lagging

**H.** The row translates with the display position p, which chases T by
the v2 law. Each cover's yaw is a delayed follower of its rest yaw
yaw_rest(i - p). Two variants:

- **H1 (spacing from offset).** X from slot_X(i - p).
- **H2 (spacing from own yaw).** A chain anchored at p. Consecutive
  centres are `0.22 + 1.194*(|cos yaw_i| + |cos yaw_i+1|)/2` apart, so
  the X extents of neighbours never overlap and no two covers can
  intersect.

The `y` variants (H1y, H2y) take depth from the cover's own yaw, pivoting
about its near edge, instead of from the offset. Models B, A and A2 are
from 9.5. Same scoring as 9.5, with three frames added: f1154 (forward
fast), f0830 (just after the f0819 reversal) and f1205 (settling after
the stop at f1191).

| Model | f0395 | f0560 | f0700 | f0819 | f0901 | f1154 | f0830 | f1205 | mean |
|---|---|---|---|---|---|---|---|---|---|
| B (p, yaw instant) | 18.5 | 15.6 | 13.2 | 23.3 | 21.3 | 25.6 | 33.3 | 14.0 | 20.6 |
| **A** (per cover to slot(i - T), k 5.7) | **6.2** | 12.4 | 10.9 | 21.2 | **7.8** | 15.2 | 32.6 | 13.2 | **14.9** |
| **A2** (spring, omega 8) | **5.3** | 12.3 | 11.0 | 21.1 | **6.3** | 15.3 | 32.8 | 13.2 | **14.7** |
| H1, yaw lag k 8 / 4 / 2 /s | 32 / 42 / 54 | 35 / 50 / 62 | 38 / 50 / 62 | 28 / 29 / 40 | 25 / 35 / 45 | 37 / 48 / 56 | 32 / 32 / 34 | 14 / 20 / 34 | 30 / 38 / 48 |
| H1, spring omega 16 / 8 | 34 / 47 | 41 / 58 | 41 / 58 | 28 / 29 | 26 / 38 | 40 / 51 | 33 / 32 | 14 / 22 | 32 / 42 |
| H1, pure delay 50 / 100 ms | 30 / 45 | 37 / 51 | 38 / 51 | 26 / 28 | 23 / 30 | 41 / 48 | 34 / 35 | 13 / 14 | 30 / 38 |
| H1y, lag k 8 | 27.6 | 35.4 | 35.0 | 24.0 | 21.4 | 38.4 | 31.3 | 15.8 | 28.6 |
| H2 (chain), yaw instant / lag 8 / lag 4 | 23 / 43 / 52 | 17 / 45 / 62 | 16 / 45 / 62 | 23 / 37 / 46 | 23 / 34 / 44 | 27 / 42 / 58 | 34 / 33 / 35 | 16 / 19 / 31 | 22 / 37 / 49 |
| H2y (chain), yaw instant / lag 8 | 21.9 / 37.7 | 16.5 / 50.9 | 15.9 / 50.6 | 23.7 / 31.8 | 22.7 / 34.6 | 22.1 / 46.0 | 33.3 / 32.2 | 16.3 / 22.5 | 21.6 / 38.3 |

Single step: time-domain RMS against the five v2 steps, over four curves
(centre and inner edge of the outgoing and the incoming cover).
`scripts/step3d.py`.

| Model | RMS |
|---|---|
| B (offset, yaw instant) | **0.055** |
| A, all rates 5.7 | 0.067 |
| A, yaw 8 /s | 0.057 |
| H1, yaw lag k 16 / 8 / 4 / 2.6 | 0.067 / 0.095 / 0.151 / 0.191 |
| H2 chain, yaw instant | 0.093 |
| H2 chain, yaw lag k 16 / 8 / 4 | 0.113 / 0.140 / 0.193 |

Findings:

- **H is rejected (F).**
  - Every flip lag or delay makes every still and the single step worse
    than no lag. The score rises monotonically from B (lag 0) as the lag
    grows.
  - H does not produce the fan.
  - H's corollary points the wrong way:
    - A lagging flip happens *after* a cover passes the centre, on the
      outgoing side.
    - Stock turns covers on the **arriving** side in forward fast scroll:
      in f1154 a near-frontal cover sits at x 258-300, and in f0560 the
      turning covers reach back to z 1.5 on the right.
    - In stock the flip *leads* the display. That is the behaviour of A,
      whose covers chase the target T.
- **Spacing from own yaw (chain) does not help (F).** With yaw instant it
  scores 21.6-22.4 against B's 20.6. On the single step it scores 0.093
  against 0.055.
- **No-interpenetration constraint (M/F).**
  - Stock's scene is collision-free without any constraint. With
    sides at yaw 90 and near edges on the centre plane, the offset layout
    (X, z, yaw interpolated over one slot) gives **no intersection
    between neighbours at any point of a step** (checked at p = 0, 0.1,
    ..., 1).
  - AstraPod clips because of its own geometry: sides at 55-67.5 deg
    about the near edge, a 1.875 S camera and per-slide projection. Its
    motion law is not the cause.
  - Stock's neighbours are not pushed on contact. In v2 steps F and G the
    left neighbour's near edge starts moving in the caption frame. Its
    progress at 33 / 100 / 200 / 300 / 400 ms is 0.14 / 0.43 / 0.68 /
    0.82 / 0.93, and p (v2 law) predicts 0.18 / 0.45 / 0.71 / 0.85 /
    0.94. **M**
  - So adopting stock's scene (9.6) removes the clipping. A collision
    rule is not needed.
- **Still open:** f0819, f0830 (21-33 for every model) and the shingles of
  f0560 / f1154 (12-15 for A). The best model remains A / A2. Its
  remaining fault is that covers in transit swing through frontal and
  spread out, where stock keeps them at stack spacing.

### 9.8 (v4) Slow-motion reference IMG_3742.mov

Source: iPhone slow motion, 1920x1080, exported at 30 fps, 3452 frames.
The video carries location metadata. It stays outside the repository and
is referenced only as a symlink in the scratch area. Working data:
`/tmp/claude-1000/coverflow-ref/v4/` (`rect/` LCD 640x480, `dev/` device
plane with the click wheel, `theta.npy` finger angle, `flow.npy` stack
shifts, `diff.npy` render changes).

**Time map (M).**
- Frames 0-~100: normal speed (1/30 s per frame). The finger moves 3 deg
  per frame, and the scene changes in every frame.
- Frames ~100-~115: the iPhone speed ramp.
- Frames ~115-~3340: 240 fps capture played 8x slow (1/240 s per frame).
  This agrees with the finger: its real angular speed is continuous across
  both boundaries (0.6 rev/s before the ramp, 0.5-0.7 rev/s after it; and
  -0.5 rev/s before the end ramp, -1.5 rev/s after it).
- Frames ~3340-3452: normal speed.
- The analysed span is frames 115-3236 (13.0 s real). The LCD is lost to
  glare in frames 473-498, 2500-2506 and 3237-3353.

**Render cadence (M).** The stock renderer updates every 7-9 capture
frames (27-34 Hz) while it settles or scrolls slowly, and every 14-18
frames (13-17 Hz) in steady fast scroll. A still scene issues no updates.

**Input (M).** The fingertip is tracked on the click wheel in the device
plane, in all but 121 of 3452 frames.
- Frames 115-1430 (5.5 s): forward. The finger accelerates from 0.5 to
  2.2 rev/s and holds 1.9-2.3 rev/s from frame 740 to 1340.
- Frames 1430-1850 (1.75 s): the finger rests on the wheel.
- Frames 1850-3236 (5.8 s): back, at 0.5-2 rev/s, with several slowdowns.

**The caption leads the covers (M).** In scroll the caption names an
album ahead of the one at the centre. At frame 288 the caption reads
"STAY" (The Kid LAROI) while the centre holds K-391's "Play"; the order
is Play, Monsters, STAY, so the caption is 2 items ahead. The caption
crossfades between names. The caption is the best available reading of
the target T.

**Steady fast scroll (M).**
- Every cover on screen is a slat. No cover is frontal, and the slats
  keep near-stack spacing across the whole width (frames 1000-1448).
- Top views (F = 135) put the near edges on a line from z 1.07 at the
  centre to z 1.3-1.35 at X +1.5 on the arriving side. The arriving-side
  slats show more of their faces than the departing-side slats.
- The stack moves 28-46 LCD px per render on the departing side, which is
  1.0-1.6 items per render, or about 15-25 items/s at a finger speed of
  2 rev/s. Faster passages exceed what matching can resolve: above about
  3 items per render the slat pattern aliases. These speeds are lower
  bounds.

**Stop after fast scroll (M).** This is the cleanest event in the video.
- The finger stops at frame ~1430.
- The display runs on for 70 frames (0.29 s) and moves about 11 items
  (+/-3, from stack shifts) as slats. This is the lag d at release.
- At frame 1494 the arriving cover is already frontal (a flat top-view
  segment at z 1.1, X 0.2-0.55). Its right neighbour is drawn in front of
  it and hides its right half. Between frames 1494 and 1498, one render,
  the draw order swaps: the arriving cover is drawn on top and shows whole,
  0.61 S right of centre (77 LCD px), still at z 1.07-1.1. What changes in
  that render is the **draw order**, not the yaw (corrected in 9.9).
- The frontal cover then slides to the centre with no further turning.
  The remaining distance decays exponentially: 62, 48, 36, 27 and 19 px
  at frames 1506, 1517, 1524, 1534 and 1542. That gives **k = 7 +/- 1 /s**
  (tau 0.14 s), close to v2's k = 5.7 (F). The cover is centred by frame
  ~1610 (0.42 s after the flip).
- So the flip **leads** the translation. At X = 0.61 the offset layout
  (9.3) would give the cover a yaw of 67 deg; stock already shows 0 deg.

**What this means for the model (I).**
- Position: one scroll position p chases T at about the rate measured in
  v2 (k 6-7 /s near the end of travel).
- Yaw is keyed to the target, not to p. During fast scroll T is about 11
  items ahead of p, so the cover with i = T sits off screen to the right.
  Every cover on screen has i < T and takes a slat yaw: no centre cover
  turns frontal. When T stops, the cover with i = T enters the view
  already frontal, or flips within a render, and slides to the centre at
  rate k.
- The single step (v2, d <= 1) shows a smooth 0.3 s turn. A model with yaw
  = slot_yaw(i - T) plus a fast yaw spring is consistent with both, if the
  spring rate is set by the step: the v2 single step needs yaw to follow
  within about 0.1 s. This is not fitted yet.
- Candidate for the next fit: X(i), z(i) from slot(i - p); yaw(i) from a
  spring toward slot_yaw(i - T); p from the v2 sum law. One set of
  constants. It predicts slats at speed (all i < T), the stop seen here,
  and fans when d is 1-3.

**Not done (v4).**
- p(t) through the fastest passages needs per-slat identity matching
  (colour per slat, order kept) instead of strip translation.
- T(t) as a count: counting caption changes, or a fixed degrees-per-item
  with acceleration. The finger gives 2.5-20 items per revolution in the
  windows that matched (median about 10), which suggests wheel
  acceleration but does not measure it.
- The unified fit and the score table against all stills (v1 and v4) and
  the v2 single step.
- Only one stop is measured. The reverse passages (frames 1850-3236)
  contain more slowdowns to check.

### 9.9 (v4) Unified model, scores and equations for AstraPod

**More stop samples (M).** The reverse segment (frames 1850-3236) has
no stop after fast scroll. The finger never rests there: its slowest speed
is 0.18 rev/s at frame 2990. It has slow single steps instead (frames
1850-2160, render gaps of 31-57 frames), and five slowdowns from 1.3-2.5
rev/s to 0.2-0.6 rev/s (frames 2410, 2590, 2850, 3010, 3130). The
slowdowns give the fan look of v1: one near-frontal cover on the
**arriving** side, left of centre in reverse. Frame 2946 has it 0.44 S
from centre, which is u = 0.54 in slot terms. So frame 1430-1610 remains
the only measured stop. Lag at release, flip position and translation
rate come from that one event (9.8).

**Draw order (M).** Stock draws covers by distance from the centre slot,
with the centre cover last. The swap happens once d falls to about 0.5-0.75.
The arriving cover's right neighbour is drawn over it until then.

**The model (F).** The yaw target leads p by the lag, capped at one item.
It is one rule, with one set of constants:

```
d   = T - p                                    (items)
p  += sign(d) * min(|d|, min(k*|d| + v0, vcap) * dt)
Tl  = p + clamp(d, -L, +L)                     (yaw lead point)
for each cover i:
    u      = i - p
    X_i    = s * (xn1 * |u|)                   |u| < 1
           = s * (xn1 + dx * (|u| - 1))        |u| >= 1        (s = sign u)
    yaw_t  = -sign(i - Tl) * ths * min(|i - Tl|, 1)
    yaw_i += (yaw_t - yaw_i) * (1 - exp(-ky * dt))
    z_i    = zn + 0.5 * |sin yaw_i|            (near edge on the centre plane)
T clamped to p +- dmax when input arrives
```

Constants: k 5.7 /s, v0 0.2 items/s (v2); **ky 8 /s** (v2 single step,
best of 4-40); **L 1 item**; ths 90 deg, xn1 0.817 S, dx 0.22 S, zn 1.06 S
(v3 rest fit). vcap 25 items/s and dmax 11 items are **F from one event**.
They do not change the v1 scores (no v1 still exceeds d 3.6), and they
improve the v4 mean by 0.7 px. The stills hardly constrain them. The v4
stop does: 11 +/- 3 items left at release, covered in 0.29 s.

**Scores (F).** The metric is the h-profile RMS over the best 80 % of
columns (px). Trimming drops pale cover tops that the silhouette misses:
the v4 centre cover's white top costs 23 px untrimmed at rest. Each still
takes the best frame within +-0.1 s and the best of four target phases, as
before. The input is 15 deg per item from the finger (v1 thumb, v4
fingertip).

| Model | v1 mean | v4 mean | both | single step (v2) |
|---|---|---|---|---|
| B (all from p) | 13.0 | **8.9** | 11.0 | 0.055 |
| A (all chase slot(i-T)) | **8.8** | 13.4 | 11.1 | 0.067 |
| Y, L unlimited (yaw keyed to T) | 9.3 | 14.4 | 11.9 | 0.080 |
| Y, L 2 | 9.4 | 10.4 | 9.9 | 0.080 |
| **Y, L 1** | 9.8 | 10.6 | 10.2 | 0.080 |
| **Y, L 1, vcap 25, dmax 11** | 9.8 | 9.9 | **9.9** | 0.080 |
| Y, L 1, ky 16 | 9.6 | 9.8 | 9.7 | 0.149 |
| Yz (z from slot), L 2 | 9.9 | 12.0 | 11.0 | 0.056 |

Per still (Y, L 1, vcap 25, dmax 11): v1 f0395 6.2, f0560 8.1, f0700
6.0, f0819 13.3, f0830 13.8, f0901 7.3, f1154 15.6, f1205 7.9; v4 f0300
7.2, f1016 8.8, f1424 9.9, f1488 11.0, f1502 10.6, f1700 9.2, f2700
12.0, f2946 10.3. The detection floor is about 8 px.

Findings:
- No single model wins every column. **Y with L 1-2 is the only one
  within 1.5 px of the best on both videos.**
  - B is best on v4 but worst on v1: it has no fans.
  - A is best on v1 but swings covers through frontal at speed (v4 f1016:
    22 px).
- Keying yaw to T without a cap on the lead (L unlimited) fails at speed
  (v4 f1424: 17.6). With T 10 items ahead, every cover on screen turns to
  the departing side, even on the arriving side. Stock's arriving-side
  covers keep the arriving-side orientation, partly turned.
- z from yaw (Y) against z from slot (Yz): the stills favour z from yaw
  (the v4 stop shows a frontal cover at z 1.07-1.1, 0.61 S off centre).
  The single step favours z from slot (0.056 against 0.080). **Choose z
  from yaw.** It is the geometry of stock's rest layout, it keeps covers
  collision-free, and the single-step gap is 2.4 % of the step's travel.
- Still open: f0819 and f0830 (13-14) and f1154 (15.6). These are the
  steady-fast slats of v1.

**Wheel to T (F, not confirmed in v4).** v1 measured 15-17.5 deg per item
with no acceleration (section 2). In v4 the caption-band changes give
13-17 deg per item in slow passages: 15 changes in 197 deg at frames
1850-2160, and 21 in 350 deg at frames 116-420. The band also changes on
renders and reflections, so this is only consistent with v1, not a
measurement. At 15 deg per item, the forward pass (3160 deg of finger)
feeds 210 items. The display travelled at least 112 items (strip matching
undercounts above 3 items per render). Either part of the input is
dropped (dmax) or the display count is low. **Not confirmed.** Counting
captions in the fastest passage needs per-slat identity matching, which
was skipped. So vcap is a lower bound (15-30 items/s measured) and dmax
has +/- 3 items of uncertainty.

**AstraPod per refresh (I, ready to implement).** The world unit is the
cover width, 128 px.

- Camera: one pinhole for the whole scene, no per-slide projection.
  - F = CAM_DIST = 137 px.
  - Screen x = 160 + F * X / z, half height = 64 * F / z, about the
    horizon row.
  - A cover at z 137 shows 128 px wide. The horizontal FOV is about 100
    deg.
- Layout:
  - first side slot: outer (near) edge at X = 104.6, i.e. 0.817 * 128.
  - stack spacing 28.2 (0.22 * 128).
  - side yaw 90 deg.
  - near edges on z = 135.7 (1.06 * 128, the centre plane).
  - frontal centre at z 137.
- State: T (int), p (Q16.16 items), and yaw per visible cover (Q16.16
  deg; |i - p| <= 8).
- Wheel: 4 raw ticks (15 deg) = T +- 1. Clamp T to p +- 11.
- Each refresh, with dt the real time since the last one (about 15 ms at
  66.9 Hz):
  1. `d = T - p`; `v = min(5.7*|d| + 0.2, 25)`;
     `p += sign(d) * min(|d|, v*dt)`.
  2. `Tl = p + clamp(d, -1, 1)`.
  3. For each visible cover: `yaw_t = -sign(i - Tl) * 90 * min(|i - Tl|, 1)`
     and `yaw += (yaw_t - yaw) * a`, where `a = 1 - exp(-8*dt)`. In fixed
     point use `a = 8*dt - 32*dt*dt` (error under 0.3 % at dt <= 30 ms).
  4. `u = i - p`. X is `104.6*|u|` inside |u| < 1, otherwise
     `104.6 + 28.2*(|u| - 1)`, with the sign of u.
     `z = 135.7 + 64*|sin yaw|` (centre of the cover). The cover's edges
     sit at `X -+ 64*cos yaw`, `z -+ 64*sin yaw`.
  5. Draw order: by `|i - round(p)|`, largest first, centre last.
- Expected look:
  - single step: the centre cover turns over about 0.3 s while p moves.
  - slowing down: one near-frontal cover on the arriving side, 0.5-1 slot
    out (fans).
  - steady fast: all covers are slats. The arriving side is turned
    partway, with no frontal cover.
  - stop after fast scroll: 11 items of catch-up in about 0.3 s; the target
    cover arrives already frontal and slides in at 5.7-7 /s.

### 9.10 (v4) Turn speed, interpenetration and depth

The implementation of 9.9 (AstraPod 7904142300) reported three problems:
the covers next to the centre are near face-on at 20+ clicks/s; neighbours
interpenetrate in fast fans; and the centre cover never recedes. This
section tests rules for all three against the same stills and single
steps.

**Variants tested (F).** All keep 9.9 and add one rule.
- **ce:** the yaw rate grows with the speed beyond what a single step
  reaches: `ky + ce * max(0, |v| - (k + v0))`.
- **kde:** the yaw also advances by at least `kde * 90 deg` per item of that
  excess travel.
- **YT:** yaw keyed to T, with no lead cap (L unlimited).
- **clear:** a cover is turned at least `acos(2 * gap)`, so it clears a
  neighbour at the stack step.
- **push:** a geometric rule with no constant. Covers are placed from the
  most turned to the least turned. Each less-turned cover moves back in
  depth (never forward) until its top-view segment clears every placed
  cover within two slots.

Neither speed term affects the single step, because a single step never
exceeds k + v0. The scores use the 9.9 metric. "Single step" uses a new
harness (`ystep.py`): there 9.9's Y scores 0.097, against 0.080 in the
9.9 harness, because the harnesses take the step's first frame from
different refreshes. "Crossing frames" count refreshes where any two
covers within 6 slots intersect in 3D. The test is constant clicks for 3
s, then a stop, at 66.9 Hz (302 refreshes per rate).

| Rule | v1 | v4 | both | single step | crossing frames 3 / 10 / 20 / 48 clicks/s | centre yaw at 20/s |
|---|---|---|---|---|---|---|
| 9.9 (L 1) | 9.79 | 9.87 | 9.83 | 0.097 | 0 / 56 / 39 / 87 | 27 |
| **L 1 + push** | 9.88 | 9.97 | **9.93** | 0.097 | **0 / 0 / 0 / 0** | 27 (pushed back) |
| L 2 + push | 10.94 | **9.15** | 10.05 | 0.097 | 0 / 0 / 0 / 0 | - |
| L 1 + ce 1 + push | 10.64 | 9.72 | 10.18 | 0.097 | 0 / 0 / 0 / 0 | 28 |
| L 1 + ce 2 + push | 11.73 | 12.67 | 12.20 | 0.097 | 0 / 0 / 0 / 0 | 49 |
| L 1 + kde 1 + push | 9.84 | 12.10 | 10.97 | 0.097 | 0 / 0 / 0 / 0 | 39 |
| L 1 + kde 2 + push | 14.08 | 21.62 | 17.85 | 0.097 | 0 / 0 / 0 / 0 | 79 |
| YT + kde 8 (+ push) | 19.75 | 28.31 (27.90) | 24.0 | 0.097 | 9 / 36 / 3 / 1 (push: 0 / 0 / 0 / 0) | 90 |
| YT + clear | - | - | - | 0.097 | 0 / 0 / 2 / 177 | 15 |
| YT + kd 2-12 (distance rule on all travel) | - | - | - | 0.198-0.275 | - | - |

Findings:
- **A faster turn at speed is not supported by the stills (F).** Every
  speed term makes the stills worse, by 0.8 px (ce 1) to 14 px (YT kde 8).
  - All-edge-on rows (YT kde 8, centre yaw 90 at 20 clicks/s) score 39 px
    on v4 f1424 and 26-29 px on v1 f0560 / f0700.
  - The measured fast frames do not show every cover edge-on:
    - v1 f0560 and f1154 are shingles at 20-40 deg, f0446 is at about
      60 deg (9.4).
    - In v4 f1424 the arriving side is turned partway (top-view segments
      slope by 30-60 deg).
    - Only the reversal still v1 f0819 is all 75-90 deg.
  - A distance rule applied to all travel (kd) breaks the single step
    (0.20-0.28 against 0.097).
- **Push removes every crossing (F).** Its stills score is within 0.1 px of
  9.9, and the single step is unchanged. The yaw-only rule (clear) leaves
  crossings at 48 clicks/s and keeps the near-frontal cover.
- **Push also gives the recede (F, M).** The cover nearest the lead point
  stays the least turned. When its turned neighbours reach into its span,
  it is pushed behind them.
  - Model at 3-10 clicks/s: that cover sits at z 1.2-1.4 (up to 2.2 while a
    neighbour slat overlaps it). It returns to z 1.06 within 0.08 s of the
    neighbours turning away.
  - Measured: v1 fans f0395 / f0901 have a frontal cover at z 1.55-1.6
    (9.4). The v4 fan at frames 2938-2954 has a near-frontal cover at z
    1.30-1.43 (h 94-104 px). Rest is at z 1.04-1.07. **M**
  - So no rule tied to speed or to d is needed. Depth follows from turn
    plus no interpenetration. At 20+ clicks/s, push puts the near-frontal
    cover at u ~1 behind the slats, and the slats largely hide it. This
    answers "the centre is near face-on" without a faster turn. (I: not
    checked against rendered stock frames.)
- **Push does not match the v4 stop (M against F).**
  - Stock at frame 1494 has the arriving cover frontal at z 1.1 while its
    right neighbour's slat, drawn in front of it, crosses its span. **Stock
    interpenetrates there for about one render, and the draw order hides
    it.**
  - The model pushes that cover back to z 2.2-2.4 until d < 0.5. It also
    turns the cover later: the model reaches X 0.6 with yaw -17 deg (L 2)
    or -36 deg (L 1); stock shows 0 deg at X 0.61.
  - With vcap 40 the model arrives 16 frames after stock (vcap 25: about
    50 frames later). Stock's post-release catch-up averages about 35
    items/s.
  - This is one event. It points to L 2 and vcap 40, which gives v4 9.15
    but v1 10.94.

**Winning equations (F).** These are 9.9's, with one geometric step added
after the yaw update. The world unit is 128 px (S).

```
d   = T - p
v   = min(5.7*|d| + 0.2, vcap)                       vcap 25 (40 fits the v4 stop better)
p  += sign(d) * min(|d|, v*dt)
Tl  = p + clamp(d, -L, +L)                           L 1 (L 2 fits the v4 stop better)
for each visible cover i (|i - p| <= 7):
    yaw_t  = -sign(i - Tl) * 90 * min(|i - Tl|, 1)
    yaw_i += (yaw_t - yaw_i) * (8*dt - 32*dt*dt)
    X_i    = sign(u) * (104.6*|u|)                   |u| < 1, u = i - p
           = sign(u) * (104.6 + 28.2*(|u| - 1))     |u| >= 1
    z_i    = 135.7 + 64*|sin yaw_i|                  centre; edges at X -+ 64 cos, z -+ 64 sin
push:  sort visible covers by |yaw| descending; for each cover c in that order:
         dz = 0
         while dz < 205 and segment(c, z_c + dz) properly crosses
               segment(n) for any already-placed n with |n - c| <= 2:
             dz += 2.56                              (0.02 S per step)
         z_c += dz
T clamped to p +- 11 when input arrives; draw by |i - round(p)|, centre last
```

A crossing is a proper intersection of the two top-view segments.
Touching at an endpoint does not count: a slat's near edge resting on a
frontal face is allowed, and the stacks themselves rely on this. The loop
costs at most 80 steps x 4 tests per cover, and is empty for most covers
in most refreshes.

### 9.11 Turn

| Symbol | Value | Meaning |
|---|---:|---|
| J, k | .04889487371, 5.741560141/s | interruption impulse area and free relaxation rate |
| H, W, τ | .04500622572 s, .040 s, .1338588077 s | recovery onset, ramp width, impulse decay time |
| R, m | 3.625962361 items, .9413078492 | turning field half-width and gain |
| c, r | 4.841117576/s, 1.051554892 items | local hinge relaxation and radius |
| Jb, τb | .04572036368, .06917145764 s | bank impulse area and decay time |
| ΔH | .005194731245 s | extra recovery delay per additional accepted item, up to four |
| e, L | .031496, .4 item | finite endpoint offset and contact allowance coefficient |

At the default settings, time is measured in physical seconds. Split the interval at each accepted selection and integrate at most 1/60 s at a time. The carrier, hinge and depth states stay continuous across changes of target.

For unit hinge `g` and depth state `b`, yaw is −πg/2, X = 27.776(i−p) + 73.344g and Z = 156 + 59.264b. The square has side 128, its center Y is −14.976, and the focal length is 156 px about (160,120). Evaluate the 31 albums centered on round(p), clipped to the library. Resolve XZ contacts in descending absolute yaw, breaking ties by increasing index. Limit the lift by the speed-dependent allowance before projection. Composite the nearest positive depth at each column.

### 9.12 Stock algorithm

The 3D carousel uses the slot, timing and projection constants in
`apps-ipod/screens/covers/stock_motion.h`. Sections 9.9–9.11 describe
earlier fitted models. The [firmware report](coverflow-stock-firmware.md)
records the evidence for each constant and the limits of the emulator replay.

| Quantity | Stock value |
|---|---|
| Viewport origin and focal length | (160, 120) px and 160 px |
| Eye and projection | Eye at z = 160, scale = 160 / (160 − z) |
| Cover | 70 × 70 world units, centred at y = −9 |
| Selected slot | x = 0, z = 72.5, yaw = 0°, projected width = 128 px |
| Side slot k ≥ 1 | x = ±(55 + 15(k−1)), z = 40, yaw = ±90° |
| Visible band | Current centre satisfies abs(x) < 100 |
| Retarget | Sample the current pose at input time, then interpolate to the new slot over 1000 ms |
| Position and yaw | Both use 1 − (1 − t)⁵, with t clamped to [0, 1] |
| Wheel | Four of 96 positions per revolution advance one album. Keep the signed remainder across reversals. Clamp selection at the ends. |

A new outer cover inherits its neighbour's start pose and timer, with x
offset by 15 units outwards. The precise stock insertion trigger is
inferred. Side covers start 100 units outside their slots. On return from
the stock Now Playing screen the walls start 100 ms apart. The selected cover starts at
(−43, −10, 72.5), yaw −9°, holds for 250 ms and then returns to the
centre in 1000 ms. Firmware report §6 confirms this return pose.

On menu entry, AstraPod places the selected album at the front of the
right stack, starting at (155, −9, 40), yaw +90°. Its right neighbours
start one slot farther out. All visible covers start off-screen. The
menu uses 3/8 of the authored timings: walls start 37.5 ms apart and
travel for 375 ms. At 150 ms the right stack starts a one-slot scroll
that turns the selected album forward. It reaches the centre at 525 ms.
This menu choreography is an AstraPod choice.

The inferred menu leave path uses a 500 ms quintic ease-in, backdated
by 250 + 100 × abs(k) ms. Side covers move 100 units outwards. The
centre moves to (170, −9, 40), yaw +90°. AstraPod omits the mirrored
reflection and keeps its themed caption and album panel.

On the Classic, the button driver delivers groups of four positions as
detents in bits 24–30 of the scroll event. The carousel applies the full
detent count once and ignores velocity. The driver discards the remainder
when it posts an event, so the carousel cannot reproduce stock remainder
carry or its initial gesture dead band from that event. Remote and simulator
actions without a physical count advance one album.

The renderer composites covers from the outermost inwards, with the
highest-z cover last. Each column retains exposed rows of earlier covers.
Moving covers use nearest
sampling. At eased progress ≥ 0.995 they switch to linear filtering. The
headless host replay in `apps-ipod/screens/covers/tests/stock_replay.py`
compares the projected edges with the reference model and with tracked
markers in the stock emulator frames. The replay uses the stock driver's
input filter and a 34 ms delivery offset for those captures. That offset
does not enter the player code.

On menu entry the parent page, caption and status area cross-fade over
187.5 ms. The reverse fade lasts 250 ms. Cover silhouettes stay opaque.

Album and playback transitions use the same camera. The selected cover
turns 180° to the panel in 500 ms and returns in 1000 ms. Its back face
uses a snapshot of the themed panel. The live panel takes over at rest.
Transitions from the panel or the cover to Now Playing use a 500 ms
quadratic ease-out. The playback theme's artwork slot determines the face-on
endpoint, even when the track has no artwork or decoding is still in progress.
The inverse projection keeps the longer side at 70 world units. Themes without
an artwork slot use the stock pose. If the live layout differs from the
configured slot after the turn begins, the destination moves smoothly to the
live slot over the remaining time, with a minimum of 150 ms. With UI performance
logging enabled, each C/D choice records the rectangle, reason and bitmap state
in `/.rockbox/ui-perf.txt` under `cover-wps`.

Playback controls slide in from the left during the 500 ms turn. Their
leading edge reaches the cover rectangle as the cover lands. Over the next
180 ms the native controls move to their normal positions over the stationary
art. Both stages use quadratic ease-out. Return plays the control motion in
reverse, clearing the artwork before its turn begins. The backdrop travels
under the cover. The stationary slide starts after the flat landing frame is
submitted. It advances by elapsed time, capped at 30 ms per submitted frame
so a late LCD transfer cannot skip the slide. The playback page retains the
landed cover pixels beneath the controls until it paints the selected track's
own artwork, the track changes, or the page closes. The selected album supplies
the cached blur before playback decoding. A late bitmap updates only the cover
during the slide. A cache miss uses the theme background until
playback can reveal its blur underneath the controls. Each transition frame
updates the whole LCD.

MENU after playing a panel track retraces the 500 ms turn and wall motion,
then restores the same panel and selected row. Its next MENU returns to the
carousel. A direct return from playback holds the playback pose for 250 ms,
then moves to the centre over 1000 ms. These transitions use no fade.

Scrolling transfers the smallest contiguous row band that contains changed
cover or background pixels. Caption transfers use the remaining bandwidth.
Each scroll still submits one full-screen refresh when the motion settles.
