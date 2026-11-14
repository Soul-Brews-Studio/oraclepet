# waveshare-7-pet

The `jc3248-pet` desk-pet, **ported to the Waveshare ESP32-S3-Touch-LCD-7** (800×480 RGB) — same ESP32-S3 chip, new body. A live demonstration of *port at the right seam*: the pet's brain is reused verbatim; only the display HAL changed.

## What was reused vs rewritten

| Layer | File | Change |
|-------|------|--------|
| **Brain** — BLE/NUS heartbeat parse | `src/ble.cpp`, `src/ble.h` | **copied unchanged** |
| **Animator** — state machine + AnimatedGIF + HUD, draws into `spr` | `src/pet.cpp`, `src/pet.h` | **copied unchanged** |
| **Wiring** | `src/main.cpp` | only the `#include` (`axs15231_qspi.h` → `ws_7inch_rgb.h`) |
| **Body** — display HAL (the seam) | `src/ws_7inch_rgb.h` | **NEW** — RGB panel + CH422G + GT911 |

`ws_7inch_rgb.h` exposes the **same `namespace axs` API** (`init / fill_solid / push / read_touch / set_backlight`) so `main.cpp` is otherwise identical to jc3248-pet's.

## How the seam works

`pet.cpp` renders a **320×480** LovyanGFX sprite (unchanged). `ws_7inch_rgb.h::push()` **centers that sprite (letterboxed)** on the 800×480 RGB canvas via Arduino_GFX — so the pet ships with **zero `pet.cpp` changes** (Stage B). Going full-screen 800×480 (bump `PET_W/PET_H` + hi-res GIFs) is a later step.

## Hardware (see `../waveshare-7/HARDWARE.md` for the full map)

- RGB: DE=5 VSYNC=3 HSYNC=46 PCLK=7; R=[1,2,42,41,40] G=[39,0,45,48,47,21] B=[14,38,18,17,10]; pclk 16MHz falling.
- I²C SDA=8 SCL=9; **CH422G** expander (mode 0x24 / out 0x38) drives display enable + GT911 reset; **GT911** touch auto-detects 0x5D/0x14.
- The GT911 reset ordering (CH422G enable → INT(GPIO4) low → reset → release) is encoded exactly in `init()` — this is the load-bearing bit the 2.8B got wrong.

## Build

```bash
cd lab/waveshare-7-pet
pio run -e waveshare-7inch          # compile (espressif32@6.9.0 + Arduino_GFX 1.4.7)
# board not yet plugged in — flash with: pio run -e waveshare-7inch -t upload
```

## Recommended bring-up (two stages — hardware-first)

1. **Stage A — body bring-up:** temporarily replace `setup()/loop()` with `axs::init()` → `axs::fill_solid(RED/GREEN/BLUE)` → a `read_touch()` print loop. Prove the RGB timing + CH422G/GT911 ordering on real hardware *before* the pet.
2. **Stage B — drop in the soul:** the committed `main.cpp` — copies `pet.cpp`/`ble.cpp` unchanged, pet centered/letterboxed. Confirm BLE heartbeat → state → animation end-to-end.

## ⚠️ Verify on hardware (board not yet connected)

- **Byte order** — LGFX sprite buffer vs Arduino_GFX native RGB565. If red/blue swap, set `spr.setSwapBytes()` in `pet.cpp` or swap in `push()`.
- **Backlight** — driven by CH422G enable bit (`0x1E`), not a PWM pin; `set_backlight()` is on/off only. Confirm no dedicated BL GPIO.
- **RGB refresh drift at 800×480** (same class as the 2.8B color/drift work) — if it tears, drop `prefer_speed` 16→12MHz or accept ~20Hz.
- **Partition/LittleFS** — `default_16MB.csv` for the 16MB flash; a LittleFS data partition + the `/characters/bufo` GIF pack are needed at flash time. Do a full `erase_flash` before the first 16MB flash (8→16MB offsets shift).

## Status
Scaffold complete; **compile-verified** (see commit). Not yet flashed — board not connected.
