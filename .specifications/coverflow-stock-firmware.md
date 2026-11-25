# Stock iPod Classic Cover Flow: exact algorithm

**Status: the stock algorithm is read from Apple's code and checked against
frames of stock firmware running in the emulator.**

- **Decryption is done.** The coordinator decrypted all three bodies on the
  device with a temporary AstraPod debug item ("Decrypt Apple firmware"). The
  DFU route failed: the USB transfers timed out.
- **Static analysis is done for 2.0.5.** The Cover Flow code and constants are
  the same in 1.1.2, 2.0.4 and 2.0.5.
- **Stock 1.1.2 boots in the emulator and shows Cover Flow.** Frames come with
  an exact input log.
- **The model matches the emulator frames.**
  - All seven runs end on the same album as the model.
  - Slow and reversing runs match within 1 to 2 px (median).
  - Fast runs match to a median of 3 to 4 px. The rest is the tracker and up
    to one frame of input delay.
- Section 1 is the algorithm. Section 2 has the evidence.

One correction to the earlier RepriseOS-based model: **rotation does not lead
the slide.** Rotation and position use the same eased progress. The 1.33
rotation scale exists in the code, but it is overwritten with 1.0 before it is
used (section 2.3).

Units: "world units" are Apple's model units, and "px" are LCD pixels on the
320×240 screen. Time is in ms. A wheel "position" is a raw wheel position:
96 positions make one turn.

## 1. The stock algorithm (renderer units, ready to port)

### 1.1 Constants

| Name | Value | Meaning | State |
|---|---|---|---|
| `CAM_Z` | 160 | eye on the z axis, looking toward −z | verified (code + frames) |
| `FOCAL` | 160 px | `scale = 160 / (160 − z)` px per world unit, the same on x and y | verified |
| screen centre | (160, 120) px | full 320×240 viewport. The title bar is drawn over it | verified (frames) |
| `HALF` | 35 | half side of a cover quad (70×70 world units) | verified |
| centre slot | x 0, y −9, z 72.5, yaw 0° | 70 × 160/87.5 = **128.0 px**, centre at y 103.5 px | verified |
| side slot k ≥ 1 (right) | x = 55 + 15(k−1), y −9, z 40, yaw +90° | edge-on wall that faces the centre | verified |
| side slot k ≥ 1 (left) | x = −55 − 15(k−1), y −9, z 40, yaw −90° | mirror image | verified |
| visible band | \|x\| < 100 (current, interpolated x) | slots k = 1..3 on each side, so 7 covers at rest | verified |
| reflection | quad directly below the cover, 35 units high | 128×64 texture, mirrored, with the fade baked in | verified |
| clear colour | opaque white | — | verified |
| `DURATION` | 1000 ms | one timer per cover. Every scroll restarts it | verified |
| `E(t)` | `1 − (1 − t)^5` | ease-out, exponent 5.0 | verified |
| rotation progress | `E(t)`, the same as position | the 1.33 scale is dead code (2.3) | verified (code + frames) |
| `WHEEL_DIV` | 4 raw positions per cover | remainder kept, no acceleration | verified (code + frames) |
| linear filtering | when the timer is done or E ≥ 0.995 | nearest filtering while moving | verified (code) |

Yaw is relative to "facing the camera". Apple stores it as 180° (centre),
270° (right) and 90° (left), and its view matrix flips x. Only the difference
matters, and the frames confirm the sign convention below: at +90° a right-side
cover's outer edge (texture u = 1) is the near edge.

### 1.2 Pose to pixels

```c
/* World units in, LCD pixels out. y grows downwards on the LCD. */
void project(float x, float y, float z, float *sx, float *sy)
{
    float s = 160.0f / (160.0f - z);
    *sx = 160.0f + x * s;
    *sy = 120.0f + y * s;
}

/* Four corners of a cover, u, v in {0, 1}; (u, v) = (0, 0) is the top-left
 * of the artwork. */
void cover_corner(struct pose *p, int u, int v, float *sx, float *sy)
{
    float c = cosf(DEG2RAD(p->yaw)), s = sinf(DEG2RAD(p->yaw));
    float du = (2 * u - 1) * HALF, dv = (2 * v - 1) * HALF;
    project(p->x + du * c, p->y + dv, p->z + du * s, sx, sy);
}

/* Reflection: the same horizontal edges, moved down. The top edge of the
 * reflection is the cover's bottom edge (y + 35). The bottom edge is
 * y + 70, with the reflection texture's v running 0..1 from top to bottom. */
```

