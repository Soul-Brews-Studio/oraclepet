# /// script
# requires-python = ">=3.10"
# dependencies = ["aiohttp>=3.9"]
# ///
"""A pretend pet for the voice server: streams a WAV as one utterance, framed as VOICE.md says.

    uv run --script simpet.py speech.wav [--url ws://127.0.0.1:4801/pet] [--pet Claude-SIM] [--fast]
"""
from __future__ import annotations

import argparse
import asyncio
import json
import struct
import sys
import wave
from pathlib import Path

from aiohttp import ClientSession, WSMsgType

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "python"))
from oraclepet import protocol as P  # noqa: E402


def load16k(path: str) -> list[int]:
    w = wave.open(path)
    if w.getframerate() != 16000 or w.getsampwidth() != 2 or w.getnchannels() != 1:
        raise SystemExit("need a 16 kHz mono 16-bit WAV (afconvert -f WAVE -d LEI16@16000 -c 1 in.aiff out.wav)")
    n = w.getnframes()
    return list(struct.unpack(f"<{n}h", w.readframes(n)))


async def stream(url: str, pet: str, pcm: list[int], uid: int, fast: bool, per_frame: int = 4) -> dict | None:
    blocks = P.psnd_encode(pcm)[16:]                       # drop the header: the wire carries blocks
    async with ClientSession() as s, s.ws_connect(url) as ws:
        await ws.send_str(json.dumps({"type": "hello", "pet": pet, "project": "simpet", "fw": "sim", "sr": 16000, "codec": "ima-psnd"}))
        await ws.send_str(json.dumps({"type": "start", "id": uid, "preroll_ms": 300, "floor": 10}))
        step = 256 * per_frame                              # 4 blocks = 126 ms of audio per frame
        for off in range(0, len(blocks), step):
            await ws.send_bytes(blocks[off:off + step])
            if not fast:
                await asyncio.sleep(per_frame * 504 / 16000)
        await ws.send_str(json.dumps({"type": "end", "id": uid, "samples": len(pcm), "reason": "silence"}))
        try:                                                # wait a little for the transcript
            msg = await asyncio.wait_for(ws.receive(), 15)
            if msg.type == WSMsgType.TEXT:
                return json.loads(msg.data)
        except asyncio.TimeoutError:
            return None
    return None


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("wav")
    ap.add_argument("--url", default="ws://127.0.0.1:4801/pet")
    ap.add_argument("--pet", default="Claude-SIM")
    ap.add_argument("--id", type=int, default=1)
    ap.add_argument("--fast", action="store_true", help="do not pace the audio in real time")
    a = ap.parse_args()
    print(asyncio.run(stream(a.url, a.pet, load16k(a.wav), a.id, a.fast)))


if __name__ == "__main__":
    main()
