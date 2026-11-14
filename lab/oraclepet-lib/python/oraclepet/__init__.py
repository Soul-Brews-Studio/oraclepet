"""OraclePet host library: drive the oracle desk pet over Bluetooth LE (protocol v1).

    from oraclepet import Pet
    async with Pet("D350") as pet:
        print(await pet.info())
        print(await pet.say("hello"))

protocol.py is the wire format (pure Python, pinned by ../golden/vectors.json); pet.py is the
Bluetooth side (bleak) with the Mac app's transfer engine; tts.py is speech per OS.
"""
__version__ = "0.1.0"

from .pet import BleLink, Found, Link, Outcome, Pet, PetError, Progress, find_pet, scan
from .protocol import PROTOCOL_VERSION, Heartbeat, PetFirmware

__all__ = ["BleLink", "Found", "Heartbeat", "Link", "Outcome", "PROTOCOL_VERSION", "Pet", "PetError",
           "PetFirmware", "Progress", "find_pet", "scan", "__version__"]