Check, centre cover at rest: corners x = ±35 at z = 72.5 give 96.0..224.0 px.
y = −9 ± 35 gives 39.5..167.5 px. The reflection ends at 231.5 px. Right-side
walls: the near edges (z = 75) are at 263.5, 291.8 and 320.0 px. The far edges
(z = 5) are at 216.8, 232.3 and 247.7 px.

Apple draws with a depth buffer. A 2D port must sort: draw from the largest
`|x|` towards the centre, then the cover with the largest z last. At rest,
inner walls cover outer walls, and the centre cover covers the inner edge of
walls 1. This sort is a port rule, not stock code.

### 1.3 Per-cover state and the frame update

```c
struct cover {
    int album;            /* album index */
    int rel;              /* slot: album - selected */
    struct pose start;    /* x, y, z, yaw at the last retarget */
    struct pose target;   /* slot(rel) */
    uint32_t t0;          /* ms, start of the 1000 ms timer */
};

struct pose slot(int rel)
{
    if (rel == 0) return (struct pose){ 0, -9, 72.5f, 0 };
    if (rel > 0)  return (struct pose){ 55 + 15 * (rel - 1), -9, 40, +90 };
    return (struct pose){ -55 + 15 * (rel + 1), -9, 40, -90 };
}

float ease(uint32_t now, uint32_t t0)          /* E(t) */
{
    float t = (now - t0) / 1000.0f;
    if (t >= 1) return 1;
    if (t <= 0) return 0;
    return 1 - powf(1 - t, 5);
}

struct pose current(struct cover *c, uint32_t now)
{
    float e = ease(now, c->t0);
    return lerp(c->start, c->target, e);       /* x, y, z and yaw alike */
}
```

Each frame: compute `now`, take `current()` for every live cover, recycle and
add covers (1.5), draw. Apple's firmware works in 16.16 fixed point for all of
this: `E` with `Fixed16_Power`, and sin/cos from a 5° table with linear
interpolation. Floats are used above only for clarity.

### 1.4 Wheel input and retargeting

```c
static int wheel_rem;                 /* lives as long as the player runs */

void on_wheel(int delta)              /* raw positions, signed, 96 per turn */
{
    int steps;
    wheel_rem += delta;
    steps = wheel_rem / 4;            /* C division: truncates towards 0 */
    wheel_rem -= steps * 4;
    if (steps)
        scroll(steps);
}

void scroll(int steps)
{
    int target = clamp(selected + steps, 0, album_count - 1);
    uint32_t now = ms_now();
    steps = target - selected;
    if (steps == 0)
        return;                       /* at the ends: no bounce, no motion */
    selected = target;
    for (each live cover c) {
        c->start  = current(c, now);  /* retarget from where it is now */
        c->rel   -= steps;
        c->target = slot(c->rel);
        c->t0     = now;              /* every cover, the same start time */
    }
}
```

What this means:

- **No acceleration and no coalescing.** Each wheel event that crosses a
  multiple of 4 positions scrolls at once. If one event carries 8 positions,
  every target jumps 2 slots in one retarget.
- **A new scroll during a transition** restarts every cover's 1000 ms ease
  from its current pose. Position is continuous. Velocity jumps to the start
  speed of the new curve, which is 5 × (remaining distance) / 1 s.
- **Reversal is not special.** The remainder keeps its sign through a
  reversal: +3 followed by −1 leaves +2.
- **The wheel driver has a dead band** (0x08093718). It applies to every
  screen, not only Cover Flow.
  - After a touch, a move smaller than about 6 positions is held. When the
    held total reaches 4, it is dropped and the band is cleared.
  - A move of 36 or more positions in one report is lost.
  - In the emulator, a lone 4-position gesture does nothing, and the first
    4 positions of each gesture are lost.
  - AstraPod has its own wheel driver, so copy this only if the first-move
    feel must match too. State: read from code. The value 6 is the emulator
    author's measurement, and the frames agree with it.
