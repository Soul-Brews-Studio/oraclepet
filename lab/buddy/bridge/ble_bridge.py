#!/usr/bin/env python3
"""
BLE bridge: connects Claude Code on Linux to the claude-desktop-buddy ESP32.

Scans for the buddy device (advertising NUS), pairs, then:
  - sends time sync on connect
  - sends periodic heartbeat with Claude Code session status
  - receives permission responses (approve/deny) from touch UI

Usage:
  uv run --with bleak bridge/ble_bridge.py
  # or: pip install bleak && python bridge/ble_bridge.py

Optional for richer session info:
  pip install psutil
"""

import argparse
import asyncio
import glob
import json
import os
import time
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from bleak import BleakClient, BleakScanner
from bleak.exc import BleakError

NUS_SERVICE = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
NUS_RX      = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
NUS_TX      = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

HEARTBEAT_SEC = 5
DEVICE_PREFIX = "Claude-"
STATUSLINE_PATH = Path(os.getenv("CLAUDE_STATUSLINE_JSON", "/tmp/claude-statusline/raw.json"))
CLAUDE_DIR    = Path.home() / ".claude"
TRANSCRIPT_DIR = CLAUDE_DIR / "transcripts"
PROJECTS_DIR = CLAUDE_DIR / "projects"
ACTIVE_WINDOW = 30  # seconds — statusline/transcript modified within this = active session

try:
    import psutil
    HAS_PSUTIL = True
except ImportError:
    HAS_PSUTIL = False


def count_claude_sessions():
    """Detect Claude Code sessions from statusline/project JSONL files + process list."""
    now = time.time()
    total = 0
    running = 0
    seen = set()

    # Method 1: current statusline source of truth from Claude Code statusline hook.
    if STATUSLINE_PATH.exists():
        try:
            age = now - STATUSLINE_PATH.stat().st_mtime
            total += 1
            if age < ACTIVE_WINDOW:
                running += 1
            seen.add(str(STATUSLINE_PATH))
        except OSError:
            pass

    # Method 2: modern Claude Code project JSONL files.
    if PROJECTS_DIR.exists():
        for f in PROJECTS_DIR.glob("*/*.jsonl"):
            if str(f) in seen:
                continue
            try:
                age = now - f.stat().st_mtime
                if age < 3600:
                    total += 1
                if age < ACTIVE_WINDOW:
                    running += 1
            except OSError:
                continue

    # Method 3: legacy transcript files (older desktop buddy reference).
    if TRANSCRIPT_DIR.exists():
        for f in TRANSCRIPT_DIR.glob("ses_*.jsonl"):
            try:
                age = now - f.stat().st_mtime
                if age < 3600:
                    total += 1
                if age < ACTIVE_WINDOW:
                    running += 1
            except OSError:
                continue

    # Method 4: process scan (supplement if psutil available).
    if HAS_PSUTIL and total == 0:
        for proc in psutil.process_iter(["name", "cmdline"]):
            try:
                cmdline = " ".join(proc.info["cmdline"] or [])
                if "claude" in cmdline.lower() and ("--session" in cmdline or "code" in cmdline.lower()):
                    total += 1
                    if proc.status() == psutil.STATUS_RUNNING:
                        running += 1
            except (psutil.NoSuchProcess, psutil.AccessDenied):
                continue

    return {"total": total, "running": running, "waiting": 0}


def _get(obj, path, default=None):
    cur = obj
    for key in path:
        if not isinstance(cur, dict) or key not in cur:
            return default
        cur = cur[key]
    return cur


def _int(value, default=0):
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


def _usage_total(status):
    usage = _get(status, ["context_window", "current_usage"], {}) or {}
    fields = ("input_tokens", "output_tokens", "cache_creation_input_tokens", "cache_read_input_tokens")
    return sum(_int(usage.get(k)) for k in fields)


