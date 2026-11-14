# Cardputer BLE Reader

SwiftUI iPad/iPhone receiver for the Claude/Cardputer Buddy BLE bridge.

The app now has three tabs:

1. **Claude Buddy** — a full-page Cardputer-style Claude Buddy screen, drawn from the real `buddy_ui_cp.py` visual contract.
2. **Claude Home** — a real-data HTTP receiver for the Python bridge's Claude statusline heartbeat.
3. **BLE RX** — receiver/debug console with raw JSON, decoded rows, scan controls, and logs.

## Transport

The app acts as a **receive-only Nordic UART Service (NUS) peripheral** so the Python bridge can connect to the iPad the same way it connects to a hardware Buddy:

- Advertised local name: `Claude-iPad`
- Service: `6e400001-b5a3-f393-e0a9-e50e24dcca9e`
- RX / desktop → iPad write: `6e400002-b5a3-f393-e0a9-e50e24dcca9e`
- TX / iPad → desktop notify/read: `6e400003-b5a3-f393-e0a9-e50e24dcca9e`

The iPad UI does **not** show app-generated heartbeat counters as live data. The Buddy screen uses explicit transport states:

- `HOME..` / `HOME` — the Python Claude Home HTTP feed is polling / has delivered real statusline data.
- `ADV` — the iPad is advertising as a NUS peripheral but has no live data yet.
- `LINKED` — CoreBluetooth received real bytes on the NUS RX characteristic.

## Real-data proof model

A screenshot must answer “mock or real?” without reading code:

1. **Python bridge target** — iPad advertises `Claude-iPad` + NUS only. Session data is not hidden inside BLE advertisements.
2. **Claude Home polling** — `home.polls`, `home.bytes`, and decoded fields increment only after HTTP GETs to the Python bridge.
3. **RX writes** — `server.rx_writes` and `server.rx_bytes` increment only when a BLE central writes to NUS RX.
4. **Decoded Buddy data** — raw JSON and flattened rows update only from received Python Home or CoreBluetooth bytes.

The old iPad-generated TX heartbeat controls are not part of the primary UI anymore. They remain internal/debug code only; the visible Buddy tab is receive-first.

## Python bridge data source

`lab/buddy/bridge/ble_bridge.py` builds its heartbeat from the real Claude statusline JSON:

- Default source: `/tmp/claude-statusline/raw.json`
- Override: `CLAUDE_STATUSLINE_JSON=/path/to/raw.json`
- Included fields: project, session, model, context usage, quota usage/resets, token counts, session counts.

Before testing BLE, prove the payload is real and not mock:

```sh
uv run --with bleak python lab/buddy/bridge/ble_bridge.py --print-heartbeat | python3 -m json.tool
```

Expected proof fields include:

```json
{
  "source": "claude-statusline",
  "project": "...",
  "session": "...",
  "model": "...",
  "tokens": 123,
  "context_used_pct": 42,
  "q7d_pct": 43
}
```

## Claude Home HTTP feed — current working real-data path

macOS currently TCC-kills the Python/Bleak scan path before BLE can connect on Nat's machine, so the app also has a **Claude Home** tab that receives the same real heartbeat over local HTTP.

Run the feed on the Mac:

```sh
PYTHONUNBUFFERED=1 uv run --with bleak python \
  lab/buddy/bridge/ble_bridge.py \
  --serve-http --host 0.0.0.0 --port 8765
```

Prove the feed is real:

```sh
curl -s http://127.0.0.1:8765/heartbeat | python3 -m json.tool
```

The iPad default URL is editable in the Claude Home tab:

```text
http://192.168.1.141:8765/heartbeat
```

The Buddy screen shows `HOME` only after this endpoint returns statusline data with `source: "claude-statusline"`. It does not use mock values for the Claude Buddy page.

## Run the bridge in maw

