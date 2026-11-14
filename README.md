# OraclePet

An open desk pet for ESP32-S3 boards, and the lab it grew out of. A small animated character
lives on a screen next to your computer, follows what your AI agents are doing over Bluetooth
LE, and takes new firmware, animation packs and WebAssembly apps over the air.

**Flash one from your browser:** https://soul-brews-studio.github.io/oraclepet/

- **USB** (first install): plug the board in, pick a character, press Install. Uses
  [ESP Web Tools](https://esphome.github.io/esp-web-tools/) over Web Serial.
- **Bluetooth** (every update after that): connect to the pet, then send firmware, a
  character pack or an app over Web Bluetooth. Firmware updates go to the idle OTA slot and
  roll back if the new image does not confirm itself.

Both need Chrome or Edge on desktop, or Chrome on Android. Safari, Firefox and iOS have
neither Web Serial nor Web Bluetooth.

## Boards

| board | screen | firmware project |
|---|---|---|
| Guition JC3248W535 | 3.5″ 320×480, AXS15231B QSPI, touch | `jc3248_pet_idf` |
| WT32-SC01 Plus | 3.5″ 480×320, ST7796 | `oraclepet_sc01` |
| ESP32-S3 2.8″ (ILI9341 + ES8311 audio) | 2.8″ 240×320 | `oraclepet_lcd28` |

Each board's firmware has its own project name, and the pet refuses an over-the-air image
built for another board.

## What is in here

The pet itself:

| folder | what |
|---|---|
| [`lab/jc3248-pet-idf`](lab/jc3248-pet-idf) | the pet firmware: native ESP-IDF v6, display, touch, GIF engine, BLE (NimBLE), OTA, audio, WAMR app runtime |
| [`lab/oraclepet-lib`](lab/oraclepet-lib) | the BLE protocol ([`PROTOCOL.md`](lab/oraclepet-lib/PROTOCOL.md)) with matching C, Python and browser libraries, golden test vectors, and the Web Bluetooth page |
| [`lab/desk-pet-mac`](lab/desk-pet-mac) | OraclePet for macOS (Swift, CoreBluetooth): links the pets, shows agent state, sends OTA updates |
| [`lab/pet-sdk`](lab/pet-sdk) | build pet apps in C, Zig, Rust or AssemblyScript to `.wasm` |
| [`lab/pet-studio`](lab/pet-studio) | a local web page and MCP server over the tools above |
| [`lab/buddy/characters`](lab/buddy/characters) | the character packs (each has its own license note) |

The rest of the lab, kept as it was built: the Arduino desk pet this started from
(`lab/buddy`, a fork of Anthropic's MIT-licensed `claude-desktop-buddy`), WebAssembly on the
ESP32 (`lab/gif-wasm`, `lab/gif-wamr`, `lab/esp32-wasm-submission`), browser simulators
(`lab/sim-gallery`), ESPHome dashboards (`lab/jc3248w535`, `lab/waveshare-2.8b`,
`lab/esp32-fleet-pulse-esphome`), Discord and WebRTC voice on the ESP32, and write-ups of the
bring-up bugs in [`blog/`](blog).

## Build the firmware yourself

ESP-IDF v6.0.1, target `esp32s3`:

```sh
cd lab/jc3248-pet-idf
mkdir -p data/characters && cp -R ../buddy/characters/bufo data/characters/
idf.py set-target esp32s3
idf.py -DPET_DEFAULT_PACK=bufo build flash
```

Other boards: `idf.py -B build-sc01 -DPET_BOARD=sc01 -DSDKCONFIG=build-sc01/sdkconfig build`
(or `lcd28`). The GitHub Pages site is built by
[`.github/workflows/pages.yml`](.github/workflows/pages.yml) from this same source.

## Security notes

- The OTA service has no pairing or passcode. Anyone in Bluetooth range can send the pet
  an image while it is advertising. The pet checks the size, CRC and project name and rolls
  back an image that fails to boot, but it does not check who sent it. Do not use it where
  that matters.
- Configs that need credentials (Wi-Fi, MQTT, OTA passwords) read them from a local
  `secrets.yaml` or the environment. Copy the `*.example` files; never commit the real ones.

## License

MIT for the code in this repository, see [`LICENSE`](LICENSE). Third-party code keeps its own
license (AnimatedGIF: Apache-2.0, `parking_lot_core`: MIT/Apache-2.0, `lab/buddy`: MIT,
Anthropic). Character art: see each pack's `PROVENANCE.md` or `README.md`. The `bufo` GIFs are
third-party art from bufo.zone and are not covered by the MIT license.