- The 8 ms from the step to `t0` in the frame fit (section 2.6) is the
  emulator's event delivery. It is not a stock delay.

### 1.5 Recycling at the edges

- After each update, a cover is removed when its **current** x is at or beyond
  ±100. Removal keeps at least one cover.
- New covers are added beyond each end, for the next album, while that end has
  albums left and its outermost cover is inside the band. This trigger is
  inferred from a partial read of 0x080ec9ec. The new cover's pose and timer
  below are verified.
- A new cover N next to neighbour B on side d (+1 or −1) gets:
  - `rel = B.rel + d`, and `target = slot(rel)`
  - `t0 = B.t0`, with the same 1000 ms
  - `start = B.start`, with `start.x += 15·d`. y, z and yaw are copied from B.
  - its current x set to B's current x + 15·d
  
  So it slides in exactly one slot outside its neighbour, in step with it.

### 1.6 Entering and leaving (lower priority)

- **Enter** (`CoverFlowCover_InitializeAnimation` 0x08147c44 with the entry
  function at 0x08147bd0):
  - A side cover with slot k starts at its target with x moved 100 units
    outwards. Its timer starts **100 ms × |k|** after entry.
  - The centre cover starts 250 ms after entry, from (−43, −10, 72.5) at
    yaw −9°.
  - All of them use `E(t)` over 1000 ms.
  - State: verified for the stagger, the ±100 offset and 250 ms. The centre
    start pose is read from the initializer and not checked in frames.
- **Leave** (0x08147db0, state 2):
  - Every cover runs a 500 ms **ease-in** (`t^5`), from its current pose.
  - Side covers move ±100 units outwards.
  - The centre cover moves to (170, −9, 40) at yaw +90°.
  - The timer is back-dated by 250 + 100·|k| ms.
  - State: the code is read. That this path is "leave" is inferred from its
    callers, not seen in frames.
- **The album flip** (select on the centre cover, states 6 and 7, 250 ms delay,
  500 ms) is not analysed.

## 2. Evidence

Addresses are 2.0.5 virtual addresses in `cf205/dram.bin`. Load
`dec/osos-38.2.0.5.body` from offset 0xaf98 at 0x08000000. All values are 16.16
fixed point unless noted. The disassembly helper:

```sh
cd /tmp/claude-1000/stockfw/cf205
D(){ arm-none-eabi-objdump -D -b binary -marm --adjust-vma=0x08000000 \
     --start-address=$1 --stop-address=$2 dram.bin | tail -n +8 | cut -f1,3-; }
```

### 2.1 Slot constants: initializer 0x082a2bcc

`0x08147a40(node, value)` sets a shared `AnimationNode`. `0x0824e148`
converts a float to 16.16.

```
82a2bfc ldr r0,=0x8a81e50 ; mov r1,r5(=0)        ; bl 8147a40   V_cpos.x = 0
82a2c08 sub r4,r5,#0x90000                        ;              r4 = -9.0
82a2c0c ldr r0,=0x8a81e68 ; mov r1,r4             ; bl 8147a40   V_cpos.y = -9
82a2c18 ldr r1,=0x42910000 (72.5f) ; bl 824e148 ; ldr r0,=0x8a81e80 ; bl 8147a40   V_cpos.z = 72.5
82a2c40 0x8a81f28 <- 0 ; 82a2c50 mov r1,#0xb40000 -> 0x8a81f40 ; 0x8a81f58 <- 0   V_crot = (0,180,0)
82a2c78 mov r1,#0x370000 -> 0x8a81e98 ; r4 -> 0x8a81eb0 ; #0x280000 -> 0x8a81ec8  V_spos = (55,-9,40)
82a2ca8 0x8a81f70 <- 0 ; ldr r1,=0x10e0000 -> 0x8a81f88 ; 0x8a81fa0 <- 0        V_rrot = (0,270,0)
82a2d5c ldr r0,=0x8a81e20 ; mov r1,#0x50000       ; bl 8147a40   ease exponent = 5.0
82a2d74 ldr r0,=0x8a81e38 ; mov r1,#0x20000       ; bl 8147a40   second exponent = 2.0 (flip)
```

