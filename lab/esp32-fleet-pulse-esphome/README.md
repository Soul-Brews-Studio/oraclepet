# esp32-fleet-pulse-esphome

The **ESPHome + LVGL** sibling of `lab/esp32-fleet-pulse/` (the Arduino/LovyanGFX
version). Same board (Guition **JC3248W535** — ESP32-S3 + AXS15231 QSPI 320×480),
same backend (**maw** `:3456`), but a *declarative* config: ESPHome's built-in
`mipi_spi` AXS15231 driver + LVGL widgets + `http_request`, no C++ HAL.

## What it shows
A live **fleet session-list dashboard**: polls `http://192.168.1.109:3456/api/ls`
every 5 s, parses the JSON, and updates 16 LVGL rows (session name + window count,
active sessions highlighted white vs idle gray).

> Why ESPHome+LVGL for the *list* (not the terminal content): LVGL excels at
> structured widgets (a clean dashboard, OTA-updatable). For the raw `/api/capture`
> terminal stream, the Arduino version stays better (LVGL would mangle dense ANSI).
> Same board, two faces. Built base reused from `../jc3248w535/jc3248w535.yaml`.

## Setup
```bash
cp secrets.yaml.example secrets.yaml   # then fill in wifi_ssid / wifi_password
esphome config   fleet-pulse.yaml      # validate
esphome run      fleet-pulse.yaml      # compile + flash (USB)
```
(`esphome` via `pipx install esphome` or `uvx esphome`.)

## Files
- `fleet-pulse.yaml` — the JC3248W535 session-list dashboard (board + display + LVGL + http_request)
- `terminal.yaml` — the JC3248W535 tmux-pane terminal viewer (portrait, 320×480 QSPI)
- `terminal-landscape.yaml` — the AXS15231-landscape dead-end (in-repo as honest record)
- `terminal-waveshare7.yaml` — Waveshare **7** (800×480, CH422G) landscape port
- `terminal-waveshare7-portrait.yaml` — Waveshare 7 portrait variant
- `terminal-waveshare7-toggle.yaml` — Waveshare 7 with web-switchable orientation
- `terminal-waveshare7b.yaml` — the **Waveshare 7B port** of the terminal viewer (1024×600 RGB landscape-native — see below)
- `BLOG.md` — full backstory + every gotcha from the JC3248W535 bring-up
- `secrets.yaml` — wifi creds, **gitignored**
- `secrets.yaml.example` — template

Built by the `/team-agents` team `fleet-pulse-esphome` (base-port / http-json / lvgl-ui).

---

## Waveshare 7B port (`terminal-waveshare7b.yaml`)

The landscape sibling that the JC3248W535's AXS15231 wouldn't render. Target board: **Waveshare ESP32-S3-Touch-LCD-7B** — 1024×600 RGB-parallel, GT911 touch, CH32V003 backlight controller. ESPHome-able today via the `mipi_rgb` model + an unmerged external_component for the CH32V003 I/O expander.

> Don't confuse the 7B with the plain **ESP32-S3-Touch-LCD-7** — that one is 800×480 and uses a CH422G for backlight. Different panels, different IO chips. See `~/.claude/.../memory/reference_waveshare_7b.md` for the per-board pin map.

### What changes vs `terminal.yaml`
**Display-AGNOSTIC** (preserved verbatim, no edits): the entire `fetch_pane` lambda (ANSI→LVGL recolor, UTF-8 decode, Thai/emoji handling, dash-cap, trailing-space defer, bottom-pin via blank-line prepend, `lv_label_set_text_static`, header header-skip cache), the `refresh_panes` lambda (FixedVector<const char*> dynamic dropdown), the `poll_loop`, the JetBrains Mono + Sarabun font extras at 3 sizes, the http_request/web_server/wifi shape, the globals, the select/number/button/interval/text_sensor blocks.

