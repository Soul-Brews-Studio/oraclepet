# WT32-SC01 Plus — ESPHome baseline

Second display board in the fleet. **Not** the JC3248W535 — different chip
config, different display bus, different touch controller. Identified by
esp32-exp on 2026-05-29 (`make chipid` read 2 MB PSRAM, which is the tell).

| | JC3248W535 (`lab/jc3248w535`) | **WT32-SC01 Plus** (this) |
|---|---|---|
| Module | N16R8 (16 MB / **8 MB octal** PSRAM) | N16R2 (16 MB / **2 MB quad** PSRAM) |
| Display | AXS15231B 320×480 **QSPI** | ST7796UI 320×480 **8-bit 8080 parallel** |
| Touch | AXS15231B I²C `0x3B` | FT6336U I²C `0x38` |
| Port (this host) | `/dev/cu.usbmodem13301` | `/dev/cu.usbmodem1201` |
| MAC | `98:a3:16:e3:ec:c8` | `68:b6:b3:36:b0:18` |

IDs: Panlee/Wireless-Tag **WT32-SC01 Plus** = model `ZX3D50CE08S-USRC-4832`,
module `WT32-S3-WROVER-N16R2`.

## The display DOES work in pure ESPHome (released 2026.5.1)

The ST7796 here is an **8-bit 8080 parallel** panel. The path is *not* obvious:

- ESPHome's dedicated **i80** component was **reverted/abandoned** upstream
  (PRs #8227–8230, reverted by #8384; the #8228/#8229/#8230 follow-ups closed
  unmerged). So there is no `i80:` block and no `ili9xxx: bus_type: i80` — not
  in any release, not on dev. (`esphome config` → `Component not found: i80`.)
- **But the capability shipped differently.** An 8-bit 8080 panel is, at the
  pin level, an **8-lane octal SPI bus** (the WR strobe is the clock). ESPHome's
  `mipi_spi` platform carries a **built-in `WT32-SC01-PLUS` model**
  (`esphome/components/mipi_spi/models/ili.py`) that bakes in
  `bus_mode=octal, dc_pin=0, reset_pin=4, invert_colors=true, color_order=BGR,
  mirror_x=true`. Confirmed present in **2026.5.1** and the config validates:

```
$ esphome config wt32-sc01-plus.yaml
  INFO Configuration is valid!        # model → bus_mode: octal, BGR, invert, mirror_x
```

So `wt32-sc01-plus.yaml` drives the screen natively:

```yaml
spi:
  - id: octal_spi
    type: octal
    clk_pin: 47                 # WR = octal clock
    data_pins: [9, 46, 3, 8, 18, 17, 16, 15]
display:
  - platform: mipi_spi
    model: WT32-SC01-PLUS       # bakes dc=0, reset=4, BGR, invert, mirror_x
    spi_id: octal_spi
```

Plus LEDC backlight on GPIO45, FT6336U touch on I²C (SDA 6 / SCL 5, INT 7), and
an LVGL banner+heartbeat. (Credit: octal/model path from the HA-community
"fully working with PSRAM" thread; pinout verified by esp32-exp; i80 PR
archaeology by esp32-exp.)

**Buddy-port note:** the WT32-SC01 Plus is still *the* canonical LovyanGFX/
TFT_eSPI reference board, so it remains the strongest **buddy-port** target for
the desk-pet's TFT_eSprite rendering (45 `M5.*` + 22 `TFT_eSprite` refs) — that's
the Arduino path, independent of this ESPHome config.

## Verified pinout (single source of truth)

```
LCD ST7796 (8080 8-bit):  WR=47   DC/RS=0   RESET=4   RD=tied-high   ~20MHz
LCD data D0..D7:          9, 46, 3, 8, 18, 17, 16, 15
Backlight:                GPIO 45 (LEDC PWM)
Touch FT6336U (I2C):      addr 0x38   SDA=6   SCL=5
microSD (SPI):            CS=41   CLK=39   MISO=38   MOSI=40
extras:                   RS485 + audio CODEC/speaker
```
Sources: doc.panel-tag.com, homeding SC01-plus, Bodmer/TFT_eSPI #2319,
sukesh-ak LovyanGFX gist. (Compiled by esp32-exp.)

## Flash

```bash
# from repo root — confirm which port is THIS board first:
make boards
make flash APP=wt32-sc01-plus PORT=/dev/cu.usbmodem1201
```
