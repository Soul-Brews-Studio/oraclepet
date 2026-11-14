# Design

## Source of truth
- Status: Active Claude Home + BLE receive-only Buddy page
- Last refreshed: 2026-06-05
- Primary product surfaces: `CardputerBLEReader` SwiftUI iPad/iPhone Claude Buddy receiver.
- Evidence reviewed:
  - `CardputerBLEReader/ContentView.swift` — current three-tab Buddy/Home/RX SwiftUI surface.
  - `CardputerBLEReader/BuddyBLEModel.swift` — Claude Home HTTP poller plus CoreBluetooth NUS peripheral receiver and legacy scanner/debug decoder.
  - `lab/buddy/bridge/ble_bridge.py` — Python heartbeat source, `--print-heartbeat`, BLE bridge, and `--serve-http` Claude Home feed.
  - `/opt/Code/github.com/moremas/build-with-claude/buddy/device/buddy_protocol.py` — Buddy BLE message source of truth.
  - `/opt/Code/github.com/nat-build-with-oracle/build-with-claude/buddy/device/apps/claude_buddy.py` — Cardputer app entry, keyboard bindings, `/flash/apps` install shape.
  - `/opt/Code/github.com/nat-build-with-oracle/build-with-claude/buddy/device/apps/hello_cardputer.py` and `snake.py` — shared three-zone Cardputer chrome evidence.
  - `/opt/Code/github.com/nat-build-with-oracle/build-with-claude/buddy/device/buddy_ui_cp.py` — authoritative 240×135 Cardputer visual contract.
  - `design/ipad-buddy-ui-real-ipad.svg` — canonical SVG-first iPad visual reference for the SwiftUI port.
  - `/tmp/cardputer_ble_ipad_attachments/263BF380-BA95-42EB-BB8A-D449630F1688.png` — physical iPad capture showing the old form was visually plain and unclear about mock vs live data.

## Brand
- Personality: technical, trustworthy, luminous, receive-proof instrument panel not toy form.
- Trust signals: explicit provenance labels, Claude Home poll/byte counts, RX write counts, byte counts, raw RX/Home JSON, decoded rows, and bridge payload source.
- Avoid: prefilled values that look like device truth; claiming Buddy data before a BLE write arrives.

## Product goals
- Goals: let Nat use the iPad as a Claude Buddy display with real Python-sent Claude statusline data now, and still prove whether Buddy protocol data is really received over BLE when the host Bluetooth launcher is fixed.
- Non-goals: production App Store polish, hidden automation of destructive commands, file push over unauthenticated BLE.
- Success signals: first screen answers “is this mock or live?” without reading logs; screenshot looks like a real diagnostic console.

## Personas and jobs
- Primary personas: Nat debugging JC/Cardputer Buddy BLE; future agents verifying hardware proof.
- User jobs: poll the Claude Home feed, advertise as a Claude/NUS peripheral, receive valid Buddy heartbeat JSON, inspect live Home/RX evidence, and debug Python bridge discovery.
- Key contexts of use: physical iPad beside ESP32 hardware, low-light bench debugging, screenshot-based handoff.

## Information architecture
- Primary navigation: three tabs — full Buddy page first, Claude Home feed second, BLE RX console third.
- Core screens: full Cardputer Buddy mirror, Claude Home feed proof, live BLE receive proof dashboard, receiver state/raw JSON, optional scanner/debug panels, protocol map, event log.
- Content hierarchy: Buddy page first; Home proof second; raw/debug details in BLE RX tab; static protocol reference last.

## Design principles
- Truth before controls: provenance/status must precede editable mock/test values.
- No silent mockups: local draft, live TX, and live RX are separate visual states.
- Dense but readable: use compact monospaced evidence rows inside large touch-friendly cards.

## Visual language
- Color: Anthropic/Cardputer palette from `buddy_ui_cp.py` anchors the hero (`ORANGE #CC785C`, `CREAM #F0EEE6`, `DARK #1F1F1F`, black); cyan for scan/NUS, green for live/ack, orange for draft/warning, purple for device RX/status.
- Typography: rounded bold iPad headings; monospaced evidence rows; the Cardputer mirror uses a small monospaced DejaVu9-like style at the original 240×135 coordinates.
- Spacing/layout rhythm: real-iPad frame reference first; inside the app, large Cardputer mirror hero, then three proof tiles, then controls and detailed cards.
- Shape/radius/elevation: iPad glass/screen frame in SVG; SwiftUI cards use rounded glass panels with subtle borders and shadows; Cardputer mirror uses a black device bezel and small-radius LCD screen.
- Motion: none required.
- Imagery/iconography: text-first; avoid decorative icons that reduce clarity.

## Components
- Existing components to reuse: SwiftUI buttons, Grid/LazyVGrid, TextField, Stepper.
- New/changed components: hero card, status pill, proof tile, metric tile, evidence rows, JSON box.
- Variants and states: Home polling, Home live, advertising/no-write, central subscribed/no-heartbeat, real BLE RX, decoded heartbeat, TCC/bridge failure.
- Token/component ownership: local SwiftUI helpers in `ContentView.swift`; no new design-system dependency.