### 2.2 Layout: `0x081483c0` (called by Retarget, Initialize and the leave path)

```
81483cc ldr r1,=0x8a81e98 ; ldr r1,[r1,#4]   r1 = V_spos.x = 55
81483e0 mov r2,#0xf0000                       spacing 15.0
81483e4 bne side                              rel != 0
81483e8 target_pos <- V_cpos ; target_rot <- V_crot
8148400 ldr r1,=0x3faa3d71 (1.33f) ; bl 824e148 ; str r0,[cover+0x26c]  clamp scale (see 2.3)
814841c bge right
8148420 add r0,r0,#1 ; mul r0,r2,r0 ; sub r6,r0,r1     x = 15(rel+1) - 55
8148430 r5 = V_rrot.y(270) - V_crot.y(180) = 90        left yaw (Apple 90 = -90 relative)
814846c sub r0,r0,#1 ; mul r0,r2,r0 ; add r5,r0,r1     x = 15(rel-1) + 55, yaw stays 270
814849c str r7(=0x10000),[r4,#620]                     side clamp scale 1.0
```

### 2.3 Retarget: `CoverFlowCover_Retarget` 0x08148060 (verified)

```
8148090 mov r2,#1000 ; bl 815609c        AnimationTimer_Start(renderer, now, 1000)
81480a0 bl 8147b00                       start <- renderer's current x,y,z,rot (0x08147b00 copies +0x34..+0x48)
81480a8 add r0,r0,r5 ; str [r4,#0x130]   rel += delta
81480b4 bl 81483c0                       target <- layout(rel); writes clamp scale 1.33 or 1.0
8148120 ldr r2,=0x8a81e20 ; bl 815d26c   EaseOut_Bind(+0x214, timer, exponent 5)
8148130 mov r2,#0x10000 ; bl 80f3780     Clamp_Bind(+0x254, ease, 1.0)
8148148..8148160 Lerp_SetProgress(pos x,y,z, +0x214 ease)
814816c..8148184 Lerp_SetProgress(rot x,y,z, +0x254 clamp)
8148188 mov r0,#3 ; strb [r4,#4]         state = 3 (scrolling)
```

`AnimationClamp_Bind` 0x080f3780 stores its third argument at `+0x18`:
`80f37b8 str r7,[r4,#24]`. For the clamp at cover +0x254 that is +0x26c, the
same word that layout wrote. **So after every retarget the rotation scale is
1.0**, and `AnimationClamp_Update` 0x080f37dc (`min(1, max(0, input × scale))`)
passes `E(t)` through unchanged. In the other paths the rotation lerps bind
straight to an ease node, so they never read the clamp:
- `InitializeAnimation`: 0x08147d38..0x08147d9c bind to +0x214
- the leave path: 0x08147ebc..0x08147f20 bind to +0x234

**The 1.33 is never used.** The frames agree: rotation scale 1.0 fits with an
RMS of 0.72 px, and 1.33 gives 6.3 px (2.6).

The state-4 branch (0x081480bc..0x0814811c) normalises the start yaw to 0 or
360. State 4 is set only by the 256 ms jump path (0x080ec9a8). Scrolling never
takes it.

Primitives (verified):

| Function | Address | Formula |
|---|---|---|
| `AnimationTimer_Update` | 0x08155f6c | `t = (now − start) / duration`, saturating at 1 |
| `AnimationEaseOut_Update` | 0x0815d2e4 | `1 − (1 − t)^exp` for 0 < t < 1 |
| `AnimationEaseIn_Update` | 0x0818d14c | `t^exp` (leave path only) |
| `AnimationLerp_Update` | 0x0815c3dc | `start + (end − start) · progress` |
| `AnimationClamp_Update` | 0x080f37dc | `clamp(input × scale, 0, 1)` |

