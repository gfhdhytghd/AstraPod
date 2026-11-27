# Control Centre — Full-Screen Page Design

Redesign of the long-press MENU quick screen (`style.quick_screen=page`, aliasing `hud`) for **Classic**, **Fruit Company**, and **Fruit Company Ultra** on the 320×240 iPod Classic LCD. **Scrim** and **Light** keep their existing tiles layout.

## Layout and Typography

- **Status bar (`y = 0..19`, 20 px):** Standard stock white-chrome status bar (`StatusBarWhite_Background`, `13-helvetica-bold` embossed `"Quick Settings"` title, playback icon, battery).
- **Body (`y = 20..239`, 320×220 px):** Full-bleed edge-to-edge stack led by **Volume** as the primary hero control, followed by the configured quick-screen settings deduplicated in slot order (`TOP`, `LEFT`, `RIGHT`, `BOTTOM`; any slot already bound to `Volume` merges into Row 0, and identical top/bottom slots such as the default `Brightness` pair merge into one row).
  - **4 rows total** (default: Volume, Brightness, Shuffle, Repeat): Volume `64 px` hero + three `52 px` rows (`64 + 3×52 = 220 px`).
  - **5 rows total** (Volume + four distinct settings): Volume `52 px` hero + four `42 px` rows (`52 + 4×42 = 220 px`).
  - Fewer than 4 rows leave the stock brushed steel-blue backdrop (`STOCK_ART_BACKGROUND_LIGHTBLUE`) beneath the last row.
- **Fonts:** `UI_FONT_TITLE` (`17-helvetica-bold`, 13 px cap height) for labels and `UI_FONT_LIST` (`16-helvetica-bold`, 12 px cap height) for values and switch text. Both faces carry full CJK and fallback coverage.

## Skeuomorphic Surfaces and Controls

- **Active wheel-target row (glossy blue bar):** Full-width stock iPod Classic glossy blue selection gradient (`UI_GRADIENT_SELECTION` / `plastic_selection`) with white embossed text (`#ffffff` over `#003670`). Across all three themes the blue bar always indicates which control the click wheel currently adjusts — **Volume** on open.
- **Inactive rows:** Convex pearl-white vertical sheen (`#fcfdff → #eaeef3`) with a 1 px white top highlight and a 2 px engraved hairline divider (`#cfd5dd` / `#aeb6c0`). Near-black embossed text (`#111418`).
- **Volume hero row:** Large `Volume` label, live dB (or remote %) readout, low/high speaker icons, and a wide glossy Aqua slider with a 3D chrome circular knob (`20 px` in 4-row layout, `18 px` in 5-row). Active blue rows switch the filled track to luminous ice-blue glass over a recessed sapphire trough.
- **Numeric sliders (`Brightness`, etc.):** Label top-left, right-aligned value in `16-helvetica-bold`, and a full-width glossy slider with a 3D chrome knob below.
- **Boolean switches (`Hardware Keyclick`, etc.):** Right-aligned iPhone OS-era glossy `On`/`Off` switch (`90×30 px` or `90×28 px`) with a chrome thumb; focused `On` switches use a recessed sapphire well with a bright outer rim.
- **Choice / Cider steppers (`Shuffle`, `Repeat`, etc.):** Right-aligned glossy brushed stepper pill (`32 px` or `28 px` tall) with `16-helvetica-bold` value text and stacked `▴/▾` arrows; focused rows lift the pill to bright pearl-white with deep blue text.
- **Classic key caps:** On Classic, each setting row below Volume carries a brushed-chrome key cap on the left showing its physical click-wheel key (`↑`, `←`, `→`, `↓`, or `↑↓` when top and bottom share one setting).

## Controls and Transitions

- **Fruit Company & Fruit Company Ultra (`ui_wheel_navigation()`):** Mirrors the stock iPod Classic Now Playing click-wheel convention:
  - **On open, wheel adjusts Volume immediately** (Volume row is focused).
  - **`SELECT` (tap)** steps wheel focus across the page (`Volume → row 1 → … → Volume`).
  - **Wheel** adjusts whichever row is focused (clockwise increments / turns `On` / steps forward; counter-clockwise decrements / turns `Off` / steps back).
  - **Idle auto-return:** After **4 seconds** without wheel or `SELECT` input on another row, focus automatically returns to **Volume** so reaching for the wheel later always adjusts Volume.
  - **`SELECT` (hold)** opens the Shortcuts menu (non-Cider).
  - **`MENU` (tap)** closes the Control Centre.
  - **`LEFT`, `RIGHT`, and `PLAY`** remain playback controls and never alter focus, values, or dismiss the page.
- **Classic:** Keeps its existing key map (`Wheel` = Volume, `MENU`/`LEFT`/`RIGHT`/`PLAY` step their four slots, `SELECT` closes, `SELECT` hold opens Shortcuts).
- **Push / Pop transition:** Full-screen horizontal slide (`320 px`, `UI_MOTION_PUSH`/`UI_MOTION_POP` = 18 frames ≈ 270 ms, `UI_EASE_QUAD` ease-out) composited from the retained caller snapshot and the page's offscreen buffer. Closing restores the caller's exact framebuffer snapshot in place and returns `QUICKSCREEN_RESTORED` with no extra redraw or flash.

## Mockups (320×240)

- [classic.png](classic.png) — Classic with Volume hero and directional key caps (`↑↓`, `←`, `→`)
- [fruit-company-volume-focus.png](fruit-company-volume-focus.png) — Fruit Company / Ultra on open (Volume active on wheel)
- [fruit-company-slider-focus.png](fruit-company-slider-focus.png) — Fruit Company / Ultra after `SELECT` moves wheel focus to Brightness
- [fruit-company-choice-focus.png](fruit-company-choice-focus.png) — Fruit Company / Ultra after `SELECT` moves wheel focus to Shuffle
- [fruit-company-5rows-switch.png](fruit-company-5rows-switch.png) — Fruit Company / Ultra with 5 rows (Volume + 4 distinct settings, switch focused)
