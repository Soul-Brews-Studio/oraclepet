#!/usr/bin/env python3
"""
WiFi bridge: connects Claude Code to buddy ESP32 over TCP.

The ESP32 runs a TCP server on port 7654. This script connects,
sends heartbeat JSON, and receives permission responses.

Usage:
  python3 wifi_bridge.py                    # auto-discover via mDNS or scan
  python3 wifi_bridge.py 192.168.1.100      # direct IP
  python3 wifi_bridge.py 192.168.1.100 7654 # IP + port
"""

import json
import os
import socket
import sys
import time
import threading
from datetime import datetime, timezone
from pathlib import Path

PORT = 7654
HEARTBEAT_SEC = 5
TRANSCRIPT_DIR = Path.home() / ".claude" / "transcripts"
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


def find_buddy_ip():
    """Scan local subnet for ESP32 on port 7654."""
    import subprocess
    try:
        result = subprocess.run(["ip", "route", "get", "1.1.1.1"],
                                capture_output=True, text=True, timeout=3)
        for part in result.stdout.split():
            if part.startswith("192.168.") or part.startswith("10."):
                local_ip = part
                break
        else:
            return None

        subnet = ".".join(local_ip.split(".")[:3])
        print(f"[bridge] scanning {subnet}.0/24 for buddy on port {PORT}...")

        def try_connect(ip):
            try:
                s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                s.settimeout(0.3)
                s.connect((ip, PORT))
                s.close()
                return ip
            except (socket.timeout, ConnectionRefusedError, OSError):
                return None

        from concurrent.futures import ThreadPoolExecutor
        with ThreadPoolExecutor(max_workers=50) as pool:
            futures = {pool.submit(try_connect, f"{subnet}.{i}"): i for i in range(1, 255)}
            for fut in futures:
                result = fut.result()
                if result:
                    return result
    except Exception as e:
        print(f"[bridge] scan failed: {e}")
    return None


def reader_thread(sock):
    buf = ""
    while True:
        try:
            data = sock.recv(1024)
            if not data:
                print("[bridge] server closed connection")
                break
            buf += data.decode("utf-8", errors="replace")
            while "\n" in buf:
                line, buf = buf.split("\n", 1)
                line = line.strip()
                if not line or not line.startswith("{"):
                    continue
                try:
                    msg = json.loads(line)
                    cmd = msg.get("cmd")
                    if cmd == "permission":
                        print(f"[buddy] PERMISSION id={msg.get('id')} decision={msg.get('decision')}")
                    else:
                        print(f"[buddy] {msg}")
                except json.JSONDecodeError:
                    pass
        except Exception as e:
            print(f"[bridge] read error: {e}")
            break


def send(sock, text):
    try:
        sock.sendall((text + "\n").encode("utf-8"))
    except Exception as e:
        print(f"[bridge] send error: {e}")
        raise


def main():
    host = sys.argv[1] if len(sys.argv) > 1 else None
    port = int(sys.argv[2]) if len(sys.argv) > 2 else PORT

    while True:
        if not host:
            host = find_buddy_ip()
            if not host:
                print("[bridge] buddy not found, retry 5s...")
                time.sleep(5)
                continue

        print(f"[bridge] connecting to {host}:{port}...")
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(5)
            sock.connect((host, port))
            sock.settimeout(None)
            print(f"[bridge] connected to {host}:{port}")

            t = threading.Thread(target=reader_thread, args=(sock,), daemon=True)
            t.start()

            ts = make_time_sync()
            print(f"[bridge] time sync: {ts}")
            send(sock, ts)
            time.sleep(0.2)

            send(sock, make_heartbeat(msg="wifi bridge"))
            print("[bridge] heartbeat loop (ctrl+c to stop)")

            while True:
                time.sleep(HEARTBEAT_SEC)
                sessions = count_sessions()
                if sessions["running"] > 0:
                    msg = f"{sessions['running']} active"
                elif sessions["total"] > 0:
                    msg = f"{sessions['total']} sessions"
                else:
                    msg = "wifi connected"
                send(sock, make_heartbeat(sessions, msg))

        except (ConnectionRefusedError, socket.timeout, OSError) as e:
            print(f"[bridge] connection failed: {e}")
        except KeyboardInterrupt:
            print("\n[bridge] stopped")
            return
        finally:
            try:
                sock.close()
            except Exception:
                pass

        if len(sys.argv) <= 1:
            host = None
        print("[bridge] reconnect 5s...")
        time.sleep(5)


if __name__ == "__main__":
    main()
