# beacon — pure-BLE text broadcast oracle

> Two pieces of code, one ESP32 board, zero network.
> One advertises a string over BLE; the other shows it big on the screen.

## What this is

A small sibling of `lab/jc3248w535/` (brewing dashboard) sharing the same
JC3248W535EN hardware but a *different content surface*. Flash one OR
the other — they're separate firmwares, not coexisting features.

| Piece | Where | What |
|---|---|---|
| `beacon-display.yaml` | this dir | ESPHome firmware — scans BLE adverts, filters by name prefix `oracle:`, renders payload in noto_56 |
| `ble-beacon.py` | this dir | PEP 723 single-file uv script — advertises a string in the BLE local name as `oracle:<text>` via `bless` |

No Wi-Fi. No MQTT. No API. No OTA. The device only listens for BLE
advertisements; the broadcaster only sends them. Range ≈ 10 m line-of-sight.

## Why local-name encoding (not manuf data)

macOS CoreBluetooth restricts outbound BLE advertisements to LocalName +
ServiceUUIDs — no manufacturer data is exposed to userland. So the payload
rides in the local name with an `oracle:` prefix, and the firmware filters
on that prefix. Max payload ≈ 21 chars (BLE advert is 31 B total, 28 B after
the prefix overhead).

## Flash

```bash
make compile      # uvx esphome compile beacon-display.yaml
make run          # compile + upload + tail logs
make upload       # upload only
make logs         # tail device logs
```

No `secrets.yaml` needed — the YAML has no `!secret` references.

## Broadcast a beacon

```bash
# direct:
uv run ble-beacon.py "hello catlab"

# or via make:
make broadcast TEXT="hello catlab"

# time-bounded:
BLE_DURATION=30 uv run ble-beacon.py "30s only"
```

`bless` will request BLE permissions on first run — grant Bluetooth access
to your terminal (System Settings → Privacy & Security → Bluetooth).

## Verify end-to-end

In one pane:

```bash
make broadcast TEXT="hello catlab"
```

In another:

```bash
make logs
```

You should see, within a few seconds:

```
[I][beacon:050]: MATCH text='hello catlab' t=12345ms
```

…and the screen updates to show **hello catlab** in big letters.

## How it fits the wider project

- `lab/jc3248w535/` — brewing dashboard, MQTT-fed from xiaoer-oracle
- **`lab/beacon/`** — bare BLE beacon receiver, no network at all
- `lab/oracle-app/` — Flutter cross-platform viewer of the brewing MQTT topics

The board is the form; what the board *is* depends on which firmware you
flash. One body, many roles.
