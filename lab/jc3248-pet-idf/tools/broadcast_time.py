# /// script
# requires-python = ">=3.11"
# dependencies = ["bless>=0.2.6"]
# ///
"""broadcast_time.py — CONNECTIONLESS time broadcast to the jc3248 pet over BLE.

The pet is BLE-only and now SCANS (observer role) for a time beacon. This advertises
the host's NTP-synced time in the BLE *local name* — "clk:<epoch>:<tzoff>" — with no
connection. The pet reads it off the advertisement and sets its clock; it then ticks
locally, so a refresh every couple of minutes is plenty (corrects drift).

Why the name (not manufacturer data): macOS CoreBluetooth only lets you advertise a
LocalName + ServiceUUIDs, so the payload rides the name — same trick as
lab/beacon/ble-beacon.py.

Run alongside usage_bridge.py (which keeps the connection for the usage windows):
    uv run tools/broadcast_time.py
macOS: the terminal needs Bluetooth permission. Ctrl-C / SIGTERM to stop cleanly.
"""
import asyncio
import signal
import time

from bless import BlessServer

REFRESH = 120.0   # seconds between re-advertising with a fresh epoch


def time_name() -> str:
    now = int(time.time())
    lt = time.localtime(now)
    tzoff = -time.timezone + (3600 if lt.tm_isdst else 0)   # local UTC offset, secs
    return f"clk:{now}:{int(tzoff)}"


async def main() -> None:
    stop = asyncio.Event()
    loop = asyncio.get_running_loop()
    for sig in (signal.SIGTERM, signal.SIGINT):
        try:
            loop.add_signal_handler(sig, stop.set)
        except NotImplementedError:
            pass

    print("broadcasting time (connectionless) — Ctrl-C to stop")
    while not stop.is_set():
        nm = time_name()
        server = BlessServer(name=nm)
        await server.start()
        print("→", nm)
        try:
            await asyncio.wait_for(stop.wait(), timeout=REFRESH)
        except asyncio.TimeoutError:
            pass            # refresh cycle: restart with a new epoch
        await server.stop()  # stop cleanly so macOS doesn't replay a stale advert
    print("stopped")


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nbye")
