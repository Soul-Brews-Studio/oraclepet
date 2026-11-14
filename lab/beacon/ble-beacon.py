# /// script
# requires-python = ">=3.11"
# dependencies = [
#     "bless>=0.2.6",
# ]
# ///
"""
ble-beacon — advertise a text string as a BLE peripheral on macOS.

The ESP32 firmware (lab/beacon/beacon-display.yaml v2 — BLE variant)
scans for advertisements from this beacon, extracts the manufacturer
data, and renders the text in big letters.

Range: ~10m line-of-sight (BLE class 2). No internet, no broker.

Usage:
  uv run ble-beacon.py "hello catlab"
  uv run ble-beacon.py "stream of consciousness"

Env:
  BLE_NAME     advertised peripheral name (default "oracle-beacon")
  BLE_COMPANY  manufacturer ID hex (default 0xFFFF — testing range)
  BLE_DURATION seconds to advertise (default forever; ctrl-c to stop)
"""

import asyncio
import logging
import os
import signal
import sys

from bless import BlessServer

LOG = logging.getLogger("ble-beacon")

BLE_NAME = os.environ.get("BLE_NAME", "oracle-beacon")
BLE_COMPANY = int(os.environ.get("BLE_COMPANY", "0xFFFF"), 16)


async def advertise(text: str, duration: float | None) -> None:
    """Advertise `text` via BLE local name, prefixed with 'oracle:'.

    macOS CoreBluetooth restricts outbound advertising to LocalName +
    ServiceUUIDs (no manuf data). bless calls into that, so encoding the
    payload in the name is the only viable path.

    Max name length is ~20 chars after the "oracle:" prefix (BLE adv packet
    is 31 bytes total with overhead).
    """
    full_name = f"oracle:{text}"
    if len(full_name) > 28:
        LOG.warning("name truncated to 28 chars (BLE adv limit): %r", full_name[:28])
        full_name = full_name[:28]
    LOG.info("advertising local_name=%r (%d chars)", full_name, len(full_name))

    server = BlessServer(name=full_name)
    await server.start()
    LOG.info("advertising started — ctrl-c or SIGTERM to stop")

    # Make SIGTERM behave like SIGINT: trigger the finally below.
    # Without this, `kill <pid>` skips cleanup and macOS bluetoothd
    # keeps replaying the dead advertisement until BT is cycled.
    stop_event = asyncio.Event()
    loop = asyncio.get_running_loop()
    for sig in (signal.SIGTERM, signal.SIGINT):
        try:
            loop.add_signal_handler(sig, stop_event.set)
        except NotImplementedError:
            pass  # Windows / restricted environments

    try:
        if duration:
            try:
                await asyncio.wait_for(stop_event.wait(), timeout=duration)
            except asyncio.TimeoutError:
                pass  # duration elapsed normally
        else:
            await stop_event.wait()
    finally:
        await server.stop()
        LOG.info("advertising stopped")


def main() -> int:
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(name)s %(message)s")

    if len(sys.argv) < 2:
        print(__doc__.strip(), file=sys.stderr)
        return 2

    text = " ".join(sys.argv[1:])
    duration_env = os.environ.get("BLE_DURATION")
    duration = float(duration_env) if duration_env else None

    try:
        asyncio.run(advertise(text, duration))
    except KeyboardInterrupt:
        LOG.info("interrupted")
    return 0


if __name__ == "__main__":
    sys.exit(main())
