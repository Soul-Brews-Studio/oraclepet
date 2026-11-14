#!/usr/bin/env python3
"""usage_bridge.py — feed the jc3248-pet REAL Claude usage over BLE.

No hardcoding: the numbers come straight from Claude Code. Claude Code writes a
statusline JSON every render that includes the live rate limits:

    "rate_limits": {
      "five_hour": { "used_percentage": 6,  "resets_at": <epoch> },
      "seven_day": { "used_percentage": 25, "resets_at": <epoch> }
    }

Our statusline command (~/.claude/statusline-command.sh) dumps that JSON to
/tmp/claude-statusline/raw.json. This bridge reads it, turns resets_at into
seconds-until-reset, and streams {pct5h,reset5h,pct7d,reset7d} to the pet's
Nordic UART every few seconds — so the pet HUD shows LIVE usage and the link
dot stays green. Reconnects automatically if the pet drops.

    /tmp/blevenv/bin/python tools/usage_bridge.py            # stream forever
    /tmp/blevenv/bin/python tools/usage_bridge.py --once     # send one snapshot and exit

macOS: the terminal running this needs Bluetooth permission.
"""
import asyncio, json, time, sys
from bleak import BleakScanner, BleakClient

RX     = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"   # NUS RX (write: us -> pet)
RAW    = "/tmp/claude-statusline/raw.json"        # Claude Code's statusline JSON dump
PERIOD = 8.0                                       # seconds between heartbeats (pet stale = 30s)
PREFIX = "Claude"


def build_snapshot():
    """Read Claude Code's live rate_limits → a pet heartbeat. None if unavailable."""
    try:
        d = json.load(open(RAW))
    except Exception:
        return None
    rl = d.get("rate_limits", {}) or {}
    fh = rl.get("five_hour", {}) or {}
    sd = rl.get("seven_day", {}) or {}
    now = int(time.time())
    lt  = time.localtime(now)
    tzoff = -time.timezone + (3600 if lt.tm_isdst else 0)   # local UTC offset (secs); GMT+7 → 25200
    # epoch/tzoff feed the IDF pet's top-strip clock (it's BLE-only, can't NTP itself)
    snap = {"total": 1, "running": 0, "waiting": 0, "epoch": now, "tzoff": int(tzoff)}
    if "used_percentage" in fh:
        snap["pct5h"] = int(fh["used_percentage"])
        if fh.get("resets_at"):
            snap["reset5h"] = max(0, int(fh["resets_at"]) - now)
    if "used_percentage" in sd:
        snap["pct7d"] = int(sd["used_percentage"])
        if sd.get("resets_at"):
            snap["reset7d"] = max(0, int(sd["resets_at"]) - now)
    cw = d.get("context_window", {}) or {}
    if "used_percentage" in cw:
        snap["msg"] = f"ctx {int(cw['used_percentage'])}%"
    # Always send: the heartbeat now carries the clock (epoch/tzoff) even before
    # rate-limit data is available, so the pet's time stays synced regardless.
    return snap


async def main():
    once = "--once" in sys.argv
    print("usage_bridge — streaming REAL Claude rate_limits to the pet")
    while True:
        print(f"scanning for '{PREFIX}*' ...")
        dev = await BleakScanner.find_device_by_filter(
            lambda d, a: (d.name or "").startswith(PREFIX), timeout=15.0)
        if not dev:
            print("no Claude-* device — pet powered & advertising? retrying...")
            if once: return
            await asyncio.sleep(3); continue
        try:
            async with BleakClient(dev) as c:
                print(f"connected to {dev.name} — link should go GREEN")
                while True:
                    snap = build_snapshot()
                    if snap:
                        await c.write_gatt_char(RX, (json.dumps(snap) + "\n").encode(), response=False)
                        print(f"  -> 5h {snap.get('pct5h','?')}%  7d {snap.get('pct7d','?')}%"
                              f"  reset5h={snap.get('reset5h','?')}s reset7d={snap.get('reset7d','?')}s")
                    else:
                        print(f"  (no rate_limits in {RAW} yet — render the statusline once)")
                    if once:
                        await asyncio.sleep(0.8); return
                    await asyncio.sleep(PERIOD)
        except Exception as e:
            print("link dropped:", e, "— reconnecting")
            if once: return
            await asyncio.sleep(2)


try:
    asyncio.run(main())
except KeyboardInterrupt:
    print("\nstopped.")
