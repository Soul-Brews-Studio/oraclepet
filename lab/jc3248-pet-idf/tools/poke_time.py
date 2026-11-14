#!/usr/bin/env python3
"""poke_time.py — push the host's NTP-synced clock to the jc3248 pet over BLE.

The pet is BLE-only (no Wi-Fi), so it can't reach NTP itself. This connects to
the pet's Nordic UART Service (NUS) and writes a heartbeat line carrying the
current Unix epoch + local timezone offset; the pet stores it and ticks locally
off its own timer (so it keeps running between syncs). Re-sends every 60s to
correct drift.

Usage (no install needed):
    uv run --with bleak tools/poke_time.py              # auto-find Claude-*
    uv run --with bleak tools/poke_time.py Claude-ECC8  # by exact name
Or:  pip install bleak && python3 tools/poke_time.py

macOS: the terminal needs Bluetooth permission (System Settings > Privacy).
"""
import asyncio
import json
import sys
import time

from bleak import BleakScanner, BleakClient

NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"   # client -> device, WRITE
NAME_PREFIX = sys.argv[1] if len(sys.argv) > 1 else "Claude-"
PERIOD_S = 60


def heartbeat_line() -> bytes:
    """One newline-terminated JSON line the pet's ble.c parseLine() understands."""
    now = time.time()
    lt = time.localtime(now)
    # local offset from UTC in seconds (handles DST). GMT+7 -> 25200.
    tzoff = -time.timezone + (3600 if lt.tm_isdst else 0)
    payload = {"epoch": int(now), "tzoff": int(tzoff)}
    return (json.dumps(payload) + "\n").encode()


async def main() -> None:
    print(f"scanning for '{NAME_PREFIX}*' (10s)…")
    dev = await BleakScanner.find_device_by_filter(
        lambda d, ad: bool(d.name) and d.name.startswith(NAME_PREFIX),
        timeout=10.0,
    )
    if not dev:
        print(f"!! no device whose name starts with '{NAME_PREFIX}' — is the pet advertising?")
        return
    print(f"connecting {dev.name} ({dev.address})…")
    async with BleakClient(dev) as client:
        print("connected — sending time every", PERIOD_S, "s (Ctrl-C to stop)")
        while True:
            data = heartbeat_line()
            await client.write_gatt_char(NUS_RX, data, response=False)
            print("→", data.decode().strip())
            await asyncio.sleep(PERIOD_S)


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nbye")
