# jc3248-pet-idf — the bufo desk-pet on native ESP-IDF v6

A frog (`bufo`) that animates on a **Guition JC3248W535** (ESP32-S3, 3.5″ 320×480
QSPI AXS15231 panel), reacts to taps with a heart + a ribbit, shows a live clock
and Claude usage, and signs itself *Cat Lab Brewing*. A from-scratch port of the
Arduino/PlatformIO pet (`../jc3248-pet/`) to native ESP-IDF v6 — *many bodies, one soul.*

## Screen

```
┌──────────────────────────┐
│                21:38     │  clock — HH:MM over date, top-right
│             Sat 30 May   │
│                          │
│        🐸  (frog GIF)    │  AnimatedGIF, 3× upscale, centered
│                          │
│     Cat Lab Brewing      │  always-on signature
├────────────────────────●─┤  ● link dot (green linked / gray idle)
│ bufo                     │  pet name
│ 5h 47% 3h01m          Ψ  │  usage windows + the amber Ψ mark
│ wk 42% 1d11h          Ψ  │
└──────────────────────────┘
```

## Two-feed BLE (the important part)

The pet is **BLE-only (no Wi-Fi)**, so the host feeds it over BLE — via **two
independent feeds** so the clock is robust and the usage is rich:

| Feed | Transport | Host script | Survives no-connection? |
|------|-----------|-------------|--------------------------|
| 🕒 **Time** | connectionless **broadcast** (the pet *scans* for it) | `tools/broadcast_time.py` | **Yes** |
| 📊 **Usage** (5h/wk) | NUS **connection** (the host writes heartbeats) | `tools/usage_bridge.py` | No |

- **Time** rides a BLE advertisement: the host broadcasts the local name
  `clk:<epoch>:<tzoff>`; the pet (NimBLE **observer** role) scans for it and sets
  its clock with no link, then ticks locally off `esp_timer`. Re-broadcast every
  120 s corrects drift. The connection's heartbeat *also* carries `epoch`, so time
  works whether or not a broadcast is heard.
- **Usage** rides the NUS connection (`6e400002…` RX): `usage_bridge.py` reads
  Claude Code's live rate-limits and writes a JSON heartbeat (`pct5h`, `reset5h`,
  `pct7d`, `reset7d`, …) every 8 s.

### Why an *active* scan + *local name* (learned from `../beacon`)

**macOS CoreBluetooth only lets a host advertise a LocalName + ServiceUUIDs — no
manufacturer data** — and it often puts the name in the **scan response**. So:

- the payload must ride the **local name** (`clk:…`), and
- the listener **must use an ACTIVE scan** (`ble_gap_disc` `passive=0`) to receive
  the scan response. A passive scan silently misses it.

This is exactly how the prior ESPHome beacon receiver (`../beacon/beacon-display.yaml`)
worked — `esp32_ble_tracker: scan_parameters: {active: true}` matching on
`x.get_name()`. The IDF pet does the same by hand in `main/ble.c`. (The first cut
used a passive scan and saw nothing — one-line fix.)

### Coexistence
The pet **advertises** (peripheral, for the usage connection) **and scans**
(observer, for the time broadcast) at once. To stop the active scan from
radio-starving the link, the scan is **paused while connected** and resumed on
disconnect — so a connected usage bridge stays solid, and the connection's `epoch`
keeps time during that window.

## Build / flash

```bash
make build              # reproducible build (pins the IDF py3.13 venv; see Makefile)
make flash              # build + flash to PORT (default /dev/cu.usbmodem2101)
make monitor            # serial console (USB-Serial-JTAG; there is no UART bridge)
make fullclean          # wipe build/; needed after sdkconfig.defaults changes
```

After editing `sdkconfig.defaults` (e.g. a NimBLE role), `make fullclean` and move
`sdkconfig` aside so it regenerates from defaults — defaults only seed a *fresh* config.

## Second board: the 2.8" ILI9341 (lcd28)

The same firmware also runs on a 2.8" ESP32-S3 board (Freenove ESP32-S3 Display 2.8 /
LCDWIKI ES3C28P pinout: ILI9341 240x320 on SPI, FT6336 touch, ES8311 codec, 16 MB flash,
8 MB octal PSRAM). `main/board.h` picks the drivers and the layout; everything above them
(GIF engine, HUD, clock, BLE, OTA, wasm apps) is shared.

