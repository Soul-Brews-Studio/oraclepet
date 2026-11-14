# oracle-face — Cat Lab Brewing JC3248W535EN

> ψ esp32-oracle's first body with a screen. ESPHome firmware for the Cat Lab Brewing
> JC3248W535EN dev board (ESP32-S3, 3.5" 320×480 QSPI display, AXS15231 driver IC).

## Hardware

| Item      | Value                                       |
|-----------|---------------------------------------------|
| MCU       | ESP32-S3-WROOM-1 (16 MB flash, 8 MB PSRAM)  |
| Display   | AXS15231, 320×480 RGB565, QSPI              |
| Touch     | AXS15231 (I²C — **deferred to v0.2**)       |
| Backlight | GPIO 1, LEDC PWM 5 kHz                      |

QSPI pin map (from vendor `esp_bsp.h`):

| Signal | GPIO | Signal | GPIO |
|--------|------|--------|------|
| CS     | 45   | DATA0  | 21   |
| PCLK   | 47   | DATA1  | 48   |
| DC*    | 8    | DATA2  | 40   |
| TE     | 38   | DATA3  | 39   |
| BL     | 1    | RST    | NC   |

\* DC is **shared with Touch SCL** in vendor wiring — intentional, since QSPI does
not use a DC line. v0.2 will wire touch on `{SDA=GPIO 4, SCL=GPIO 8}`.

## Quick start

Always uses `uvx` — no global ESPHome install needed.

```bash
make secrets         # creates secrets.yaml from example
$EDITOR secrets.yaml # fill in wifi + ota
make compile         # uvx --from esphome esphome compile jc3248w535.yaml
make run             # compile + upload + tail logs
```

To generate a fresh API encryption key:

```bash
make rand
```

## Notes

- ESPHome ≥ 2025.11 ships mainline support for `mipi_spi` model `AXS15231` and
  the `axs15231` touchscreen — no `external_components` required.
- Framework is `esp-idf` (not Arduino) for cleaner QSPI / LVGL story.
- PSRAM is `octal @ 80 MHz` — typical for ESP32-S3-WROOM-1.
- The AP fallback SSID is `Oracle-Face Fallback` if Wi-Fi creds fail.

## Fonts

The UI prints `ψ` (U+03C8, Greek small letter psi). LVGL's built-in Montserrat
doesn't include Greek, so both sim and firmware use a custom **Noto Sans**
subset (ASCII + ψ) at sizes 14/22/28.

- **TTF source**: `sim/fonts/NotoSans-Regular.ttf` (committed, 806 KB — OFL licensed; shared by sim + YAML)
- **Sim** uses `sim/fonts_gen/noto_*.c` — generated C tables, 4 bpp, compressed
- **Firmware** uses ESPHome's `font:` component which subsets the same TTF at compile time

Regenerate sim font tables (e.g. after expanding the glyph set):

```bash
make sim-fonts   # runs npx lv_font_conv@latest
```

## Roadmap

- **v0.1** *(this)* — display + LVGL heartbeat label + Wi-Fi/API + AP fallback
- **v0.2** — capacitive touch (AXS15231 I²C, SDA=4 / SCL=8)
- **v0.3** — Home Assistant entities (button/light surfaced as widgets)
- **v0.4** — onboard "oracle says" portable-text renderer fed from MQTT
