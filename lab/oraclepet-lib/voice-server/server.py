# /// script
# requires-python = ">=3.10"
# dependencies = ["aiohttp>=3.9", "zeroconf>=0.130", "numpy", "onnxruntime", "mlx-whisper"]
# ///
"""OraclePet voice server: the pets' utterances over WiFi, saved on this Mac (VOICE.md).

    uv run --script server.py            # ws://<this mac>:4801/pet, page at http://127.0.0.1:4801/

Each utterance becomes <root>/<YYYY-MM-DD>/<HHMMSS>-<pet>-<id>.wav and .json, and one line in
<root>/log.jsonl (root: ~/OraclePet/voice-log, or $ORACLEPET_VOICE_DIR). Silero VAD (ear.py)
checks it for speech; only the speech is transcribed, locally with Whisper on the GPU
(ORACLEPET_STT=local, the default) or by Paxa STT (ORACLEPET_STT=paxa, needs the Keychain key).
The text goes back to the pet and into the .json; log.jsonl gets {"update": <file>, "text": ...}.
An utterance without speech (a fan, a door) is kept but never transcribed: it costs nothing.
"""
from __future__ import annotations

import asyncio
import base64
import datetime as dt
import json
import logging
import os
import subprocess
import sys
import time
from pathlib import Path

from aiohttp import ClientSession, ClientTimeout, WSMsgType, web

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "python"))
from oraclepet import protocol as P  # noqa: E402  (PSND decode and WAV, shared with the library)

BLOCK = P.BLOCK_BYTES if hasattr(P, "BLOCK_BYTES") else 256
BLOCK_SMP = 504
MAX_SAMPLES = 16000 * 31                       # 30 s cap on the pet, plus slack
ROOT = Path(os.environ.get("ORACLEPET_VOICE_DIR", Path.home() / "OraclePet" / "voice-log"))
PORT = int(os.environ.get("ORACLEPET_VOICE_PORT", "4801"))
STT_URL = os.environ.get("ORACLEPET_STT_URL", "https://api.paxalabs.com/v1/stt")
STT_ENGINE = os.environ.get("ORACLEPET_STT", "local")
MIN_SPEECH_S = 0.3                            # less than this much speech: nothing to transcribe
log = logging.getLogger("voice")


def paxa_key() -> str | None:
    """The Paxa API key from the Keychain (never logged, never written)."""
    try:
        k = subprocess.run(["security", "find-generic-password", "-s", "co.laris.oraclepet.paxa", "-w"],
                           capture_output=True, text=True, timeout=5).stdout.strip()
        return k or None
    except (OSError, subprocess.SubprocessError):
        return None


def decode_blocks(data: bytes, samples: int) -> list[int]:
    """Whole PSND blocks (any split) -> exactly `samples` samples."""
    header = b"PSND" + bytes([1, 1, 0, 0]) + (16000).to_bytes(4, "little") + samples.to_bytes(4, "little")
    return P.psnd_decode(header + data) or []


def rms_peak(pcm: list[int]) -> tuple[int, int]:
    if not pcm:
        return 0, 0
    return int((sum(s * s for s in pcm) / len(pcm)) ** 0.5), max(abs(s) for s in pcm)


class Utterance:
    def __init__(self, pet: str, uid: int, meta: dict):
        self.pet, self.id, self.meta = pet, uid, meta
        self.started = dt.datetime.now().astimezone()
        self.audio = bytearray()