**Display-SPECIFIC** (the irreducible seam):
- `display:` — `mipi_rgb` model:RPI 1024×600 @ **30MHz** pclk inverted, color_order RGB, byte_order little_endian. Porches: hsync 152/162/48 (back/pulse/front), vsync 13/45/3. Pinout: DE=5, HSYNC=46, VSYNC=3, PCLK=7; R=[1,2,42,41,40] G=[39,0,45,48,47,21] B=[14,38,18,17,10]. Confirmed against ESPHome's official Waveshare built-in map.
- `external_components:` — **PINNED** to `github://latonita/esphome@799f67369e7847706ec633e288a45a89c4c24bce` (NOT `github://pr#10071`). The PR is OPEN with CHANGES_REQUESTED; a `refresh: 1h` against its head can silently break the build. Bump the pin when it merges upstream.
- `power_supply` + `output: waveshare_io_ch32v003` — backlight enable + PWM through CH32V003 EXIO 2. **PWM safety mandatory**: `inverted: true` + `safe_pwm_levels { min_value: 1, max_value: 247 }` (247/255 cap is circuit-mandated).
- `touchscreen:` — `gt911` on I2C bus_a (SDA=8, SCL=9), INT=GPIO4, reset via CH32V003 EXIO 1.
- LVGL geometry refit: term box y=24, width=1020, height=572; VISCOLS bumped 80→120; per-size fit table sm=40 / md=31 / lg=28 (off-by-one verified by adversarial review).

### Quick start
```sh
cp secrets.yaml.example secrets.yaml          # fill in wifi_ssid / wifi_password
uvx esphome config terminal-waveshare7b.yaml  # validate
uvx esphome run    terminal-waveshare7b.yaml --device /dev/cu.usbmodemXXXX --no-logs
uvx esphome logs   terminal-waveshare7b.yaml --device /dev/cu.usbmodemXXXX
```
The native USB-Serial-JTAG port enumerates as a short `usbmodemXXXX` and auto-resets reliably; the CH343 UART bridge port is the longer name and is fussier on macOS.

### First-flash results (2026-05-31)

Built against ESPHome 2026.5.1 / IDF (esp-idf framework, octal PSRAM @ 80MHz, execute_from_psram).

| Bin | Size | sha256 |
|-----|------|--------|
| `firmware.factory.bin` round 1 (placeholder wifi) | 1,126,816 B | `b177118a973f4707784ab59a5b6989a4409e87cf12df3d32be4cb6510bfbd80e` |
| `firmware.bin` / `.ota.bin` round 1 | 1,126,816 B | `699fcbf5c75c159fa063246a5c93315f697510509bdc15f659a9fdad23b5bcd2` |
| `firmware.factory.bin` round 2 (real wifi) | 1,126,816 B | `728ddd967ac3d11911e9b93fa5be953ab1e4a583898c36a89ed3f8a6dc9b2529` |
| `firmware.bin` / `.ota.bin` round 2 | 1,126,816 B | `d50186cf4f1c99543d944e85790b09624deb7a962aae1e94b30ac278db85730f` |

Steady-state poll cycle: **270–400ms median** (BELOW the JC3248W535's documented 455ms post-fix floor, on a panel ~4× the pixel area). The `lv_label_set_text_static` + VISCOLS clip + trailing-space defer + render-only-on-change all ported and paid off at scale.

### Known soft risk
**pclk 30MHz is aggressive for 1024×600** (~32fps, thin margin). If visible flicker / tear / horizontal shift appears: drop `pclk_frequency` to `25MHZ` first; only widen porches if that doesn't settle.

### Plan-B (only if backlight stays black)
The CH32V003 component (PR #10071) is unmerged. If the LED's dead even with a clean boot log, sidestep the component entirely and assert backlight via a raw I2C write to address `0x24` on bus_a from `on_boot`. The CH32V003 EXIO2 is backlight enable; the agillis hardware YAML's component source has the register map.

### Browser flash via `webflasher/`
Sibling dir in the repo root (gitignored) — esp-web-tools install page lifted from `laris-co/esphome-fw/installers/run/template`. Build first, copy `firmware.factory.bin` next to `manifest.json`, run `./serve.sh`, open `http://localhost:8123/` in desktop Chrome / Edge.