```sh
maw new cardputer-ble-rx \
  --path /opt/Code/github.com/Soul-Brews-Studio/esp32-oracle/agents/3-claude-buddy \
  --window bridge \
  --print \
  --no-attach

maw run cardputer-ble-rx:bridge \
  'PYTHONUNBUFFERED=1 uv run --with bleak python -X faulthandler lab/buddy/bridge/ble_bridge.py'

maw peek cardputer-ble-rx:bridge
```

For the current HTTP feed path:

```sh
maw new cardputer-home \
  --path /opt/Code/github.com/Soul-Brews-Studio/esp32-oracle/agents/3-claude-buddy \
  --window bridge \
  --print \
  --no-attach

maw run cardputer-home:bridge \
  'PYTHONUNBUFFERED=1 uv run --with bleak python lab/buddy/bridge/ble_bridge.py --serve-http --host 0.0.0.0 --port 8765'

maw peek cardputer-home:bridge
```

## macOS Python BLE privacy note

On Nat's macOS 26.4.1 test machine, the iPad app built/installed and advertised successfully, and `--print-heartbeat` proved real statusline data. The actual Python BLE scan still crashed before connection with:

```text
Termination Reason: Namespace TCC, Code 0
This app has crashed because it attempted to access privacy-sensitive data without a usage description.
The app's Info.plist must contain an NSBluetoothAlwaysUsageDescription key...
```

That crash is a macOS TCC/privacy launcher issue for Python/Bleak, not a mock-data issue in the iPad app. Use `maw peek` to distinguish these states:

- `--print-heartbeat` succeeds → bridge can build real Buddy JSON from Claude statusline.
- `[bridge] scanning...` followed by TCC crash → Python process lacks an accepted Bluetooth usage description / permission grant.
- `[bridge] found Claude-iPad` / `connected` / `heartbeat loop started` → live BLE path is running and the iPad should show RX writes.

## Generate

```sh
xcodegen generate
```

## Install on Nat's iPad

```sh
xcodebuild \
  -project CardputerBLEReader.xcodeproj \
  -scheme CardputerBLEReader \
  -destination 'platform=iOS,id=00008103-000109A20AD3001E' \
  -configuration Debug \
  -allowProvisioningUpdates \
  -allowProvisioningDeviceRegistration \
  install \
  CODE_SIGN_STYLE=Automatic \
  DEVELOPMENT_TEAM=6K28WEXX78 \
  CODE_SIGN_IDENTITY='Apple Development'
```

Launch:

```sh
xcrun devicectl device process launch \
  --device 'iPad (4)' \
  studio.soulbrews.CardputerBLEReader \
  --terminate-existing \
  --activate
```

## Capture physical iPad screenshot

The included UI test captures the app screen from the physical iPad and stores it as an xcresult attachment.

```sh
rm -rf /tmp/cardputer_ble_ipad_capture.xcresult /tmp/cardputer_ble_ipad_attachments
xcodebuild test \
  -project CardputerBLEReader.xcodeproj \
  -scheme CardputerBLEReader \
  -destination 'platform=iOS,id=00008103-000109A20AD3001E' \
  -configuration Debug \
  -resultBundlePath /tmp/cardputer_ble_ipad_capture.xcresult \
  -allowProvisioningUpdates \
  -allowProvisioningDeviceRegistration \
  CODE_SIGN_STYLE=Automatic \
  DEVELOPMENT_TEAM=6K28WEXX78 \
  CODE_SIGN_IDENTITY='Apple Development'

xcrun xcresulttool export attachments \
  --path /tmp/cardputer_ble_ipad_capture.xcresult \
  --output-path /tmp/cardputer_ble_ipad_attachments
```

Note: keep the iPad unlocked while the UI test runs. The exported attachment path is transient under `/tmp/cardputer_ble_ipad_attachments/`.

## Device firmware note

This iPad app is the safe receive-side test surface. It does not require merging the Cardputer sketch into the production JC firmware. If testing the opt-in JC Cardputer sketch separately, keep using `CARDPUTER_PORT_SKETCH=ON`; default JC firmware remains `OFF`.
