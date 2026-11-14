# JC3248 App Gallery

Numbered, single-purpose apps for the **Guition JC3248W535** (ESP32-S3 + AXS15231
320×480 QSPI). **One app per build**, selected by `-DAPP=N`. Each app adds **one
layer** on top of the proven native driver — flash them in order to **bisect**
which layer breaks rendering in the full buddy.

> Why: the buddy renders white post-init, but a bare "cycle solid colours" loop
> (app 000) works perfectly. So the bug is in the stack *above* the driver. Each
> app isolates one layer; the **first app that shows white = the culprit**.

## Flash one app at a time
```bash
cd lab/jc3248-apps
make list                 # show all apps
make flash N=0            # flash app 000 (sanity: must cycle colours)
make flash N=1            # then 001, 002, ... in order
make monitor              # serial (115200) — some apps log here
# different port: make flash N=5 PORT=/dev/cu.usbmodemXXXX
```
After adding/removing app files: `make reindex` (regenerates `src/apps/index.h`).

## Warm-up: ~10s on app 000, fast (~1.5s) on the rest

The AXS15231 needs a few seconds of non-black frames after display-on before it
will hold content. `axs::init()` provides this:
- **app 000** runs the full ~10s colour cycle (red→…→white) — the baseline /
  cold-boot proof, and a boot animation.
- **apps 001+** run a fast ~1.5s white settle instead (no colour loop), so each
  bisection flash isn't 10s of waiting. Each app then keeps pushing its own
  coloured frames, which sustains the hold.

So: flash an app, let the brief settle finish (screen goes black), then read the
number. On a **cold** power-on of a non-000 app the short settle can be marginal —
if it comes up white, flash **000** once to warm the panel, then reflash your app.

Each app (001+) then renders a **big static number = its own app number** on a
distinct background, so you can tell at a glance *which* app is running and
*whether its layer rendered*. **No number / white / blank after the warm-up = that
layer is the culprit.** (000 is the exception — it's the native colour-cycle
baseline; it has no sprite/font.)

## Apps

| # | Name | Adds this layer | LOOK FOR (after warm-up) |
|---|------|-----------------|----------|
| 000 | colorcycle | native driver only (baseline ✅) | colours cycle forever (red→…→white) |
| 001 | sprite_solid | LovyanGFX `LGFX_Sprite` → `axs::push` | big "**1**" on **green** |
| 002 | sprite_shapes | sprite `fillRect`/`drawRect`/`drawString` | two bordered rects + big "**2**" on black |
| 003 | sprite_fastpush | tight push loop, no delay (push-rate) | big "**3**" on **purple**, rock-steady |
| 004 | sprite_parented | sprite with an (un-init) `LGFX_JC3248` parent | big "**4**" on **cyan** vs **white** |
| 005 | tft_setrotation | `dev.setRotation()` then push | big "**5**" on **orange** vs **white** ← prime suspect |
| 006 | tft_setbrightness | `dev.setBrightness()` then push | big "**6**" on **blue** vs **white** |
| 007 | upscale_3x | 3× nearest-neighbor `fillRect` upscale | blocky checker + big "**7**" |
| 008 | sprite_continuous | tight push loop, no delay | big "**8**" on **magenta**, steady |
| 009 | sprite_doublebuffer | two PSRAM sprites, ping-pong | big "**9**" on **maroon**, steady |
| 020 | touch_raw | `axs::read_touch` (serial) | big "**20**" on **navy**; serial prints coords on touch |
| 021 | touch_draw | draw at touch point | big "**21**" on black; yellow trail follows your finger |

### Animations / demos (040–047)
Full-frame `LGFX_Sprite` animations pushed every loop — the gallery's showcase deck.
(These are demos, not bisection probes — no warm-up confound since they're clearly moving.)

| # | Name | LOOK FOR |
|---|------|----------|
| 040 | bounce | 5 colored balls bouncing with comet trails |
| 041 | starfield | warp-speed stars streaming out from center |
| 042 | plasma | flowing full-screen rainbow plasma field |
| 043 | matrix_rain | green Matrix glyph rain, white heads + fading tails |
| 044 | sine_waves | 3 scrolling cyan/magenta/yellow sine waves |
| 045 | fireworks | rockets launch → burst into fading sparks |
| 046 | rainbow_scroll | rainbow gradient bands scrolling upward |
| 047 | spinner | rotating comet-head dot ring + breathing circle |

*(010–019 GIF / 030–039 M5-HAL slots reserved for a later batch.)*

## How to read the results
- **000 must cycle colours.** If even 000 fails, it's the board/driver (not the gallery).
- Flash upward. After each flash, **wait out the ~10s warm-up**, then look. The
  **lowest-numbered app whose number does NOT appear (white/blank)** names the
  broken layer. Likely: a sprite layer (001–003) or a `tft.*` interaction (004–006).
- `make monitor` is ground truth: each app prints `[appNNN] …` and the dispatcher
  prints `>>> warm-up done — APP NNN rendering now`. If serial says the right app
  but the screen has no number, that app's render layer is the bug.
- Once found, fix the buddy at exactly that point (e.g. don't call the offending
  LovyanGFX op while the native driver owns SPI2).

## Layout
```
platformio.ini          proven JC3248 build (pioarduino 55.3.38, qio_opi PSRAM)
Makefile                the flash selector (make flash N=…)
src/axs15231_qspi.h     the PROVEN native driver (single source of truth)
src/lgfx_jc3248_device.h LovyanGFX device (for the tft.* probe apps 004–006)
src/app_api.h           the app contract every app implements
src/main.cpp            dispatcher: Serial + axs::init() + app_setup/app_loop
src/apps/app_NNN_*.h    one app each (#if APP==N)
```