### 2.4 Camera and quad: `ArtworkRenderer_Draw` 0x08271fa0, GL setup 0x08252724

Projection (GL setup, matrix mode `0x1701` = `GL_PROJECTION` from the literal at 0x08252a78):

```
8252818 ldr r2,=0x15555 (4/3) ; mov r1,#0x5a0000 (90.0) ; bl 8273908   perspective(90°, 4/3)
8252840 mov r0,#0xa00000 ; sub r0,r0,#0x1400000 ; str r0,[sp,#92]      m[15] = -160
```

`0x08273908`:
- `m[0] = −cot(45°) = −1`
- `m[5] = m[0] × aspect = −4/3`
- `m[10] = 0` (near −300, far +300)
- `m[11] = +1`
- `m[14]` comes from a fixed-point product that overflows. It only feeds depth.

The caller then sets `m[15] = −160`. Clip space:
- `x_c = −x`
- `y_c = −4/3 · y`
- `w = z − 160`

So `x_ndc = x / (160 − z)` and `y_ndc = 4/3 · y / (160 − z)`. On the 320×240
viewport both give `160 · v / (160 − z)` px. The modelview is the identity
(`CoverFlowView_UpdateActiveState` 0x0813818c: 0x08273d1c loads the identity,
and 0x082739dc applies it with mode `0x1700`).

Quad (`ArtworkRenderer_Draw`):
- Rotations are applied in this order: yaw first (+0x44, via 0x08273b04), then
  pitch (+0x40, 0x082739f8), then roll (+0x48, 0x08273c10).
- The cover's horizontal axis and vertical axis are the rotated matrix's first
  two columns, each × 35.0 (`8272140 mov r1,#0x230000`).
- The corners are C ± X ± Y: 0x0829c764 subtracts and 0x0829c7c0 adds.
- When pitch is 0 (always, in Cover Flow), the reflection is built at
  0x082722a4..0x082722f8:
  - The top edge is the cover's bottom corners with y + 70.
  - The bottom edge is the top corners with y + 35.
  - The z values are swapped between top and bottom, so the quad is a mirror.
- The facing test at 0x08272074 uses the vector `(x, 0, 160 − z)` against the
  cover normal. It chooses the front or the back texture, and it confirms the
  eye at z = 160.

### 2.5 Wheel: `CoverFlowController` 0x08162f10, `WheelAccumulator` 0x081b0d4c

```
8162f3c ldr r3,=0x15e (350) ; mov r2,#1 ; mov r1,#4 ; bl 81b0efc   Construct(acc, threshold 4, accel on, 350 ms), once
8162f68 bl 8289490                        decode the wheel event's motion
8162f70 ldr r1,[r4,#12] ; mov r2,#0 ; bl 81b0e4c   ConsumeMotion(acc, motion, speed 0)
8162f88 blne 81628a4                      if steps != 0: CoverFlowView scroll(steps)
81628ac bl 8138050                        view+0xd8 = (steps >= 2); now; ... ; CoverFlowList_Scroll(list, now, steps)
```

In `AccumulateMotion` 0x081b0d4c, the multiplier would double every 350 ms of
steady turning. But a multiplier reset runs on every event whenever speed is 0:
0x081b0db0 calls 0x081b0ebc, which returns 1 for speed 0, and 0x081b0dc4 then
sets the multiplier to 1. Cover Flow passes speed 0. **So there is no
acceleration.** Then:

```
81b0e04 ldr r0,[r4,#20] ; ldr r7,[r4,#24] ; add r6,r0,r5   rem += motion
81b0e18 bl 8026690 (sdiv) ; mul ; sub                       steps = rem / 4; rem -= steps * 4
```

`CoverFlowList_Scroll` 0x080ec8ec clamps `steps` to the album range, returns
when it is 0, and calls `Retarget(cover, now, −steps)` for every cover in the
active chain (`80ec944`). The low-level wheel driver (0x08093718) counts
96 positions per turn, with a wrap at ±72.

### 2.6 Emulator frames (stock 1.1.2, `emu/r_single`)

Input (`emu/r_single/input.csv`): five single moves of 6 positions at 40,
2540, 5040, 7540 and 10040 ms: + + + − −.

