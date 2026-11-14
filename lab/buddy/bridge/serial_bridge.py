#!/usr/bin/env python3
"""
USB Serial bridge: connects Claude Code to buddy ESP32 via /dev/ttyACM0.
Zero firmware changes needed — buddy already reads JSON from Serial.

Usage:
  uv run --with pyserial serial_bridge.py
  uv run --with pyserial serial_bridge.py /dev/ttyACM1  # custom port
"""

import json
import os
import sys
import time
import threading
from datetime import datetime, timezone
from pathlib import Path

import serial

BAUD = 115200
DEFAULT_PORT = "/dev/ttyACM0"
HEARTBEAT_SEC = 5
CLAUDE_DIR = Path.home() / ".claude"
TRANSCRIPT_DIR = CLAUDE_DIR / "transcripts"
ACTIVE_WINDOW = 30


def count_sessions():
    now = time.time()
    total = 0
    running = 0
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
    return {"total": total, "running": running, "waiting": 0}


def make_time_sync():
    epoch = int(time.time())
    local = datetime.now().astimezone()
    tz_offset = int(local.utcoffset().total_seconds())
    return json.dumps({"time": [epoch, tz_offset]})


def make_heartbeat(sessions=None, msg="bridge connected"):
    s = sessions or {"total": 0, "running": 0, "waiting": 0}
    return json.dumps({
        "total": s["total"],
        "running": s["running"],
        "waiting": s["waiting"],
        "completed": False,
        "tokens": 0,
        "msg": msg[:23],
    })


def reader_thread(ser):
    """Read lines from ESP32 and print responses."""
    while True:
        try:
            line = ser.readline().decode("utf-8", errors="replace").strip()
            if line and line.startswith("{"):
                try:
                    msg = json.loads(line)
                    cmd = msg.get("cmd")
                    if cmd == "permission":
                        print(f"[buddy] PERMISSION id={msg.get('id')} decision={msg.get('decision')}")
                    else:
                        print(f"[buddy] {msg}")
                except json.JSONDecodeError:
                    pass
            elif line:
                print(f"[serial] {line}")
        except Exception:
            break


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_PORT

    print(f"[bridge] opening {port} @ {BAUD}")
    ser = serial.Serial(port, BAUD, timeout=1)
    print(f"[bridge] connected to {port}")

    t = threading.Thread(target=reader_thread, args=(ser,), daemon=True)
    t.start()

    # time sync
    ts = make_time_sync()
    print(f"[bridge] time sync: {ts}")
    ser.write((ts + "\n").encode())
    time.sleep(0.2)

    # initial heartbeat
    hb = make_heartbeat(msg="bridge online")
    ser.write((hb + "\n").encode())
    print("[bridge] heartbeat loop started (ctrl+c to stop)")

    try:
        while True:
            time.sleep(HEARTBEAT_SEC)
            sessions = count_sessions()
            if sessions["running"] > 0:
                msg = f"{sessions['running']} active"
            elif sessions["total"] > 0:
                msg = f"{sessions['total']} sessions"
            else:
                msg = "bridge connected"
            hb = make_heartbeat(sessions, msg)
            ser.write((hb + "\n").encode())
    except KeyboardInterrupt:
        print("\n[bridge] stopped")
    finally:
        ser.close()


if __name__ == "__main__":
    main()