def _reset_minutes(epoch):
    epoch = _int(epoch)
    if not epoch:
        return 0
    return max(0, int((epoch - time.time()) // 60))


def _short(text, limit=23):
    text = str(text or "").replace("\n", " ").strip()
    return text[:limit]


def read_statusline_snapshot():
    if not STATUSLINE_PATH.exists():
        return None
    try:
        status = json.loads(STATUSLINE_PATH.read_text())
        mtime = STATUSLINE_PATH.stat().st_mtime
    except (OSError, json.JSONDecodeError) as e:
        return {"error": str(e), "path": str(STATUSLINE_PATH)}

    usage_total = _usage_total(status)
    project = _get(status, ["workspace", "repo", "name"]) or Path(status.get("cwd", "")).name or "claude"
    session = status.get("session_name") or status.get("session_id", "")[:8]
    model = _get(status, ["model", "display_name"]) or _get(status, ["model", "id"]) or "Claude"
    used_pct = _int(_get(status, ["context_window", "used_percentage"]))
    out_tokens = _int(_get(status, ["context_window", "current_usage", "output_tokens"]))
    q5h = _get(status, ["rate_limits", "five_hour"], {}) or {}
    q7d = _get(status, ["rate_limits", "seven_day"], {}) or {}
    cost = _get(status, ["cost"], {}) or {}

    msg = f"{project} ctx {used_pct}% q7d {_int(q7d.get('used_percentage'))}%"
    return {
        "source": "claude-statusline",
        "source_path": str(STATUSLINE_PATH),
        "source_age_sec": max(0, int(time.time() - mtime)),
        "project": project,
        "session": session,
        "session_id": status.get("session_id", ""),
        "model": model,
        "msg": _short(msg),
        "tokens": usage_total,
        "tokens_today": out_tokens,
        "context_used_pct": used_pct,
        "context_limit": _int(_get(status, ["context_window", "context_window_size"])),
        "q5h_pct": _int(q5h.get("used_percentage")),
        "reset5h": _reset_minutes(q5h.get("resets_at")),
        "q7d_pct": _int(q7d.get("used_percentage")),
        "reset7d": _reset_minutes(q7d.get("resets_at")),
        "entries": [
            _short(f"{model} · {project}", 48),
            _short(session, 48),
            _short(f"ctx {used_pct}% · q7d {_int(q7d.get('used_percentage'))}%", 48),
        ],
        "cost_usd": round(float(cost.get("total_cost_usd", 0) or 0), 4),
    }


def make_time_sync():
    now = datetime.now(timezone.utc)
    epoch = int(now.timestamp())
    local = datetime.now().astimezone()
    tz_offset = int(local.utcoffset().total_seconds())
    return json.dumps({"time": [epoch, tz_offset]})


def make_heartbeat(sessions=None, msg="bridge connected"):
    """Build the Buddy heartbeat from real statusline/session data, never UI mock values."""
    s = sessions or count_claude_sessions()
    snapshot = read_statusline_snapshot()
    if snapshot and "error" not in snapshot:
        msg = snapshot.get("msg") or msg
        heartbeat = {
            "total": max(_int(s.get("total")), 1),
            "running": max(_int(s.get("running")), 1 if snapshot.get("source_age_sec", 999) < ACTIVE_WINDOW else 0),
            "waiting": _int(s.get("waiting")),
            "completed": False,
            "msg": _short(msg),
            "tokens": _int(snapshot.get("tokens")),
            "tokens_today": _int(snapshot.get("tokens_today")),
            "entries": snapshot.get("entries", []),
            "source": snapshot.get("source"),
            "project": snapshot.get("project"),
            "session": snapshot.get("session"),
            "model": snapshot.get("model"),
            "context_used_pct": snapshot.get("context_used_pct"),
            "context_limit": snapshot.get("context_limit"),
            "q5h_pct": snapshot.get("q5h_pct"),
            "reset5h": snapshot.get("reset5h"),
            "q7d_pct": snapshot.get("q7d_pct"),
            "reset7d": snapshot.get("reset7d"),
        }
    else:
        err = snapshot.get("error") if snapshot else "missing statusline"
        heartbeat = {
            "total": _int(s.get("total")),
            "running": _int(s.get("running")),
            "waiting": _int(s.get("waiting")),
            "completed": False,
            "tokens": 0,
            "tokens_today": 0,
            "msg": _short(msg if s.get("total") else "no statusline source"),
            "source": "session-scan-fallback",
            "source_error": err,
        }
    return json.dumps(heartbeat, separators=(",", ":"))




class ClaudeHomeHTTPHandler(BaseHTTPRequestHandler):
    """Tiny dev HTTP feed so iPad can receive real Claude data when macOS BLE is TCC-blocked."""

    server_version = "ClaudeHomeBuddy/0.1"

    def log_message(self, fmt, *args):
        print(f"[home] {self.address_string()} {fmt % args}")

    def _send_bytes(self, status, body, content_type="application/json"):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Cache-Control", "no-store")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.end_headers()
        self.wfile.write(body)

    def do_OPTIONS(self):
        self._send_bytes(204, b"")

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path in ("/", "/health"):
            body = json.dumps({
                "ok": True,
                "service": "claude-home-buddy-feed",
                "heartbeat": "/heartbeat",
                "source": str(STATUSLINE_PATH),
            }, separators=(",", ":")).encode("utf-8")
            self._send_bytes(200, body)
            return
        if path == "/heartbeat":
            self._send_bytes(200, (make_heartbeat() + "\n").encode("utf-8"))
            return
        self._send_bytes(404, json.dumps({"ok": False, "error": "not found"}).encode("utf-8"))


def serve_http(host, port):
    server = ThreadingHTTPServer((host, port), ClaudeHomeHTTPHandler)
    addr = server.server_address
    print(f"[home] serving real Claude heartbeat at http://{addr[0]}:{addr[1]}/heartbeat")
    print(f"[home] source={STATUSLINE_PATH}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[home] stopped")
    finally:
        server.server_close()


class BuddyBridge:
    def __init__(self):
        self.client = None
        self.rx_buffer = ""
        self.connected = False

    def on_disconnect(self, client):
        self.connected = False
        print("[bridge] disconnected")

    def on_notify(self, sender, data: bytearray):
        text = data.decode("utf-8", errors="replace")
        self.rx_buffer += text

        while "\n" in self.rx_buffer:
            line, self.rx_buffer = self.rx_buffer.split("\n", 1)
            line = line.strip()
            if not line or not line.startswith("{"):
                continue
            try:
                msg = json.loads(line)
                self._handle_message(msg)
            except json.JSONDecodeError:
                print(f"[bridge] bad JSON: {line[:80]}")

    def _handle_message(self, msg):
        cmd = msg.get("cmd")
        if cmd == "permission":
            pid = msg.get("id", "")
            decision = msg.get("decision", "")
            print(f"[bridge] PERMISSION id={pid} decision={decision}")
        else:
            print(f"[bridge] rx: {msg}")

    async def send(self, text):
        if not self.client or not self.connected:
            return
        data = (text + "\n").encode("utf-8")
        chunk = 180
        for i in range(0, len(data), chunk):
            part = data[i : i + chunk]
            try:
                await self.client.write_gatt_char(NUS_RX, part, response=False)
                await asyncio.sleep(0.005)
            except BleakError as e:
                print(f"[bridge] write error: {e}")
                return

    async def scan_and_connect(self):
        print("[bridge] scanning...")
        device = None

        while device is None:
            devices = await BleakScanner.discover(timeout=5.0)
            for d in devices:
                name = d.name or ""
                if name.startswith(DEVICE_PREFIX):
                    device = d
                    print(f"[bridge] found {d.name} ({d.address})")
                    break
            if device is None:
                print("[bridge] not found, retry 3s...")
                await asyncio.sleep(3)

        print(f"[bridge] connecting {device.address}...")
        self.client = BleakClient(device.address, disconnected_callback=self.on_disconnect)
        await self.client.connect()
        self.connected = True
        print(f"[bridge] connected (MTU={self.client.mtu_size})")

        await self.client.start_notify(NUS_TX, self.on_notify)

        ts = make_time_sync()
        print(f"[bridge] time sync: {ts}")
        await self.send(ts)
        await asyncio.sleep(0.1)

        hb = make_heartbeat(msg="bridge online")
        await self.send(hb)
        print(f"[bridge] heartbeat loop started: {hb}")

    async def run_heartbeat(self):
        while self.connected:
            await asyncio.sleep(HEARTBEAT_SEC)
            if not self.connected:
                break

            sessions = count_claude_sessions()
            if sessions["running"] > 0:
                msg = f"{sessions['running']} active"
            elif sessions["total"] > 0:
                msg = f"{sessions['total']} sessions"
            else:
                msg = "bridge connected"
            hb = make_heartbeat(sessions, msg)
            await self.send(hb)
            print(f"[bridge] heartbeat: {hb}")

    async def run(self):
        while True:
            try:
                await self.scan_and_connect()
                await self.run_heartbeat()
            except BleakError as e:
                print(f"[bridge] BLE: {e}")
            except Exception as e:
                print(f"[bridge] error: {e}")
            finally:
                if self.client:
                    try:
                        await self.client.disconnect()
                    except Exception:
                        pass
                self.connected = False
            print("[bridge] reconnect in 5s...")
            await asyncio.sleep(5)


def main():
    parser = argparse.ArgumentParser(description="Claude Buddy BLE bridge")
    parser.add_argument("--print-heartbeat", action="store_true", help="print the real heartbeat JSON and exit without BLE")
    parser.add_argument("--serve-http", action="store_true", help="serve a real Claude Home HTTP heartbeat feed for iPad polling")
    parser.add_argument("--host", default="0.0.0.0", help="HTTP host for --serve-http (default: 0.0.0.0)")
    parser.add_argument("--port", type=int, default=8765, help="HTTP port for --serve-http (default: 8765)")
    args = parser.parse_args()
    if args.print_heartbeat:
        print(make_heartbeat())
        return
    if args.serve_http:
        serve_http(args.host, args.port)
        return
    try:
        asyncio.run(BuddyBridge().run())
    except KeyboardInterrupt:
        print("\n[bridge] stopped")


if __name__ == "__main__":
    main()
