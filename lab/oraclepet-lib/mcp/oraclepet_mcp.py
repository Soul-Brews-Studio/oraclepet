# /// script
# requires-python = ">=3.10"
# dependencies = ["mcp>=1.2,<2"]   # 2.x renamed FastMCP
# ///
"""oraclepet MCP server: an agent talks to the desk pets.

The pets keep one Bluetooth link each, and OraclePet.app holds them (macOS also denies
Bluetooth to a bare process started by an agent). So this server does not use Bluetooth: it
asks OraclePet with deskpet:// URLs and reads the answer from /tmp/deskpet-status.json, which
OraclePet rewrites within ~0.3 s of any change on a pet.

    claude mcp add oraclepet -- uv run --script lab/oraclepet-lib/mcp/oraclepet_mcp.py

macOS only (it needs OraclePet). On Linux/Windows use the Python library (../python) directly.
"""
from __future__ import annotations

import json
import os
import subprocess
import time
import urllib.parse

from mcp.server.fastmcp import FastMCP

STATUS = os.environ.get("ORACLEPET_STATUS", "/tmp/deskpet-status.json")
mcp = FastMCP("oraclepet")


def _status() -> dict:
    try:
        with open(STATUS, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def _running() -> bool:
    pid = _status().get("pid")
    if not pid:
        return False
    try:
        os.kill(int(pid), 0)
        return True
    except OSError:
        return False


def _ensure_app() -> None:
    """Start OraclePet if it is not running, and wait until it writes a fresh status."""
    if _running():
        return
    subprocess.run(["open", "-a", "OraclePet"], check=False)
    for _ in range(40):
        time.sleep(0.5)
        if _running():
            return
    raise RuntimeError("OraclePet is not running and did not start (is /Applications/OraclePet.app installed?)")


def _pet(suffix: str | None) -> dict:
    pets = _status().get("pets") or []
    if suffix:
        for p in pets:
            if (p.get("name") or "").upper().endswith(suffix.upper()):
                return p
        return {}
    return pets[0] if pets else {}


def _open(host: str, pet: str | None, **q) -> None:
    _ensure_app()
    if pet:
        q["pet"] = pet
    url = f"deskpet://{host}?" + urllib.parse.urlencode({k: v for k, v in q.items() if v is not None}, quote_via=urllib.parse.quote)   # %20, not +: Swift keeps a + as a +
    subprocess.run(["open", "-g", url], check=False)   # -g: do not bring OraclePet to the front


def _wait(pet: str | None, key: str, before, done, timeout: float) -> str | None:
    """Wait until pets[pet][key] changes from `before` and done(value) holds."""
    end = time.time() + timeout
    while time.time() < end:
        v = _pet(pet).get(key)
        if v != before and v is not None and done(str(v)):
            return str(v)
        time.sleep(0.25)
    return None


def _summary(p: dict) -> dict:
    keys = ("name", "link", "firmware", "pack", "app", "ota", "speakerTest", "mic", "micText", "micWav")
    return {k: p.get(k) for k in keys}


@mcp.tool()
def pet_status() -> dict:
    """Which pets OraclePet is linked to, their firmware, animation pack, running app, and the
    last speaker test, LISTEN result (with the words it heard) and job status."""
    _ensure_app()
    s = _status()
    return {"at": s.get("at"), "heartbeat": s.get("heartbeat"),
            "pets": [_summary(p) for p in s.get("pets") or []]}


@mcp.tool()
def pet_say(text: str, pet: str | None = None) -> str:
    """Speak through a pet's speaker. Thai by default: English text is translated on the Mac
    first. `pet`: name suffix like "D350" (2.8in board, has a mic) or "AD48" (3.5in); default
    the first pet. Returns when the pet has played it."""
    before = _pet(pet).get("ota")
    _open("say", pet, text=text)
    end = ("sound after", "refused", "stalled", "Link lost", "cut short", "failed")
    v = _wait(pet, "ota", before, lambda x: any(e in x for e in end), 60)
    return v or "no answer from the pet within 60 s"


@mcp.tool()
def pet_listen(ms: int = 3000, pet: str | None = "D350", language: str = "th-TH") -> dict:
    """Record a pet's microphone for `ms` (100..10000) and return what it heard: levels (rms,
    peak, strongest frequency), the words (Apple speech recognition in `language`, e.g. th-TH,
    en-US) and the WAV path on this Mac. Only the 2.8in board (D350) has a mic."""
    p0 = _pet(pet)
    _open("listen", pet, ms=max(100, min(10000, ms)), lang=language)
    mic = _wait(pet, "mic", p0.get("mic"),
                lambda x: x.startswith("Heard") or "no mic" in x or "could not" in x, ms / 1000 + 30)
    if not mic or not mic.startswith("Heard"):
        return {"error": mic or "no answer from the pet"}
    text = _wait(pet, "micText", None, lambda x: x != "…", 30)
    p = _pet(pet)
    return {"heard": mic, "text": text, "rms": p.get("micRms"), "floor": p.get("micFloor"),
            "freq": p.get("micFreq"), "db": p.get("micDb"), "wav": p.get("micWav")}


@mcp.tool()
def pet_selftest(pet: str | None = None) -> str:
    """The pet beeps 1 kHz; a pet with a mic (D350) checks that it heard its own speaker."""
    before = _pet(pet).get("speakerTest")
    _open("selftest", pet)
    v = _wait(pet, "speakerTest", before, lambda x: not x.startswith("Beeping"), 20)
    return v or "no answer from the pet within 20 s"


@mcp.tool()
def pet_show_pack(name: str, pet: str | None = None) -> str:
    """Show an animation pack on the pet (atlas, bufo, calliope, cat, cat-orange, cat-pet,
    clawd, iris, mother, neo, pulse, siamese, volt); sent first if the pet lacks it."""
    before = _pet(pet).get("ota")
    _open("pack", pet, name=name)
    v = _wait(pet, "ota", before, lambda x: "shows" in x or "failed" in x or "refused" in x, 180)
    return v or "no answer from the pet within 180 s"


@mcp.tool()
def pet_run_app(name: str, pet: str | None = None) -> str:
    """Run a wasm app that is already on the pet (see pet_status for the list); "stop" stops it."""
    before = _pet(pet).get("ota")
    _open("app", pet, run=name)
    v = _wait(pet, "ota", before, lambda x: "runs" in x or "stopped" in x or "failed" in x, 20)
    return v or "no answer from the pet within 20 s"


if __name__ == "__main__":
    mcp.run()