- **Quantisation.** The selection goes 1 → 2 → 4 → 5 → 4 → 3. That is
  `rem / 4` with the remainder kept: 6 → 1 r2, 8 → 2 r0, 6 → 1 r2, −4 → −1 r0,
  −6 → −1 r−2. A divisor of 5, 6 or 3 does not give this sequence. Verified.
- **Rest geometry** (`f000137`, measured from the corner markers, which are
  inset 2 px):
  - centre cover 96..224 × 40..167 px; the model gives 96..224 × 39.5..167.5
  - right walls' near edges at 263, 291 and 319 px; the model gives 263.5,
    291.8 and 320.0
- **Motion.** The first step was fitted with the right edge of album 2 (moving
  to the centre) and the left edge of album 1 (moving to the left).
  `t0` was a free parameter.

  | Rotation progress | RMS (px) | t0 |
  |---|---|---|
  | **E(t)** | **0.72** | 48 ms |
  | E²(t) | 5.87 | 44 ms |
  | min(1, 1.33·E) | 6.27 | 56 ms |
  | 1 − (1 − t)² | 9.96 | 52 ms |
  | t (linear) | 16.0 | 92 ms |

  So position and rotation both use the exponent-5 ease over 1000 ms, and the
  animation starts 8 ms after the injected step. In the two-slot step at
  2540 ms, the incoming cover's outer edge follows the model within 1 to 3 px.
  The tracker's misreads (hue confusion when covers are recycled) were left
  out by hand.
- **Whole run.** `tools/stock_model.py emu/r_single` replays `input.csv`
  through section 1 and compares every tracked outer cover edge in every
  frame, over all five steps: 796 edges, median error 0.5 px, 90th percentile
  2.1 px, 95th percentile 4.6 px. The tail is the tracker's hue confusion.
- **All seven scenarios** (`emu/runs/*`, each on a fresh boot, starting at
  Album 01, 4 positions per step):
  - `tools/stock_model.py` replays each `input.csv` through section 1, with
    the dead band.
  - It checks every tracked outer edge.

  | Run | Input | Final album (model = frames) | Median error | 90th percentile |
  |---|---|---|---|---|
  | single | 5 × 6 positions, 2.5 s apart | 3 = 3 | 0.5 px | 2.3 px |
  | slow250 | 12 steps, 250 ms apart | 12 = 12 | 0.6 px | 2.5 px |
  | rev120 | steps 120 ms apart, reversing | 1 = 1 | 1.1 px | 5.4 px |
  | ramp37 | steps 250 → 37 ms apart | 24 = 24 | 1.7 px | 35 px |
  | fast22 | 22 steps, 37 ms apart | 22 = 22 | 3.1 px | 71 px |
  | fast36 | 36 steps, 37 ms apart, hits the end | 24 = 24 | 3.6 px | 71 px |
  | rev37 | steps 37 ms apart, reversing | 1 = 1 | 3.7 px | 46 px |

  (Delay 34 ms for all rows. With 8 ms, `single` is unchanged and the fast
  runs are worse.)

  - The final album matches in all seven runs. That checks the divisor, the
    remainder, the dead band and the end clamp.
  - In the slow and reversing runs the motion matches within 1 to 2 px.
  - In the fast runs the median is 3 to 4 px. The long tail comes from the
    tracker:
    - It names a cover by hue, and narrow side covers in fast runs are
      misnamed. The sub-agent reported this separately.
    - A clean check: album 1's left edge is never covered. It stays within
      2.7 px (fast22) and 5.4 px (fast36) of the model. Its residual changes
      sign between runs, which fits a delay that varies by up to one frame.
  - The delay from an injected step to `Scroll` is 8 to 34 ms in the
    emulator. That is up to one frame, because the UI thread reads wheel
    events between frames. On the real device this delay is unknown.
- **Settle.** The last frame change comes 943 ms after the last input. The
  emulator renders a frame every 34 ms (the median).

Visual check: the seq.png contact sheet from the capture run
holds frames 0–10 and 27 of the first step.

### 2.7 Same code in 1.1.2, 2.0.4 and 2.0.5