```bash
tools/build-lcd28.sh          # -> build-lcd28/oraclepet_lcd28.bin (+ bootloader, tables, storage)
```

- Layout: the cat at 2x (192x200) under a one-line clock, the oracle name under it, a 76 px HUD.
- Project name `oraclepet_lcd28` (the JC3248's stays `jc3248_pet_idf`): the BLE OTA name check
  refuses the other board's image, and OraclePet only offers the matching one.
- First flash over USB replaced xiaozhi: erase 0x9000-0x10000 (xiaozhi's NVS/otadata sit at other
  offsets), then bootloader, table, `ota_data_initial.bin` at 0xd000, app at 0x10000, storage at
  0x290000. It advertises as `Claude-D350` and updates over BLE from then on.
- Portrait needs the ILI9341's X mirror (`BOARD_LCD_MIRROR_X`), or the picture is a mirror image.

## Update over BLE (OTA, like nRF52 DFU)

Once the pet runs a firmware with the OTA layout, it no longer needs the USB cable:

```bash
make build                                   # or idf.py build
cd ../desk-pet-mac && just ota               # sends build/jc3248_pet_idf.bin to the linked pet
just status                                  # "ota": "Updating pet firmware… 61%" -> "Updated in 26 s: now … on ota_1"
```

Or Desk Pet's menu bar: **Update Pet Firmware…** and pick the `.bin`. The pet shows
"Updating NN%", checks the image, switches slot and restarts. Measured 2026-10-21:
676,720 bytes in 26 s (about 26 KB/s), ota_0 to ota_1.

- **First time: flash over USB once.** The OTA layout changes the partition table and the
  bootloader (rollback), which BLE cannot write. Write `ota_data_initial.bin` at 0xd000 as well
  (`idf.py flash` does): that range held old NVS pages before.
- **Layout** (`partitions.csv`): ota_0 stays at 0x10000 and storage stays at 0x290000, so a
  USB flash writes the same offsets as before and the GIF packs survive; ota_1 takes the free tail.
- **Safety**: the pet refuses an image that is not `jc3248_pet_idf` (app description) or whose
  CRC-32 differs, and `esp_ota_end` checks the image SHA-256. A new image must run ~10 s with
  display and BLE up before it marks itself valid; a crash before that and the bootloader
  returns to the old slot. A dropped link mid-transfer leaves the old firmware running.
- **Protocol**: `main/ota.h`. Service `6e400010-…`, control `…11` (read version / commands /
  JSON notify), data `…12` (write without response, 16 KB window, acked every 4 KB).
- The GIF packs (LittleFS `storage`) are not updated over BLE; they still go over USB.

## Run the host feeds (macOS — needs Bluetooth permission)

```bash
# robust clock (connectionless broadcast)
uv run tools/broadcast_time.py

# usage windows + green link dot (connection); reads Claude Code's statusline JSON
/tmp/blevenv/bin/python tools/usage_bridge.py    # or: uv run --with bleak tools/usage_bridge.py
```
Run **both** for the full screen; run just the broadcaster for a clock that needs no link.

## Layout

```
main/   display.c (esp_lcd QSPI + LEDC backlight)   touch.c (AXS15231 INT-gated)
        fs.c (LittleFS)   gif.cpp (AnimatedGIF 3× + signature)
        hud.c + hud_gfx.c (hand-rolled RGB565 font/blitter; byte-swapped for the BE panel)
        petclock.c (top-right clock)   audio.c (i2s_std ribbit, off-loop task)
        ble.c (NimBLE: peripheral NUS + observer time-broadcast scan)   ota.c (BLE firmware update)
components/animatedgif_prebuilt/   AnimatedGIF as prebuilt .a + .h (source in ../../third_party_src/)
tools/  broadcast_time.py (time)   poke_time.py (one-off time)
```

Toolchain: ESP-IDF v6.0.1, target `esp32s3`. 8 MB QIO flash, 8 MB OPI PSRAM.
The Arduino pet (`../jc3248-pet/`) is untouched.
