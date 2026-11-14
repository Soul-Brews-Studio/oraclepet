# oracle-app-tauri — all-in-one BLE broadcaster + tray + window

A single Tauri 2.x app that does **all three jobs**:

1. **Broadcaster** — same BLE algorithm as the standalone `oracle-beacon`, runs in a background thread (no MQTT, never any MQTT)
2. **Tray** — macOS menu-bar icon with live status (`ψ Op 23% q39%`)
3. **Window** — full dashboard mirroring the ESP32 firmware's layout

```
                 ┌────────────────────────────────┐
                 │     Tauri 2.x app process       │
                 │                                 │
                 │  ┌──────────────────────────┐   │
                 │  │  background thread       │   │
                 │  │  broadcaster::run()       │──┼─► BLE advert
                 │  │   read JSON →             │   │   oracle:Op|236|39|3d11h
                 │  │   decide() filter+gate →  │   │
                 │  │   advertise() native CB   │   │
                 │  └─────────┬────────────────┘   │
                 │            │ status_tx           │
                 │            ▼                     │
                 │  ┌──────────────────────────┐   │
                 │  │  tray::spawn_pump()      │   │
                 │  │   set_title(),            │   │
                 │  │   emit("status-update")  │──┼─► JS: render()
                 │  └──────────────────────────┘   │
                 │            ▲                     │
                 │            │ pull (5s safety)    │
                 │  ┌─────────┴────────────────┐   │
                 │  │  #[command] get_status   │   │
                 │  └──────────────────────────┘   │
                 └────────────────────────────────┘
                              ▲
                              │ shared path-dep
                              ▼
                       oracle_beacon
                       (cli-rs lib)
```

Same JSON source (`/tmp/claude-statusline/raw.json`), same wire format
(`oracle:<model>|<used>|<q7d>|<reset>`), same render layout as the ESP32.

## What the user sees

- **Menu bar**: a `ψ` icon with live title `ψ Op 23% q39%`
- **Right-click the icon**: menu with current payload, last-broadcast time, Show/Hide window, Quit
- **The window**: full dashboard with two color-graded bars + footer; closing it hides instead of quits (Quit is via the tray menu)

## Quickstart

```bash
cd lab/oracle-app-tauri
make check      # cargo check the backend
make test       # 9 tests across status + broadcaster modules
make dev        # opens both window AND tray in dev mode
make release    # production .app bundle
```

## Module layout (≤ 350 lines/file)

```
src-tauri/src/
├── main.rs            6   thin entry, calls oracle_app_tauri_lib::run()
├── lib.rs            61   tauri Builder + setup hook spawns broadcaster + tray
├── status.rs        162   StatusJson + friendly_name + snapshot + tray_title
├── broadcaster.rs   105   BLE loop thread (filter + gate + advertise)
└── tray.rs           91   Tauri tray icon + menu + status-pump
```

## Tests (9 across 3 suites)

```bash
$ make test
running 7 tests in status.rs
  friendly_name_known_codes ... ok
  snapshot_round_trips_opus ... ok
  snapshot_missing_file_errors ... ok
  snapshot_invalid_json_errors ... ok
  json_keys_stable ... ok
  tray_title_short_format ... ok
  from_sample_maps_fields ... ok

running 2 tests in broadcaster.rs
  config_constants_sane ... ok
  run_exits_promptly_when_quit_set ... ok
```

## No MQTT

Explicit non-feature: this app uses **BLE only**. Anywhere we might
have considered a transport — broadcaster output, frontend pull,
inter-thread sample passing — we use mpsc channels or BLE adverts.

`grep -ri mqtt lab/oracle-app-tauri lab/beacon/cli-rs` returns nothing.

## Future polish

- App icons in multiple resolutions (`icon.icns`, `icon.ico`, retina PNGs) for `cargo tauri build` bundle output
- Sparkline of `ctx_used_k` over time in the window
- Frontend tests with Playwright + a mock backend invoke shim
