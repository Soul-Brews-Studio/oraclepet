# tmux-tray (Swift)

A native macOS **menubar app** that lists tmux sessions and gives you a **live,
interactive terminal** into any pane — pure Swift + AppKit, **no Tauri, no web
bundle, zero dependencies**. Built as a proof-of-understanding sibling to
homekeeper's `HomekeeperLive` (which does the same over SwiftTerm).

## The chain

```
menubar  → GET  http://127.0.0.1:3456/api/ls        → list sessions/windows
click    → ws://127.0.0.1:3456/ws/pty {type:"attach", target:"sess:win", …}
type     → binary keystrokes → PTY stdin            → echo streams back
```

The backend is **maw-js** (`maw serve`, already running on `:3456`) — the same
`/api/ls` + `/ws/pty` endpoints `HomekeeperLive` uses. No SSH, no extra server.

## Run

```bash
cd lab/tmux-tray-swift
swift run                # a terminal icon appears in the menubar
```

Click the icon → `🟢 N sessions · M windows`. Click a window → a live terminal
opens. Type into it (it's the **real pane** — Enter/Tab/arrows/Ctrl-combos/⌘V
all work). The icon dims if `:3456` is unreachable.

> ⚠️ **Live input.** Keystrokes go to the real session. Try a spare shell pane first.

Override the backend (e.g. a different host/port) with `TMUX_REMOTE_URL`:
```bash
TMUX_REMOTE_URL=http://127.0.0.1:3457 swift run
```

## Files

| File | Role |
|---|---|
| `Sources/tmux-tray/App.swift` | tray (`NSStatusItem`) + `/api/ls` poll + click→mirror |
| `Sources/tmux-tray/PTYMirror.swift` | `/ws/pty` window: stream→`NSTextView`, keystrokes→socket, ANSI strip |
| `server-local.ts` | optional standalone localhost backend (reads *this* machine's tmux); redundant when maw is up |

## What it demonstrates

- **Menubar idiom** (from `steipete/codexbar`): `NSStatusItem` + `.accessory` policy
- **`localhost` gotcha**: default to `127.0.0.1` — macOS resolves `localhost`→IPv6 `::1`, but bun binds IPv4 only
- **WebSocket PTY**: `URLSessionWebSocketTask`, attach JSON + binary stdio, async `receive()` loop
- **Terminal input**: `keyDown` → terminal byte sequences (CSI for arrows, C0 for Ctrl, DEL for backspace, ESC-meta for Option)
- **Swift 6 concurrency**: `@main @MainActor` bootstrap, no `Timer`/`@Sendable` capture traps

## Not done yet

- Output is **ANSI-stripped** (no colors) — next step is SGR parsing into `NSAttributedString`.
- No app bundle / icon / signing — `swift run` only.
- No window resize → PTY `resize` message (fixed 110×34).