A masked byte-pattern search (branch offsets masked) finds these functions in
`dec/osos-35.2.0.4.body` and `dec/osos-24.1.1.2.body`. The instructions are
identical:
- Retarget
- layout
- `ArtworkRenderer_Draw`
- the perspective builder
- the slot initializer

The only differences are literal-pool data addresses. In 0x600 bytes of the
initializer there are 47 such differences, all of them pointers. Every float and
immediate constant above is the same in all three versions.

## 3. What is left and how sure each part is

| Item | State |
|---|---|
| Slot layout, camera, quad size, reflection, ease, timer, retarget, wheel divisor | verified (code + frames) |
| Rotation scale 1.0 (the 1.33 is dead) | verified (code + frames) |
| Recycling: the ±100 removal test and the new cover's pose and timer | verified in code. Not checked against frames |
| Recycling: when exactly a new cover is added (0x080ec9ec) | inferred from a partial read |
| Enter animation stagger and offsets | verified in code. The centre start pose was not checked in frames |
| Leave animation | code read. That it is "leave" is inferred |
| Album flip (states 6, 7) | not analysed |
| Edge smoothing (`CoverFlowEdges_Smooth*`) | not analysed. It is a pixel touch-up on settled covers |
| `scripts/all.txt` as one run | skipped: about 100 steps run past the 24 albums. It was split into the seven scenarios, each on its own boot |
| Emulator timing equal to the device | not confirmed. The runs did not use `-icount`. To check: film the device with the same gestures |
| Frame pairs only 2 ms apart (7 pairs in `fast36`, each with more than 30,000 changed pixels) | not explained. To check: trace the display composition at those times with `DISPTRACE=1` |
| Real-device frame rate | unknown. The emulator renders every 34 ms. The animation is time-based, so this affects smoothness only |

## 4. Emulator setup and reproduction

All paths are under `/tmp/claude-1000/stockfw/`.

```sh
cd /tmp/claude-1000/stockfw
git clone https://github.com/davidmonterocrespo24/qemu-ipod-classic
python3 -m venv venv && venv/bin/pip install distlib tomli
cd qemu-ipod-classic && git checkout -b coverflow-capture
git apply ../coverflow-capture.patch        # the capture hook
mkdir -p build && cd build
../configure --python=/tmp/claude-1000/stockfw/venv/bin/python3 \
  --target-list=arm-softmmu --disable-sdl --disable-gtk --disable-cocoa \
  --disable-capstone --disable-slirp --disable-werror --enable-pie \
  --enable-plugins --disable-libnfs --disable-curl --disable-glusterfs \
  --disable-libiscsi --disable-rbd --disable-libssh --disable-spice \
  --disable-vnc --extra-ldflags=-lcrypto
ninja qemu-system-arm

# Apple firmware (Apple's server)
B=http://appldnld.apple.com/iPod/SBML/osx/bundles
curl -O $B/061-4306.20080430.Gtr54/iPod_24.1.1.2.ipsw   # Classic 6G 1.1.2
curl -O $B/061-7299.20091217.Bghyt/iPod_35.2.0.4.ipsw   # Classic 7G 2.0.4 (MC293)
curl -O $B/041-8552.20121203.Bile3/iPod_38.2.0.5.ipsw   # Classic 2.0.5

# all seven capture scenarios, each a fresh headless boot of 1.1.2 that goes
# to Music > Cover Flow and runs one wheel script (about 67 s, in parallel)
bash emu/run_all.sh                               # -> emu/runs/<scenario>/
for r in single slow250 ramp37 fast22 fast36 rev120 rev37; do
    python3 tools/stock_model.py emu/runs/$r 34 1 # model against frames
done

# static analysis
python3 tools/cf_disasm.py dec/osos-38.2.0.5.body cf205 cf_functions_2.0.5.tsv
```

Configure needed four changes from the documented build:

1. Configure stopped with `found no usable distlib`. Fix: a venv with
   `distlib`, passed with `--python`.
2. The build stopped at `block/nfs.c:276: too few arguments to function
   'nfs_pread_async'`. Fix: `--disable-libnfs`.
