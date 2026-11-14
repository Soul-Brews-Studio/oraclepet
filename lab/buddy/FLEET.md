# Buddy Fleet — hardware, build matrix, upstream sync

The Claude desk-pet (`claude-desktop-buddy`) ported to a **LovyanGFX HAL** so one
codebase runs on every board we own. Board variants are **platformio envs, not
git branches** — add a board = one `hal_<board>.h` + one `[env:]`.

## 🗂️ Hardware inventory

All boards are **ESP32-S3** → all have **Bluetooth 5 (LE)**, so the buddy's
**BLE transport works on every one**. (The 7″ is a separate WiFi/WebSocket build.)

| Board | PSRAM / Flash | Display | Touch | BLE | MAC |
|---|---|---|---|---|---|
| **JC3248W535** (Guition) | 8 MB octal / 16 MB | AXS15231 320×480 **QSPI** | AXS15231 `0x3B` | ✅ | `98:a3:16:e3:ec:c8` |
| **WT32-SC01 Plus** (Panlee) | 2 MB quad / 16 MB | ST7796 320×480 **8080** | FT6336U `0x38` | ✅ | `68:b6:b3:36:b0:18` |
| **Waveshare 2.8B** | 8 MB octal / 16 MB | ST7701S 480×640 **RGB** | GT911 (TCA9554) | ✅ | `a0:f2:62:e3:d9:50` |
| **C3-HMI** (MakerAsia) | none / 4 MB | ST7789 240×320 **SPI** | FT5x06 `0x38` | ✅ (BLE 5.0) | `34:b4:72:12:98:20` |
| **ATD3.5-S3** (ArtronShop) | yes / — | ST7796 3.5″ **SPI** | FT5x06 | ✅ | — |
| **LilyGo EPD47** | yes / 4 MB | 4.7″ **e-paper** (epdiy) | — | ✅ | — |
| **M5StickC Plus** | — / — | 135×240 (baseline) | buttons | ✅ | — |
| *Waveshare 7″* (sibling) | 8 MB / 16 MB | ST7262 **RGB** 7″ | — | *(WiFi/WS)* | — |

## 🔨 Buddy build matrix (`pio run -e <env>`)

| env | Board | Transport | Display lib | Build |
|---|---|---|---|---|
| `m5stickc-plus` | M5StickC Plus | BLE | M5/TFT_eSPI | baseline |
| `atd35-s3` | ATD3.5-S3 | BLE | LovyanGFX | env present |
| `jc3248w535c` | JC3248W535 | BLE | LovyanGFX sprite + **native AXS15231 QSPI** (`axs15231_qspi.h`) | ✅ renders on-device |
| `wt32-sc01-plus` | WT32-SC01 Plus | BLE | LovyanGFX (`Bus_Parallel8`) | ✅ Flash 66% |
| `lilygo-epd47` | LilyGo EPD47 | BLE | epdiy | env present |
| `c3-hmi` | MakerAsia C3-HMI | BLE | LovyanGFX (`Panel_ST7789` SPI) | ⏳ HAL written, untested |
| *(todo)* `waveshare-2.8b` | Waveshare 2.8B | BLE | LovyanGFX RGB (`Bus_RGB`/`Panel_ST7701`) | ⏳ not yet written |

> **JC3248W535 note:** the AXS15231 is a DC-less QSPI-DBI panel LovyanGFX can't
> drive. LovyanGFX still renders the buddy into a sprite; `axs15231_qspi.h` (a
> native ESP-IDF QSPI driver) pushes that buffer. Key gotchas it handles:
> ≤32 KB SPI chunks (S3 GPSPI limit), standalone RAMWR `0x2C` + `0x3C` continue,
> a PSRAM→internal DMA bounce buffer, MSB-first RGB565 (MADCTL `0x00`), and the
> **full AXS15231 vendor init + SWRESET** so the panel comes up from a **cold
> power cycle** (a minimal init only works on warm reset, riding leftover register
> state — a replug shows noise). Build with `-DAXS_BOOT_DIAG` for a boot
> colour-cycle self-test. See the three `blog/2026-05-30-axs15231-*` posts for the
> full debugging story.

Each LovyanGFX board defines **two** flags: `-DBOARD_<NAME>` (selects
`hal_<board>.h`) **and** `-DBOARD_ATD35_S3` (pulls in the shared `HalM5`
wrapper + stubs in `hal.h`, via `using LGFX_ATD35 = LGFX_<board>`).

> The ESP32-S3 envs are pinned to **pioarduino 55.3.38** (esptool-v5 compatible).
> Stock `espressif32 @ 6.9.0` fails the bootloader step on machines with esptool v5
> (it calls the removed `esptool.py`).

## 🔄 Pulling Anthropic's upstream updates

This `lab/buddy/` is a **vendored snapshot**. The live upstream-tracking clone is
`lab/claude-desktop-buddy/` (gitignored, its own `.git`, `origin =
github.com/anthropics/claude-desktop-buddy`). To get Anthropic's updates:

```bash
cd lab/claude-desktop-buddy
git fetch origin                 # pull upstream main
git checkout atd35-s3-port       # our multi-board HAL branch
git merge origin/main            # fold in Anthropic's changes
pio run -e jc3248w535c           # re-verify a board builds
# then re-sync source into the vendored snapshot:
rsync -a --exclude '.git/' --exclude '.pio/' --exclude 'secrets.h' \
      --exclude 'bridge/wifi_creds.json' ./ ../buddy/
```

Our work (the HALs `hal_jc3248.h`, `hal_sc01plus.h`, `hal_atd35`, `hal_epd47`,
the `bridge/` layer) sits on `atd35-s3-port` *on top of* upstream `main`, so
upstream changes merge cleanly underneath.

## Flash a board

```bash
# from repo root — confirm the chip first (golden rule)
make ports && make chipid
cd lab/buddy
pio run -e <env> -t upload --upload-port /dev/cu.usbmodemXXXX
```

## Remaining

- **Waveshare 2.8B HAL** — deliberately *not* attempted as a LovyanGFX HAL
  (would be a rabbit-hole). The blocker: on this board the ST7701S's **CS and
  RESET are behind the PCA9554 I²C expander** (`0x20`), and so is the GT911
  touch reset. ESPHome's `st7701s` platform supports expander-backed pins
  (`cs_pin: { pca9554: exio }`); **LovyanGFX cannot drive control pins through
  an I²C expander** — same class of problem as the GT911 addr-latch timing
  (see `lab/waveshare-2.8b/`). It also needs `Bus_RGB` + 16 data pins
  (`red[46,3,8,18,17] green[14,13,12,11,10,9] blue[5,45,48,47,21]`, de=40,
  hsync=38, vsync=39, pclk=41 @16MHz) + the ST7701 SPI init sequence.
  **Verdict:** the buddy desk-pet skips this board; its working firmware is the
  pure-ESPHome dashboard (`lab/waveshare-2.8b/`). The buddy runs on every
  GPIO-direct board (the 5 above).
- Retire the superseded M5-only `lab/buddy-port/`.
