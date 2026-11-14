# oracle-beacon — Rust port (CLI + tray + WASI)

A polished Rust port of the BLE broadcaster pair (`broadcast-status.sh`
+ `ble-beacon.py`). Three build targets from one source:

| Target | Binary | What it does | Size |
|---|---|---|---|
| **CLI** (native) | `target/release/oracle-beacon` | JSON → format → filter → gate → native BLE advert via CoreBluetooth (`objc2-core-bluetooth`). No Python subprocess. | ~620 KB |
| **Tray** (native, `--features tray`) | `target/release/oracle-beacon-tray` | macOS menu-bar app. Background thread runs the broadcaster, foreground thread shows live `ψ Op X% qY%` in the title and a menu with the current payload + Quit. | ~5 MB |
| **WASI** (`wasm32-wasip1`) | `target/wasm32-wasip1/release/oracle-beacon.wasm` | Same JSON + filter + gate logic, dry-run only. Runs in any WASI host (`wasmtime`, browsers via wasi-shim). | ~345 KB |

Architecture:

```
                ┌──────────────────────────────┐
                │ src/lib.rs    pure logic     │
                │   - Statusline (serde)       │
                │   - Sample, LoopState        │
                │   - decide() filter+gate     │
                │   - extract_sample()         │
                │   - advertise() trait        │
                └──────────────┬───────────────┘
                               │
       ┌───────────────────────┼───────────────────────┐
       │                       │                       │
       ▼                       ▼                       ▼
  bin/oracle-beacon.rs   bin/oracle-beacon-tray.rs   wasm32-wasip1
  (thin CLI)             (winit + tray-icon)         (dry-run only)
       │                       │                       │
       ▼                       ▼                       ▼
  ble_macos.rs           ble_macos.rs              (no advertise)
  (CBPeripheralManager)  (same)
       │
       ▼
  📡 BLE 31-byte advert →  ESP32 dashboard
```

## Wire format

After the `oracle:` BLE local-name prefix:

```text
<model>|<ctx_used_k>|<q7d_pct>|<reset>
```

Example: `Op|263|39|3d11h` → 15-byte payload, 22-byte total.

BLE budget is 26 bytes (31-byte adv packet minus 3 for Flags AD minus
2 for AD header). The CLI rejects payloads that would overflow.

Model codes:
- `Op` → OPUS family (firmware renders "OPUS 4.7 / 1M context")
- `So` → SONNET family
- `Ha` → HAIKU family

The ESP32 firmware (`../beacon-display.yaml`) does the model→ctx_max
lookup and bar rendering.

## Quickstart

```bash
# Cheap validation first (per the /flash skill's pre-check pattern)
make check
make test               # 8 tests, no fixtures

# CLI — single broadcast
make run

# CLI — loop mode
make run-loop

# Menu-bar app
make run-tray           # opens a ψ icon in the macOS menu bar

# WASI dry-run (works on any OS)
brew install wasmtime           # if not installed
rustup target add wasm32-wasip1 # if not installed
make wasm-run

# Install both binaries
make install            # → ~/.local/bin/oracle-beacon{,-tray}
```

## Native BLE (macOS)

Uses `objc2-core-bluetooth` directly. ~40 lines of delegate plumbing in
`src/ble_macos.rs`:

1. Create a serial `DispatchQueue` for CoreBluetooth callbacks
2. Instantiate a stub `CBPeripheralManagerDelegate` (no-op state callback)
3. `initWithDelegate:queue:` on `CBPeripheralManager`
4. Poll `manager.state()` until `CBManagerState::PoweredOn` (5s timeout)
5. Build `NSDictionary { CBAdvertisementDataLocalNameKey: "oracle:..." }`
6. `startAdvertising:`
7. Sleep for the requested duration
8. `stopAdvertising`

Clean shutdown is guaranteed because the manager is dropped at the end
of the function (Rust RAII releases the CoreBluetooth registration). No
more ghost advertisements like the Python version had.

## Tray app (macOS menu bar)

`oracle-beacon-tray` uses `tray-icon` + `winit` for a polished menu-bar
experience. Architecture:

- **Background thread**: reads statusline JSON every 30s, applies the
  session filter and sanity gate, advertises the payload via the
  shared `advertise()` function. Sends the latest accepted `Sample`
  over an mpsc channel.
- **Main thread**: `winit::EventLoop` with a custom `ApplicationHandler`.
  On every poll tick (500ms): drains the sample channel and updates
  the tray title + menu items, drains menu events for Quit.
- **Shutdown**: clicking Quit sets an `AtomicBool`; the broadcaster
  thread checks it on every loop and exits cleanly. The
  `CBPeripheralManager` Drop releases the BLE registration.

Title format: `ψ Op 32% q39%` (≤22 chars so macOS doesn't elide).

Menu:
- `📡 oracle:Op|263|39|3d11h` (current payload, disabled / informational)
- `last broadcast: 22:14:17`
- ─────
- Quit oracle-beacon

## Sanity gates

Both layers from the bash version are ported as Rust unit-tested logic:

| Gate | What |
|---|---|
| **Session filter** | `/tmp/claude-statusline/raw.json` is shared across all Claude Code instances on the machine. We extract `.session_id` and skip cycles where it doesn't match the target. Auto-detects target from the most-recently-modified `.jsonl` in the cwd's project dir. |
| **Sanity gate** | Reject any cycle where `ctx_used_k` jumps >50% within 30s vs the last accepted sample. That's not normal session growth — it's a race on the shared file. |

Both gates are tested:

```bash
$ make test
running 8 tests
test tests::extract_sums_all_token_buckets ... ok
test tests::fmt_reset_humanises_correctly ... ok
test tests::model_code_maps_known_families ... ok
test tests::payload_format_is_pipe_delimited ... ok
test tests::sanity_gate_accepts_small_growth ... ok
test tests::sanity_gate_rejects_50pct_jump_within_30s ... ok
test tests::session_filter_rejects_foreign ... ok
test tests::tray_title_fits_short_format ... ok
```

## WASI use cases

- Validate format/filter logic on any platform without a Mac
- Pipe arbitrary JSON in via `--json /tmp/test.json` to test corner cases
- Future: serve from a browser sandbox as a "test harness" page

```bash
echo '{...test json...}' > /tmp/test.json
wasmtime --dir=/tmp::/tmp target/wasm32-wasip1/release/oracle-beacon.wasm \
  --dry-run --json /tmp/test.json --session af022cb2
```