## Accessibility
- Target standard: readable iPad/iPhone debug UI, high contrast on dark background.
- Keyboard/focus behavior: default SwiftUI text fields/buttons.
- Contrast/readability: status colors paired with text labels; no color-only truth state.
- Screen-reader semantics: visible text labels for all states.
- Reduced motion and sensory considerations: no required animation.

## Responsive behavior
- Supported breakpoints/devices: iPad primary; iPhone supported via adaptive grids and vertical scroll.
- Layout adaptations: proof tiles and metric tiles use adaptive grids.
- Touch/hover differences: touch-first controls; no hover-only affordances.

## Interaction states
- Loading: scanning state shown in connection pill/log.
- Empty: “No Claude/NUS devices yet”, “none yet” rows for RX/TX.
- Error: write failures and missing BLE path shown in proof state/log.
- Success: “LIVE BLE TX — OS write acknowledged” and/or “REAL NOTIFY”.
- Disabled: buttons remain active but failed writes are labeled draft/no BLE path.
- Offline/slow network: not networked; BLE absence is explicit.

## Content voice
- Tone: direct bench-debug honesty, bilingual where it clarifies truth states.
- Terminology: “Claude Home”, “receive-only”, “real BLE RX”, “server.rx_writes”, “home.polls”, “claude-statusline”, “TCC crash”.
- Microcopy rules: never call app-generated test values “real device data”; call Home live only after the Python HTTP heartbeat returns real statusline JSON; call BLE writes/notifications live only when CoreBluetooth evidence exists.

## Implementation constraints
- Framework/styling system: SwiftUI only for the iPad app, no third-party dependencies; Python bridge uses Bleak.
- Design-token constraints: local colors/components; keep code small and reviewable.
- Performance constraints: Claude Home polls every 5 seconds; CoreBluetooth peripheral callbacks update observed RX state.
- Compatibility constraints: iOS deployment target 17.0; physical iPad signing team `6K28WEXX78`.
- Test/screenshot expectations: simulator build, physical iPad install, XCUITest screenshot capture.

## Open questions
- [x] Real Claude data source moved to Python bridge via `/tmp/claude-statusline/raw.json`; iPad receives only.
- [ ] macOS Python/Bleak needs a launcher/TCC state that satisfies `NSBluetoothAlwaysUsageDescription` before live scan can connect.


## Implementation notes

- Session counters default to zero and message defaults to `DRAFT TEST — no live Claude source`; real-looking values must not be hardcoded into the app.
- `Scan`, `Stop scan`, and `Disconnect` update a visible control feedback line so taps are observable even when no BLE peripheral is present.
- The app may show `REAL WRITE` only after `writeValue` is called on a connected NUS RX characteristic, and may show `REAL NOTIFY` only after a TX notification arrives from the buddy.
- The Cardputer mirror must keep `buddy_ui_cp.py` coordinates: 240×135 landscape, header `y=0..20`, orange hairline at `y=20`, idle rows at `y=28/48/66/84`, hint strip `y=111..134`, connected identity/queue/token/message rows at `y=26/42/58/74`, prompt box `x=3 y=74 w=234 h=35`, and footer stats at `y=98`.
- `HOME..` / `HOME` are the visual states for the Python Claude Home feed. `PAIR..` is the visual state for a connected CoreBluetooth peripheral without live heartbeat/notify proof. `LINKED` is reserved for proven live BLE data so the iPad screen does not overstate the BLE trust level.


## Receive-only update — 2026-06-05

- Buddy tab is now the primary page and mirrors the Cardputer UI at large iPad scale.
- Claude Home tab is the current usable real-data path: it polls `ble_bridge.py --serve-http` and renders the same heartbeat from `/tmp/claude-statusline/raw.json`.
- BLE RX tab keeps debug controls and raw evidence, but the app no longer presents iPad-generated draft heartbeat values as live Buddy data.
- `BuddyBLEModel` starts a `Claude-iPad` NUS peripheral automatically when Bluetooth is powered on.
- Live Buddy rows come from `claudeHomeRows` or `heartbeatRXRows` only. `HOME` means real Python HTTP data; `LINKED` is reserved for real CoreBluetooth RX JSON; `ADV` means advertising but no data yet.
- `ble_bridge.py --print-heartbeat` proves the heartbeat source without BLE. The payload includes `source: claude-statusline`, project, session, model, context/quota fields, and tokens from `/tmp/claude-statusline/raw.json`.
- `ble_bridge.py --serve-http --host 0.0.0.0 --port 8765` serves `/heartbeat` for iPad polling; the Buddy screen shows `HOME` only after bytes are received from that endpoint.
- Physical-iPad screenshot capture is supported by the UI test; the latest rerun was blocked by the iPad lock screen, while build/install still succeeded.
- Current live-BLE blocker: macOS TCC kills Python/Bleak scanning with missing `NSBluetoothAlwaysUsageDescription` before it connects. Treat that as host launcher permission failure, not mock data.
