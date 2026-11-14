"""Voice server tests: VOICE.md framing, files and log, STT (mocked), the page API.

    uv run --with aiohttp python -m unittest discover -s tests      (from voice-server/)
"""
import asyncio
import json
import math
import sys
import tempfile
import unittest
import wave
from pathlib import Path
from unittest import mock

from aiohttp.test_utils import AioHTTPTestCase

HERE = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "python"))
import server as S  # noqa: E402
from oraclepet import protocol as P  # noqa: E402


def tone(n, hz=440, amp=8000):
    return [int(amp * math.sin(2 * math.pi * hz * i / 16000)) for i in range(n)]


def blocks(pcm):
    return P.psnd_encode(pcm)[16:]


class FakeEar:
    """Stands in for ear.Ear (Silero + Whisper): says where the speech is and what was said."""
    def __init__(self):
        self.regions, self.text, self.heard = [(0.0, 0.5)], "สวัสดีครับ แมวออราเคิล", []

    def speech(self, pcm):
        return self.regions

    def words(self, pcm, regions):
        self.heard.append(regions)
        return self.text


class VoiceServerTests(AioHTTPTestCase):
    async def get_application(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.key = None
        self.ear = FakeEar()
        self.srv = S.VoiceServer(S.Store(Path(self.tmp.name)), key_source=lambda: self.key, ear=self.ear, engine="local")
        return self.srv.app()

    async def asyncTearDown(self):
        await super().asyncTearDown()
        self.tmp.cleanup()

    async def utter(self, pet, uid, pcm, frames, end=True, samples=None):
        ws = await self.client.ws_connect("/pet")
        await ws.send_str(json.dumps({"type": "hello", "pet": pet}))
        await ws.send_str(json.dumps({"type": "start", "id": uid, "floor": 9}))
        for f in frames(blocks(pcm)):
            await ws.send_bytes(f)
        if end:
            await ws.send_str(json.dumps({"type": "end", "id": uid, "samples": len(pcm) if samples is None else samples,
                                          "reason": "silence", "rms": 1, "peak": 2}))
        return ws

    async def settle(self):
        for _ in range(50):
            await asyncio.sleep(0.02)
            if (Path(self.tmp.name) / "log.jsonl").exists():
                return

    def records(self):
        return [json.loads(l) for l in (Path(self.tmp.name) / "log.jsonl").read_text(encoding="utf-8").splitlines()]

    def wav_samples(self, rec):
        w = wave.open(str(Path(self.tmp.name) / rec["file"]))
        return w.getnframes(), w.getframerate()

    async def test_exact_samples_with_a_partial_last_block(self):
        pcm = tone(1000)                                    # 504 + 496: the last block is partial
        ws = await self.utter("Claude-D350", 1, pcm, lambda b: [b])
        await self.settle(); await ws.close()
        r = self.records()[0]
        self.assertEqual((r["pet"], r["id"], r["samples"], r["reason"]), ("Claude-D350", 1, 1000, "silence"))
        self.assertEqual(self.wav_samples(r), (1000, 16000))
        self.assertEqual(P.psnd_decode(P.psnd_encode(pcm)), self._read(r))

    def _read(self, r):
        w = wave.open(str(Path(self.tmp.name) / r["file"]))
        import struct
        return list(struct.unpack(f"<{w.getnframes()}h", w.readframes(w.getnframes())))

    async def test_blocks_split_across_frames(self):
        pcm = tone(504 * 5 + 7)
        ws = await self.utter("Claude-D350", 2, pcm, lambda b: [b[i:i + 100] for i in range(0, len(b), 100)])
        await self.settle(); await ws.close()
        r = self.records()[0]
        self.assertEqual(r["samples"], len(pcm))
        self.assertEqual(self._read(r), P.psnd_decode(P.psnd_encode(pcm)))

    async def test_disconnect_mid_utterance_keeps_whole_blocks(self):
        pcm = tone(504 * 3)
        ws = await self.utter("Claude-D350", 3, pcm, lambda b: [b[:256 * 2 + 40]], end=False)
        await ws.close()
        await self.settle()
        r = self.records()[0]
        self.assertEqual((r["reason"], r["samples"]), ("cut", 504 * 2))

    async def test_two_pets_at_once(self):
        a = await self.utter("Claude-AD48", 1, tone(600), lambda b: [b])
        b = await self.utter("Claude-D350", 1, tone(900, 880), lambda b: [b])
        await self.settle(); await asyncio.sleep(0.1); await a.close(); await b.close()
        got = sorted((r["pet"], r["samples"]) for r in self.records())
        self.assertEqual(got, [("Claude-AD48", 600), ("Claude-D350", 900)])

    async def test_short_utterances_are_dropped(self):
        ws = await self.client.ws_connect("/pet")
        await ws.send_str(json.dumps({"type": "start", "id": 8}))
        await ws.send_bytes(blocks(tone(504)))
        await ws.send_str(json.dumps({"type": "end", "id": 8, "samples": 504, "reason": "short"}))
        await asyncio.sleep(0.1); await ws.close()
        self.assertFalse((Path(self.tmp.name) / "log.jsonl").exists())

    async def test_garbage_is_ignored(self):
        ws = await self.client.ws_connect("/pet")
        await ws.send_str("not json")
        await ws.send_bytes(b"\x00" * 300)                  # audio before any start
        await ws.send_str(json.dumps({"type": "end", "id": 9}))
        await asyncio.sleep(0.1); await ws.close()
        self.assertFalse((Path(self.tmp.name) / "log.jsonl").exists())

    async def test_local_whisper_text_goes_to_the_pet_the_json_and_the_log(self):
        ws = await self.utter("Claude-D350", 9, tone(8000), lambda b: [b])
        msg = await ws.receive_json(timeout=5)
        await ws.close()
        self.assertEqual(msg, {"type": "heard", "id": 9, "text": "สวัสดีครับ แมวออราเคิล"})
        self.assertEqual(self.ear.heard, [[(0.0, 0.5)]])           # only the speech went to Whisper
        recs = self.records()
        self.assertEqual((recs[1]["text"], recs[1]["stt"], recs[1]["speech_s"]), ("สวัสดีครับ แมวออราเคิล", "whisper (local)", 0.5))

    async def test_no_speech_is_never_transcribed(self):
        self.ear.regions = [(0.1, 0.2)]                              # 0.1 s: a fan, a door
        self.key = "test-key"
        self.srv.engine = "paxa"
        with mock.patch.object(S, "transcribe", new=mock.AsyncMock()) as m:
            ws = await self.utter("Claude-D350", 10, tone(8000), lambda b: [b])
            await self.settle(); await asyncio.sleep(0.2); await ws.close()
        m.assert_not_awaited()
        self.assertEqual(self.ear.heard, [])
        self.assertEqual((self.records()[1]["text"], self.records()[1]["stt"]), ("", "none: no speech"))

    async def test_stt_text_goes_to_the_pet_the_json_and_the_log(self):
        self.key = "test-key"
        self.srv.engine = "paxa"
        with mock.patch.object(S, "transcribe", new=mock.AsyncMock(return_value=("สวัสดีครับ แมวออราเคิล", 123))) as m:
            ws = await self.utter("Claude-D350", 4, tone(8000), lambda b: [b])
            msg = await ws.receive_json(timeout=5)
            await ws.close()
        self.assertEqual(msg, {"type": "heard", "id": 4, "text": "สวัสดีครับ แมวออราเคิล"})
        self.assertEqual(m.await_args.args[1], "test-key")
        recs = self.records()
        self.assertEqual(recs[1], {"update": recs[0]["file"], "text": "สวัสดีครับ แมวออราเคิล", "stt": "paxa", "stt_ms": 123, "speech_s": 0.5})
        saved = json.loads((Path(self.tmp.name) / recs[0]["file"]).with_suffix(".json").read_text(encoding="utf-8"))
        self.assertEqual(saved["text"], "สวัสดีครับ แมวออราเคิล")
        listing = await (await self.client.get("/api/utterances")).json()
        self.assertEqual(listing[0]["text"], "สวัสดีครับ แมวออราเคิล")
        for f in Path(self.tmp.name).rglob("*"):            # the key never lands on disk
            if f.is_file():
                self.assertNotIn(b"test-key", f.read_bytes())

    async def test_no_key_no_stt(self):
        self.srv.engine = "paxa"
        with mock.patch.object(S, "transcribe", new=mock.AsyncMock()) as m:
            ws = await self.utter("Claude-D350", 5, tone(8000), lambda b: [b])
            await self.settle(); await asyncio.sleep(0.1); await ws.close()
        m.assert_not_awaited()
        self.assertIsNone(self.records()[0]["text"])

    async def test_transcriber_compat_api(self):
        ws = await self.utter("Claude-D350", 11, tone(8000), lambda b: [b])
        await ws.receive_json(timeout=5); await ws.close()
        h = await (await self.client.get("/health")).json()
        self.assertTrue(h["ok"]); self.assertTrue(h["speaking"]); self.assertTrue(h["sessionId"].endswith("-Claude-D350"))
        md = await (await self.client.get("/api/transcript.md", params={"session": h["sessionId"]})).text()
        self.assertIn("[Claude-D350] สวัสดีครับ แมวออราเคิล", md)

    async def test_page_and_audio(self):
        ws = await self.utter("Claude-D350", 6, tone(504), lambda b: [b])
        await self.settle(); await ws.close()
        r = self.records()[0]
        self.assertEqual((await self.client.get("/")).status, 200)
        a = await self.client.get("/audio/" + r["file"])
        self.assertEqual((a.status, (await a.read())[:4]), (200, b"RIFF"))
        self.assertEqual((await self.client.get("/audio/../../etc/passwd")).status, 404)


if __name__ == "__main__":
    unittest.main()
