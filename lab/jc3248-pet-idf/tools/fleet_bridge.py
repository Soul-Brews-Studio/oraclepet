# /// script
# requires-python = ">=3.11"
# dependencies = ["bleak>=0.22"]
# ///
"""fleet_bridge.py — the desk pet follows the herdr agent fleet, over BLE.

usage_bridge.py always sends running=0 waiting=0 total=1, so the real pet never leaves idle.
This bridge polls `herdr agent list` and turns the fleet into the heartbeat the firmware
already understands (pet.cpp computeDesired: waiting>0 -> attention, running>0 -> busy,
total==0 -> sleep, else idle):

    herdr "working"  -> running   -> busy
    herdr "blocked"  -> waiting   -> attention   (an agent is waiting on a human)
    nothing working, but some agent was busy in the last QUIET_S -> idle
    the whole fleet quiet for QUIET_S -> total=0 -> sleep

It also carries what usage_bridge.py sent (clock epoch/tzoff + 5h/7d usage from the Claude
statusline dump), so it REPLACES pet-usage: the pet takes one BLE connection at a time.

    uv run tools/fleet_bridge.py              # stream forever (reconnects if the pet drops)
    uv run tools/fleet_bridge.py --once       # send one heartbeat and exit
    uv run tools/fleet_bridge.py --dry-run    # print heartbeats, no Bluetooth

macOS: run it from a terminal that has Bluetooth permission (see ecosystem.config.cjs).
"""
import asyncio
import json
import subprocess
import sys
import time

RX      = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"   # NUS RX (write: us -> pet)
RAW     = "/tmp/claude-statusline/raw.json"        # Claude Code's statusline JSON dump
PERIOD  = 5.0                                      # seconds between heartbeats (pet stale = 30s)
QUIET_S = 300                                      # fleet idle this long -> the pet sleeps
PREFIX  = "Claude"

last_busy: dict[str, float] = {}                   # pane id -> last time it was working/blocked


def fleet():
    """(running, waiting, total, msg) from `herdr agent list`. None if herdr is unavailable."""
    try:
        out = subprocess.run(["herdr", "agent", "list"], capture_output=True, text=True, timeout=5).stdout
        agents = json.loads(out)["result"]["agents"]
    except Exception:
        return None
    now = time.time()
    running = waiting = 0
    waiting_names = []
    for a in agents:
        st, pane = a.get("agent_status"), a.get("pane_id", "?")
        if st == "working":
            running += 1
            last_busy[pane] = now
        elif st == "blocked":
            waiting += 1
            last_busy[pane] = now
            waiting_names.append(a.get("name") or a.get("cwd", "").rstrip("/").split("/")[-1] or pane)
    awake = sum(1 for t in last_busy.values() if now - t < QUIET_S)
    if waiting_names:
        msg = "wait: " + ", ".join(waiting_names)[:28]
    elif running:
        msg = f"{running} working"
    else:
        msg = "fleet idle" if awake else "fleet asleep"
    return running, waiting, awake, msg


def usage(snap):
    """Add clock + 5h/7d usage, same fields as usage_bridge.py."""
    now = int(time.time())
    lt = time.localtime(now)
    snap["epoch"] = now
    snap["tzoff"] = int(-time.timezone + (3600 if lt.tm_isdst else 0))
    try:
        d = json.load(open(RAW))
    except Exception:
        return snap
    rl = d.get("rate_limits", {}) or {}
    for key, pct, reset in (("five_hour", "pct5h", "reset5h"), ("seven_day", "pct7d", "reset7d")):
        w = rl.get(key, {}) or {}
        if "used_percentage" in w:
            snap[pct] = int(w["used_percentage"])
            if w.get("resets_at"):
                snap[reset] = max(0, int(w["resets_at"]) - now)
    return snap


def build_snapshot():
    f = fleet()
    if f is None:
        return None
    running, waiting, total, msg = f
    return usage({"total": total, "running": running, "waiting": waiting, "msg": msg})


def show(snap):
    state = ("attention" if snap["waiting"] else "busy" if snap["running"]
             else "sleep" if snap["total"] == 0 else "idle")
    return (f"{state:9} run={snap['running']} wait={snap['waiting']} total={snap['total']}"
            f"  '{snap['msg']}'  5h={snap.get('pct5h', '?')}%")


async def stream(once):
    from bleak import BleakClient, BleakScanner
    while True:
        print(f"scanning for '{PREFIX}*' ...")
        dev = await BleakScanner.find_device_by_filter(
            lambda d, a: (d.name or "").startswith(PREFIX), timeout=15.0)
        if not dev:
            print("no Claude-* device — pet powered and advertising? (stop pet-usage first) retrying...")
            if once:
                return 1
            await asyncio.sleep(3)
            continue
        try:
            async with BleakClient(dev) as c:
                print(f"connected to {dev.name}")
                while True:
                    snap = build_snapshot()
                    if snap:
                        await c.write_gatt_char(RX, (json.dumps(snap) + "\n").encode(), response=False)
                        print("  ->", show(snap), flush=True)
                    else:
                        print("  (herdr agent list failed — is herdr running?)", flush=True)
                    if once:
                        await asyncio.sleep(0.8)
                        return 0
                    await asyncio.sleep(PERIOD)
        except Exception as e:
            print("link dropped:", e, "— reconnecting", flush=True)
            if once:
                return 1
            await asyncio.sleep(2)


def main():
    if "--dry-run" in sys.argv:
        for _ in range(1 if "--once" in sys.argv else 10**9):
            snap = build_snapshot()
            print(show(snap) if snap else "herdr agent list failed", json.dumps(snap), flush=True)
            if "--once" in sys.argv:
                return 0
            time.sleep(PERIOD)
    return asyncio.run(stream("--once" in sys.argv))


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\nstopped.")
