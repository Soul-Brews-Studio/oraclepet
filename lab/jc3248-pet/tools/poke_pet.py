#!/usr/bin/env python3
"""poke_pet.py — pretend to be the Claude desktop.

Connects to the jc3248-pet over BLE Nordic UART and streams heartbeat snapshots
so bufo visibly cycles idle -> busy -> attention -> sleep. Proves the whole
bridge end-to-end WITHOUT the real desktop app.

    pip install bleak
    python3 poke_pet.py            # cycle the states forever, 4s each
    python3 poke_pet.py busy       # send ONE snapshot (idle|busy|attention|sleep) and exit

macOS: grant your TERMINAL Bluetooth permission first, or the scan finds nothing:
    System Settings -> Privacy & Security -> Bluetooth -> enable your terminal app
"""
import asyncio, json, sys
from bleak import BleakScanner, BleakClient

NUS_RX      = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"   # write here: desktop -> device
NAME_PREFIX = "Claude"                                 # the pet advertises as "Claude-XXXX"

# Usage windows (optional fields the pet shows on its HUD):
#   pct5h / pct7d   = usage percent of the 5h and 7d (weekly) windows
#   reset5h / reset7d = seconds until each window resets
# HUD renders "5h 8% 3h55m" / "wk 41% 4d23h". Omit them and the line just won't show.
# Sample matches the dashboard photo: 8% / ~3h55m  and  41% / ~4d23h.
_R5, _R7 = 3 * 3600 + 55 * 60, 4 * 86400 + 23 * 3600
_P5, _P7 = 8, 41
_U = {"reset5h": _R5, "reset7d": _R7, "pct5h": _P5, "pct7d": _P7}
SNAP = {
    "idle":      {"total": 1, "running": 0, "waiting": 0, "msg": "idle",                  "tokens": 1000, **_U},
    "busy":      {"total": 1, "running": 1, "waiting": 0, "msg": "running: Bash",         "tokens": 1200, **_U},
    "attention": {"total": 1, "running": 0, "waiting": 1, "msg": "approve: rm -rf /tmp/x","tokens": 1500, **_U},
    "sleep":     {"total": 0, "running": 0, "waiting": 0, "msg": "all done",              "tokens": 1500, **_U},
}
ORDER = ["idle", "busy", "attention", "sleep"]


async def main():
    one = sys.argv[1] if len(sys.argv) > 1 else None
    # raw send: `poke_pet.py raw '{"pct5h":50,"reset5h":1800}'` → write that JSON verbatim.
    raw = None
    if one == "raw":
        raw = sys.argv[2] if len(sys.argv) > 2 else "{}"
        json.loads(raw)  # validate; raises if bad
    print(f"scanning for '{NAME_PREFIX}*' (15s)...")
    dev = await BleakScanner.find_device_by_filter(
        lambda d, a: (d.name or "").startswith(NAME_PREFIX), timeout=15.0)
    if not dev:
        print("no Claude-* device found — is the pet powered and advertising?")
        return
    print(f"found {dev.name} [{dev.address}] — connecting...")
    async with BleakClient(dev) as c:
        print("connected — watch the HUD dot turn GREEN and bufo react.")

        async def send(key):
            line = (json.dumps(SNAP[key]) + "\n").encode()
            await c.write_gatt_char(NUS_RX, line, response=False)
            print(f"  -> {key:9s} {SNAP[key]['msg']}")

        if raw is not None:                       # raw send: write the JSON verbatim
            await c.write_gatt_char(NUS_RX, (raw + "\n").encode(), response=False)
            print(f"  -> raw {raw}")
            await asyncio.sleep(1.0)
            return
        if one:
            await send(one if one in SNAP else "busy")
            await asyncio.sleep(1.0)
            return
        i = 0
        while True:
            await send(ORDER[i % len(ORDER)])
            i += 1
            await asyncio.sleep(4.0)


try:
    asyncio.run(main())
except KeyboardInterrupt:
    print("\nstopped.")
