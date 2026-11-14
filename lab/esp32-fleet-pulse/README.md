# esp32-fleet-pulse

> The embedded sibling of HomekeeperWidget's **SessionPulse** — a small ESP32-S3
> screen on your desk that shows the live `maw` session list, awake when nothing
> else is, keeping the flame in 4MB of flash.

A Guition **JC3248W535** (ESP32-S3 + AXS15231, 320x480 QSPI portrait LCD) joins
WiFi, polls a `maw` backend over HTTP every ~5 seconds, parses the session JSON,
and renders a scrollable list: each session shows its name, window count, and a
green dot if any of its windows is active. The header shows `node/oracle` and the
total session count. WiFi-drop and HTTP-error states are surfaced on-screen.

## The chain

```
ESP32-S3  ──HTTP GET──►  192.168.1.109:3456/api/ls  ──►  maw
 (this)                   (backend, BACKEND_URL)         (session source)
```

The backend returns JSON shaped roughly like:

```json
{
  "ok": true,
  "node": "mba",
  "oracle": "esp32",
  "sessions": [
    { "name": "1-exp", "windows": [ { "index": 0, "name": "edit", "active": true } ] }
  ]
}
```

Only `ok` / `node` / `oracle` and, per session, `name` + each window's
`index`/`name`/`active` are deserialized (ArduinoJson v7 **Filter** + stream
parse), so a chatty backend still costs only a few KB of heap.

## Hardware

- **Board:** Guition JC3248W535 (ESP32-S3-WROOM, 8MB flash, PSRAM).
- **Display:** AXS15231 320x480 QSPI, driven by the self-contained native HAL
  `src/axs15231_qspi.h` (`namespace axs`). The render layer draws into a
  LovyanGFX **sprite in PSRAM** and calls `axs::push(spr.getBuffer())` to blit the
  full frame — the HAL does NOT use `Lcd.init()`.
- **Touch:** AXS15231 capacitive over I2C, INT-gated on GPIO3. Tap the top third
  to scroll up, the bottom third to scroll down (only when the list overflows).

## Set up `secrets.h`

`src/secrets.h` holds your real WiFi + backend URL and is **gitignored** — never
commit it (family golden rule: never commit secrets).

```sh
cp src/secrets.h.example src/secrets.h
$EDITOR src/secrets.h          # fill in WIFI_SSID / WIFI_PASS / BACKEND_URL
```

If `secrets.h` is missing, the build **fails loudly** with a clear `#error` rather
than coming up blank at runtime.

```c
#define WIFI_SSID   "your-ssid"
#define WIFI_PASS   "your-pass"
#define BACKEND_URL "http://192.168.1.109:3456/api/ls"
```

## Build & flash

Requires [PlatformIO](https://platformio.org/) (`pio`).

```sh
pio run                                 # build
pio run -t upload                       # flash over USB (native USB CDC)
pio device monitor -b 115200            # serial monitor
# or in one go:
pio run -t upload -t monitor
```

The env is `jc3248-fleet` (the default). The board env — pioarduino platform pin,
`memory_type = qio_opi`, 8MB flash, `no_ota.csv` partitions, PSRAM — is copied
verbatim from the proven `lab/jc3248-pet` foundation. **Do not** swap the
`platform =` pin or `memory_type` for a generic `espressif32@6.x`; that
combination is what brings PSRAM + the QSPI panel up.

## On-screen states

| State        | Screen                                                |
|--------------|-------------------------------------------------------|
| Booting      | `fleet-pulse / booting...`                            |
| WiFi joining | `WiFi / <ssid>`                                        |
| WiFi failed  | red `WiFi FAIL / check secrets.h / AP`                |
| Backend error| keeps the last good list with a `stale` flag (or, if no data yet, an orange `BACKEND / GET <code> retrying` banner) |
| OK           | the session list                                      |

## Status

First-of-its-kind for the fleet: no prior family firmware combined
**WiFi + HTTPClient + ArduinoJson on an LCD** (Oracle search found only
ESPHome/display and BLE precedents). The board/display layer is proven; the
WiFi/HTTP/JSON layer should get a real **cold-boot hardware test** before being
declared "proven."