3. Linking stopped at `undefined reference to symbol
   'AES_set_decrypt_key@@OPENSSL_4.0.0'`. Fix: `--extra-ldflags=-lcrypto`.
4. To run headless with no windows: the `--disable-sdl --disable-gtk
   --disable-vnc` options and `-display none`.

Boot flags: `SHA1_ZERO=1 PMUCHARGER=adapter DISP_ALPHA=1`. Keys on the
monitor: `q` and `e` turn the wheel, Enter is SELECT, `w` is MENU. A SELECT
press on a cold wheel is ignored, so touch the wheel first.

### Capture hook (`coverflow-capture.patch`)

- **`WHEELSCRIPT=file`** replays wheel input from a script.
  - Each line is `t_ms op [arg] [arg2]`, timed from the `g` key (`sendkey g`
    on the monitor).
  - Ops: `touch`, `step ±N`, `release`,
    `btn select|menu|play|left|right down|up`, `mark`, `end`.
  - Each step goes into the wheel FIFO at its virtual time.
  - `WHEELLOG=file.csv` logs each event with virtual ns, ms and position.
- **`LCDCAP=dir`** saves frames.
  - Every `LCDCAP_US` (2000) of virtual time it composes the display and
    writes `fNNNNNN.rgb565` (320×240, little-endian) when the frame changed,
    plus `frames.csv`.
  - `LCDCAP_SECS` sets how long it runs.
- The firmware's only clock is timer E on QEMU's virtual clock, so frame
  times and animation times share one timebase.

### Inputs

- IMG1 cut: the `8702` header is at file offset 0x4e07000 of each `Firmware-*`
  partition. The length is `0x800 + u32@0x10`. `reprise firmware extract`
  handles 2.0.5 only.
- The decrypted bodies are `dec/osos-{24.1.1.2,35.2.0.4,38.2.0.5}.body`. They
  come from the AstraPod debug item, which runs `hwkeyaes(DECRYPT, GKEY)` in
  64 KiB chunks and XORs each chunk's first block with the previous chunk's
  last ciphertext block. The first 43,008 bytes that DFU decrypted for 2.0.5
  match.
- The test disk is `disk/E_cf.img`:
  - 24 albums with numbered covers and coloured corner markers
  - built by libgpod as an MB147
  - `hash58` is zeroed
  - `disk/build_disk.sh` rebuilds it
- Apple's binaries stay in `fw/`, `img1/`, `dec/` and `cf205/`. Do not
  redistribute them.

SHA-256 of the IPSWs and IMG1s: `report.v1.md` section 4.

### DFU route (failed, kept for the record)

`reprise decrypt` (osos-lab) uploads an AES payload in BootROM DFU mode and
exchanges 512-byte chunks.

Errors on this host:
- `USB control write: Operation timed out` at body offsets 0x800, 0x9000,
  0xa800 and 0x14800.
- `apple_mfi_fastcharge` binds to the DFU device and reloads itself. Unbinding
  it led to `USB active configuration: Entity not found` until the device was
  rebound to the generic `usb` driver.

What was tried, with no effect:
- the root port instead of the hub
- a 1000 ms timeout
- 2 ms pacing
- 256-byte chunks

Do not use this route. The on-device debug item replaced it.

## 5. Recommended next step

1. **For AstraPod:**
   - Replace the fitted cascade with section 1 as written: one slot function,
     one 1000 ms quintic ease-out per cover, restarted from the current pose
     for every cover on each scroll, rotation on the same progress, and
     `delta / 4` wheel quantisation with the remainder kept.
   - Convert section 1.2 to fixed point with `scale = 160 / (160 − z)`.
     z stays in [5, 75], so the divisor stays in [85, 155].
   - Draw the outer covers first and the centre cover last.
   - Then render the same input as `emu/r_single/input.csv` and compare with
     the stock frames, with `emu/analyze.py`.
2. **If the flip or the enter and leave transitions matter:**
   - Read the flip path: 0x081484bc, states 6 and 7.
   - Capture Cover Flow entry in the emulator. Start the capture before
     pressing SELECT on "Cover Flow".
