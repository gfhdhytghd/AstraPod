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