class Store:
    def __init__(self, root: Path):
        self.root = root
        self.root.mkdir(parents=True, exist_ok=True)

    def save(self, u: Utterance, end: dict, reason: str) -> dict:
        whole = len(u.audio) // BLOCK * BLOCK
        expected = end.get("samples")
        # the exact count from "end" when it fits what arrived; else every whole block
        most = (whole // BLOCK) * BLOCK_SMP
        samples = min(int(expected), most) if isinstance(expected, int) and expected >= 0 else most
        samples = min(samples, MAX_SAMPLES)
        pcm = decode_blocks(bytes(u.audio[:whole]), samples)
        rms, peak = rms_peak(pcm)
        day = self.root / u.started.strftime("%Y-%m-%d")
        day.mkdir(parents=True, exist_ok=True)
        safe_pet = "".join(c for c in u.pet if c.isalnum() or c in "-_") or "pet"
        stem = f"{u.started.strftime('%H%M%S')}-{safe_pet}-{u.id}"
        (day / f"{stem}.wav").write_bytes(P.wav(pcm))
        rec = {
            "file": f"{day.name}/{stem}.wav", "pet": u.pet, "id": u.id,
            "start": u.started.isoformat(timespec="milliseconds"),
            "end": dt.datetime.now().astimezone().isoformat(timespec="milliseconds"),
            "samples": len(pcm), "seconds": round(len(pcm) / 16000, 2), "reason": reason,
            "rms": rms, "peak": peak, "floor": u.meta.get("floor"),
            "pet_rms": end.get("rms"), "pet_peak": end.get("peak"), "text": None,
        }
        (day / f"{stem}.json").write_text(json.dumps(rec, ensure_ascii=False, indent=1), encoding="utf-8")
        self._append(rec)
        return rec

    def update_text(self, rec: dict, text: str, engine: str, ms: int, speech_s: float | None = None) -> None:
        rec.update(text=text, stt=engine, stt_ms=ms)
        if speech_s is not None:
            rec["speech_s"] = round(speech_s, 2)
        (self.root / rec["file"]).with_suffix(".json").write_text(json.dumps(rec, ensure_ascii=False, indent=1), encoding="utf-8")
        self._append({"update": rec["file"], "text": text, "stt": engine, "stt_ms": ms, "speech_s": rec.get("speech_s")})

    def _append(self, line: dict) -> None:
        with open(self.root / "log.jsonl", "a", encoding="utf-8") as f:
            f.write(json.dumps(line, ensure_ascii=False) + "\n")

    def listing(self, limit: int = 200) -> list[dict]:
        """Utterances newest first, with their latest text."""
        path = self.root / "log.jsonl"
        if not path.exists():
            return []
        recs: dict[str, dict] = {}
        for line in path.read_text(encoding="utf-8").splitlines():
            try:
                o = json.loads(line)
            except ValueError:
                continue
            if "update" in o:
                if o["update"] in recs:
                    recs[o["update"]].update(text=o.get("text"), stt=o.get("stt"), stt_ms=o.get("stt_ms"),
                                             speech_s=o.get("speech_s"))
            elif "file" in o:
                recs[o["file"]] = o
        return sorted(recs.values(), key=lambda r: r.get("start", ""), reverse=True)[:limit]


async def transcribe(wav: bytes, key: str, session: ClientSession) -> tuple[str, int]:
    t = time.monotonic()
    async with session.post(STT_URL, headers={"Authorization": f"Bearer {key}"},
                            json={"audio": base64.b64encode(wav).decode(), "model": "paxa-stt-lite-v1-preview",
                                  "language": "th"}) as r:
        body = await r.json(content_type=None)
        if r.status != 200:
            raise RuntimeError(f"paxa stt {r.status}: {str(body)[:160]}")
    return str(body.get("text", "")).strip(), int((time.monotonic() - t) * 1000)


def read_wav(path: Path) -> list[int]:
    import struct
    import wave
    with wave.open(str(path)) as w:
        n = w.getnframes()
        return list(struct.unpack(f"<{n}h", w.readframes(n)))


class VoiceServer:
    def __init__(self, store: Store, key_source=paxa_key, ear=None, engine: str = STT_ENGINE):
        self.store = store
        self.key_source = key_source
        self.ear = ear
        self.engine = engine
        self.feeds: set[web.WebSocketResponse] = set()          # browsers watching the page
        self.session: ClientSession | None = None

    async def pet_ws(self, request: web.Request) -> web.WebSocketResponse:
        ws = web.WebSocketResponse(heartbeat=20, max_msg_size=1 << 20)
        await ws.prepare(request)
        pet, cur = request.remote or "pet", None
        log.info("pet connected from %s", request.remote)
        try:
            async for msg in ws:
                if msg.type == WSMsgType.TEXT:
                    try:
                        o = json.loads(msg.data)
                    except ValueError:
                        log.warning("%s: not JSON, ignored", pet)
                        continue
                    kind = o.get("type")
                    if kind == "hello":
                        pet = str(o.get("pet") or pet)[:40]
                        log.info("hello from %s (%s %s)", pet, o.get("project"), o.get("fw"))
                    elif kind == "start":
                        if cur is not None:                              # a start without end: keep it
                            await self.finish(ws, cur, {}, "cut")
                        cur = Utterance(pet, int(o.get("id") or 0), o)
                    elif kind == "end" and cur is not None:
                        if o.get("reason") == "short":             # under 250 ms of speech: noise, not kept
                            log.info("%s #%s: too short, dropped", pet, o.get("id"))
                        else:
                            await self.finish(ws, cur, o, str(o.get("reason") or "silence"))
                        cur = None
                elif msg.type == WSMsgType.BINARY:
                    if cur is None:
                        continue                                          # audio outside an utterance
                    if len(cur.audio) + len(msg.data) <= (MAX_SAMPLES // BLOCK_SMP + 1) * BLOCK:
                        cur.audio += msg.data
                elif msg.type == WSMsgType.ERROR:
                    break
        finally:
            if cur is not None and cur.audio:
                await self.finish(None, cur, {}, "cut")                    # disconnected mid-utterance
            log.info("%s disconnected", pet)
        return ws

    async def finish(self, ws, u: Utterance, end: dict, reason: str) -> None:
        rec = self.store.save(u, end, reason)
        log.info("%s #%d: %.1f s (%s) -> %s", u.pet, u.id, rec["seconds"], reason, rec["file"])
        await self.broadcast(rec)
        if rec["samples"] >= 4000:                                        # 0.25 s or more
            asyncio.create_task(self.stt(ws, rec))

    async def stt(self, ws, rec: dict) -> None:
        loop = asyncio.get_running_loop()
        t = time.monotonic()
        regions, speech_s = None, None
        try:
            pcm = read_wav(self.store.root / rec["file"])
            if self.ear is not None:                                      # speech at all? (a fan is not)
                regions = await loop.run_in_executor(None, self.ear.speech, pcm)
                speech_s = sum(b - a for a, b in regions)
                if speech_s < MIN_SPEECH_S:
                    self.store.update_text(rec, "", "none: no speech", int((time.monotonic() - t) * 1000), speech_s)
                    log.info("%s: no speech (%.2f s), not transcribed", rec["file"], speech_s)
                    await self.broadcast(rec)
                    return
            if self.engine == "paxa":
                key = self.key_source()
                if not key:
                    return
                self.session = self.session or ClientSession(timeout=ClientTimeout(total=30))
                text, ms = await transcribe((self.store.root / rec["file"]).read_bytes(), key, self.session)
                engine = "paxa"
            elif self.ear is not None:
                text = await loop.run_in_executor(None, self.ear.words, pcm, regions or [(0.0, len(pcm) / 16000)])
                ms, engine = int((time.monotonic() - t) * 1000), "whisper (local)"
            else:
                return
        except Exception as e:                                            # a failed STT leaves text empty
            log.warning("stt failed for %s: %s", rec["file"], str(e)[:160])
            return
        self.store.update_text(rec, text, engine, ms, speech_s)
        log.info("%s (%s, %d ms): %s", rec["file"], engine, ms, text)
        await self.broadcast(rec)
        if ws is not None and not ws.closed:
            await ws.send_str(json.dumps({"type": "heard", "id": rec["id"], "text": text}, ensure_ascii=False))

    async def broadcast(self, rec: dict) -> None:
        for f in list(self.feeds):
            try:
                await f.send_str(json.dumps(rec, ensure_ascii=False))
            except ConnectionError:
                self.feeds.discard(f)

    async def feed_ws(self, request: web.Request) -> web.WebSocketResponse:
        ws = web.WebSocketResponse(heartbeat=30)
        await ws.prepare(request)
        self.feeds.add(ws)
        try:
            async for _ in ws:
                pass
        finally:
            self.feeds.discard(ws)
        return ws

    # transcriber-oracle compatibility: its /live-meeting, /find-in-meeting and /summarize-meeting
    # skills read GET /health and GET /api/transcript.md?session=<id> from its Discord voice bot;
    # the same two here let them read what the pets heard. A session = one day of one pet.
    async def health(self, request: web.Request) -> web.Response:
        recs = self.store.listing(1)
        now = dt.datetime.now().astimezone()
        last = recs[0] if recs else None
        speaking = bool(last) and (now - dt.datetime.fromisoformat(last["end"])).total_seconds() < 10
        return web.json_response({"ok": True, "channel": "OraclePet", "speaking": speaking,
                                  "sessionId": f"{last['start'][:10]}-{last['pet']}" if last else None})

    async def transcript_md(self, request: web.Request) -> web.Response:
        session = request.query.get("session", "")
        day, _, pet = session.partition("-Claude") if "-Claude" in session else (session[:10], "", "")
        pet = ("Claude" + pet) if pet else ""
        recs = [r for r in reversed(self.store.listing(100000))
                if r.get("text") and r["start"][:10] == (day or dt.date.today().isoformat())
                and (not pet or r["pet"] == pet)]
        lines = [f"# OraclePet transcript · {session or day}", ""]
        lines += [f"{r['start'][11:19]} [{r['pet']}] {r['text']}" for r in recs]
        return web.Response(text="\n".join(lines) + "\n", content_type="text/markdown", charset="utf-8")

    async def list_json(self, request: web.Request) -> web.Response:
        return web.json_response(self.store.listing(), dumps=lambda o: json.dumps(o, ensure_ascii=False))

    async def audio(self, request: web.Request) -> web.StreamResponse:
        rel = request.match_info["path"]
        f = (self.store.root / rel).resolve()
        if not str(f).startswith(str(self.store.root.resolve())) or f.suffix != ".wav" or not f.is_file():
            raise web.HTTPNotFound()
        return web.FileResponse(f)

    async def page(self, request: web.Request) -> web.FileResponse:
        return web.FileResponse(Path(__file__).with_name("index.html"))

    def app(self) -> web.Application:
        a = web.Application()
        a.add_routes([web.get("/pet", self.pet_ws), web.get("/feed", self.feed_ws), web.get("/api/utterances", self.list_json),
                      web.get("/health", self.health), web.get("/api/transcript.md", self.transcript_md),
                      web.get("/audio/{path:.+}", self.audio), web.get("/", self.page)])

        async def close(_):
            if self.session:
                await self.session.close()
        a.on_cleanup.append(close)
        return a


async def announce(app: web.Application) -> None:
    """mDNS: _oraclepet._tcp on this Mac, so a pet on WiFi finds the server by itself (VOICE.md §2)."""
    import shutil
    import socket
    if sys.platform == "darwin" and shutil.which("dns-sd"):
        # macOS: register through mDNSResponder, which owns port 5353 and every interface.
        # (python-zeroconf next to it sent on the VPN interface: "No route to host".)
        name = f"{socket.gethostname().split('.')[0]} voice"
        app["dnssd"] = await asyncio.create_subprocess_exec(
            "dns-sd", "-R", name, "_oraclepet._tcp", "local", str(PORT), "path=/pet", "v=1",
            stdout=asyncio.subprocess.DEVNULL, stderr=asyncio.subprocess.DEVNULL)
        log.info("mDNS: _oraclepet._tcp '%s' port %d (macOS dns-sd)", name, PORT)
        return
    try:
        import socket
        from zeroconf import IPVersion, ServiceInfo
        from zeroconf.asyncio import AsyncZeroconf
    except ImportError:
        log.info("zeroconf not installed: pets need the URL sent over BLE")
        return
    import ipaddress
    import ifaddr                                    # comes with zeroconf
    host = socket.gethostname().split(".")[0]
    vpn = ipaddress.ip_network("100.64.0.0/10")      # Tailscale / NetBird: a pet on WiFi cannot reach it
    ips = sorted({a.ip for ad in ifaddr.get_adapters() for a in ad.ips
                  if isinstance(a.ip, str) and not ipaddress.ip_address(a.ip).is_loopback
                  and not ipaddress.ip_address(a.ip).is_link_local and ipaddress.ip_address(a.ip) not in vpn})
    if not ips:
        log.info("mDNS: no LAN address (WiFi off?); pets need the URL sent over BLE")
        return
    info = ServiceInfo("_oraclepet._tcp.local.", f"{host} voice._oraclepet._tcp.local.", port=PORT,
                       properties={"path": "/pet", "v": "1"}, server=f"{host}.local.",
                       parsed_addresses=ips)
    zc = AsyncZeroconf(ip_version=IPVersion.V4Only)
    await zc.async_register_service(info)
    app["zc"] = (zc, info)
    log.info("mDNS: _oraclepet._tcp %s:%d (%s)", host, PORT, ", ".join(ips))


async def unannounce(app: web.Application) -> None:
    if "dnssd" in app:
        app["dnssd"].terminate()
    if "zc" in app:
        zc, info = app["zc"]
        await zc.async_unregister_service(info)
        await zc.async_close()


def main() -> None:
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s", datefmt="%H:%M:%S")
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from ear import Ear
    srv = VoiceServer(Store(ROOT), ear=Ear())
    log.info("voice log: %s · pets: ws://<this mac>:%d/pet · page: http://127.0.0.1:%d/ · STT: %s",
             ROOT, PORT, PORT, "Paxa" if STT_ENGINE == "paxa" else "Silero + Whisper on this Mac")
    app = srv.app()

    async def warm(_app):                    # load Silero and Whisper now, not on the first utterance
        async def go():
            loop = asyncio.get_running_loop()
            t = time.monotonic()
            await loop.run_in_executor(None, srv.ear.speech, [0] * 16000)
            await loop.run_in_executor(None, srv.ear.words, [0] * 16000, [(0.0, 1.0)])
            log.info("ear ready: Silero + %s loaded in %.1f s", "Whisper", time.monotonic() - t)
        asyncio.create_task(go())
    app.on_startup.append(warm)
    if os.environ.get("ORACLEPET_VOICE_MDNS", "1") != "0":
        app.on_startup.append(announce)
        app.on_cleanup.append(unannounce)
    web.run_app(app, host="0.0.0.0", port=PORT, print=None)


if __name__ == "__main__":
    main()
