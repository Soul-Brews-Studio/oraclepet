# Waveshare ESP32-S3-Touch-LCD-7 (800×480 RGB)

Third RGB-class board for the fleet. **Same ESP32-S3 chip** as the JC3248W535 desk-pet → the `jc3248-pet` app can be ported by swapping **only the display HAL** (the irreducible hardware seam). Brain (state machine + AnimatedGIF decode + BLE/NUS) is chip-portable.

Peripheral map **learned (understood, not copied)** from `~/Downloads/somtor-meter/firmware` (env `esp32s3-lcd7`, Arduino_GFX).

## Chip / build
- ESP32-S3, **16MB flash + 8MB OPI (octal) PSRAM** (`qio_opi`, `psram_type = opi`)
- Arduino: `moononournation/GFX Library for Arduino` (`Arduino_ESP32RGBPanel` + `Arduino_RGB_Display`)

## Display — RGB parallel (16-bit)
| Signal | Pin(s) |
|--------|--------|
| DE / VSYNC / HSYNC / PCLK | 5 / 3 / 46 / 7 |
| Red (R0–R4)   | 1, 2, 42, 41, 40 |
| Green (G0–G5) | 39, 0, 45, 48, 47, 21 |
| Blue (B0–B4)  | 14, 38, 18, 17, 10 |
| Timing | pclk **16MHz**, **falling edge** (active_neg=1); hsync 8/4/8, vsync 8/4/8 (front/pulse/back) |
| Resolution | **800×480** native (somtor rotates to 480×800 portrait) |

> R/G/B pins match ESPHome's built-in `ESP32-S3-TOUCH-LCD-7-800X480` (`mipi_rgb`) model — so ESPHome drives it natively. ESPHome bakes vsync 16/16/4 (vs 8/4/8 here — within panel tolerance).

## I²C bus (shared): SDA=8, SCL=9 @ 400kHz

## CH422G IO expander
- Dual I²C address: **0x24** (mode) + **0x38** (output)
- Drives display enable/reset **and the GT911 reset** via output bits (init: `0x14` → toggle GPIO4 → `0x1E`)
- ESPHome: `ch422g` component; model's `enable_pin = ch422g #2 + #6`

## GT911 touch
- I²C, **auto-detects address** (probe 0x5D then 0x14 via product-ID reg 0x8140); status 0x814E, point0 0x8150
- **No reset pin in the touch driver** — the CH422G init resets the GT911 *first*, then touch just talks I²C.
  This is the architectural fix for the 2.8B GT911 failure: **reset is the expander's job, not the touchscreen component's.**

## Status
- Board not yet plugged in (as of 2026-05-30).
- Two app paths possible: (1) ESPHome `mipi_rgb` LVGL dashboard (native model), (2) **port the Arduino `jc3248-pet` desk-pet** — swap `axs15231_qspi.h` (QSPI push) → an RGB push layer, resize sprite 320×480 → 800×480, keep `pet.cpp`/`ble.cpp`.
