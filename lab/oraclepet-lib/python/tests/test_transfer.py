"""The transfer engine against a simulated pet: no Bluetooth, no radio, no real pet.

SimPet is the pet side of lab/jc3248-pet-idf/main/ota.c, in-process. BEGIN / BEGIN_FILE /
BEGIN_STREAM open a transfer and notify "ready" with the window; data writes land in a 32 KB
stream buffer (more than that means the host ignored the window, and the pet aborts, as
data_access does); a drain task takes 4 KB at a time, acks every 4 KB, re-acks while idle,
waits for END, checks the CRC and reports saved / streamed / done, then "reboots" onto the
other slot. SELECT_PACK, RUN_APP, PLAY, SELFTEST and LISTEN answer as the firmware does.
Faults on request: a frozen pet, a dropped link, a corrupted byte, a refusal, a lost reply.
Times are scaled from seconds to milliseconds.

    python -m unittest discover -s tests -v
"""
from __future__ import annotations

import asyncio
import json
import math
import os
import struct
import tempfile
import time
import unittest
import zlib
from pathlib import Path
from unittest import mock

from oraclepet import protocol as P
from oraclepet.pet import LINK_LOST, BleLink, Link, Pet, PetError, _is_status, _matcher


class AttError(Exception):
    """An ATT error response to a write with response (bleak raises one too)."""


def make_image(project: str = "oraclepet_lcd28", version: str = "v9.9.9", size: int = 100_000) -> bytes:
    """A synthetic app image: 0xE9, then esp_app_desc_t at offset 32, then noise."""
    img = bytearray(os.urandom(size))
    img[0] = 0xE9
    d = P.APP_DESC_OFFSET
    img[d:d + 256] = bytes(256)
    struct.pack_into("<I", img, d, P.APP_DESC_MAGIC)
    img[d + 16:d + 16 + len(version)] = version.encode()
    img[d + 48:d + 48 + len(project)] = project.encode()
    img[d + 80:d + 88] = b"12:00:00"
    img[d + 96:d + 107] = b"Oct 21 2026"
    img[d + 144:d + 176] = os.urandom(32)
    return bytes(img)


class SimPet(Link):
    """The pet side of ota.c, as a Link the Pet class drives."""

    CHUNK = 4096                        # ota_task reads the stream buffer 4 KB at a time
    ACK_EVERY = 4096
    SB_SIZE = 32 * 1024                 # stream buffer between the GATT callback and ota_task
    HDR = P.APP_DESC_OFFSET + 256       # image header + esp_app_desc_t, checked before going on

    def __init__(self, *, mtu: int = 185, window: int = 16 * 1024, project: str = "oraclepet_lcd28",
                 report_project: bool = True, slot: str = "ota_0", rollback: bool = False,
                 has_mic: bool = True, mic_char: bool = True, drain_delay: float = 0.0005,
                 idle_tick: float = 0.02, idle_limit: float = 0.6, end_wait: float = 0.5,
                 reboot_delay: float = 0.02, boot_time: float = 0.05, hang_after: int | None = None,
                 drop_after: int | None = None, corrupt: bool = False, refuse: str | None = None,
                 drop_heard: bool = False, write_error_after: int | None = None):
        self.name, self.address, self.on_lost = "Claude-51A0", "SIM", None
        self.write_error_after = write_error_after          # the host's stack fails a write, link still up
        self.mtu, self.window, self.project, self.report_project = mtu, window, project, report_project
        self.slot, self.rollback, self.has_mic, self.mic_char = slot, rollback, has_mic, mic_char
        self.drain_delay, self.idle_tick, self.idle_limit, self.end_wait = drain_delay, idle_tick, idle_limit, end_wait
        self.reboot_delay, self.boot_time = reboot_delay, boot_time
        self.hang_after, self.drop_after, self.corrupt, self.refuse = hang_after, drop_after, corrupt, refuse
        self.drop_heard = drop_heard
        self.version = "v1.0.0"
        self.fs: dict[str, bytes] = {"sounds/say.ima": P.psnd_encode([0] * 100)}   # LittleFS
        self.builtin_packs = ["atlas", "siamese"]
        self.pack, self.app = "atlas", ""
        # what the host did, for the tests
        self.commands: list[bytes] = []         # every control write, in order
        self.progress: list[int] = []           # PROGRESS values
        self.rx = bytearray()                   # NUS RX bytes (heartbeats)
        self.rx_writes: list[int] = []
        self.reads = 0
        self.max_chunk = 0
        self.max_in_flight = 0                  # bytes received beyond the last ack the pet sent
        self.overflowed = False
        self.streams: list[bytes] = []
        self.reboots = 0
        self.last_listen: bytes | None = None
        # pet state
        self._subs: dict = {}
        self._connected = True
        self._state = "idle"
        self._booting: asyncio.Event | None = None
        self._tasks: set = set()
        self._sb = bytearray()
        self._data_ev = asyncio.Event()
        self._size = self._crc = self._received = self._last_ack = 0
        self._kind, self._path = "app", None
        self._abort = self._end = self._corrupted = False

    # ---- Link ----
    @property
    def connected(self) -> bool:
        return self._connected

    def has(self, uuid: str) -> bool:
        return uuid in (P.NUS_RX, P.NUS_TX, P.OTA_CONTROL, P.OTA_DATA) or (uuid == P.OTA_MIC and self.mic_char)

    def max_write(self, uuid: str) -> int:
        return self.mtu - 3

    async def read(self, uuid: str) -> bytes:
        assert uuid == P.OTA_CONTROL
        if not self._connected:
            raise ConnectionError("not connected")
        self.reads += 1
        await asyncio.sleep(0)
        return self.firmware_json()

    async def write(self, uuid: str, data: bytes, response: bool) -> None:
        if not self._connected:
            raise ConnectionError("not connected")
        data = bytes(data)
        if uuid == P.OTA_CONTROL:
            assert response, "control takes writes with response"
            self._control(data)
        elif uuid == P.OTA_DATA:
            assert not response, "data takes writes without response"
            if self.write_error_after is not None and self._received >= self.write_error_after:
                raise RuntimeError("write failed")
            self._data(data)
        elif uuid == P.NUS_RX:
            self.rx += data
            self.rx_writes.append(len(data))
        else:
            raise AttError("no such characteristic")
        await asyncio.sleep(0)

    async def subscribe(self, uuid: str, callback) -> None:
        self._subs[uuid] = callback

    async def reconnect(self, timeout: float) -> None:
        if self._booting is not None:
            await asyncio.wait_for(self._booting.wait(), timeout)
        self._connected = True

    async def close(self) -> None:
        self._connected = False

    async def shutdown(self) -> None:
        for t in list(self._tasks):
            t.cancel()
        await asyncio.gather(*self._tasks, return_exceptions=True)

    # ---- the pet ----
    def firmware_json(self) -> bytes:
        packs = sorted(set(self.builtin_packs) | {p.split("/")[1] for p in self.fs if p.startswith("characters/")})
        apps = sorted(p[5:-5] for p in self.fs if p.startswith("apps/"))
        o = {"ver": self.version, "project": self.project, "built": "Oct 21 2026 13:23:37", "slot": self.slot,
             "sha": "657a5f60a", "state": "valid", "pack": self.pack, "packs": packs, "free": 1244,
             "app": self.app, "apps": apps, "apperr": ""}
        if not self.report_project:
            del o["project"]
        return json.dumps(o, separators=(",", ":")).encode()

    def tx(self, data: bytes) -> None:
        """A line the pet sends on NUS TX ({"evt":...})."""
        self._notify(P.NUS_TX, data)

    def _notify(self, uuid: str, data: bytes) -> None:
        cb = self._subs.get(uuid)
        if cb is not None and self._connected:
            asyncio.get_running_loop().call_soon(cb, data)

    def _status(self, **o) -> None:
        self._notify(P.OTA_CONTROL, json.dumps(o, separators=(",", ":")).encode())

    def _spawn(self, coro) -> None:
        t = asyncio.ensure_future(coro)
        self._tasks.add(t)
        t.add_done_callback(self._tasks.discard)

    def _lose_link(self) -> None:
        self._connected = False
        if self.on_lost is not None:
            self.on_lost()

    def _control(self, cmd: bytes) -> None:
        """ctrl_access(): one command, answered at once or by a task."""
        self.commands.append(cmd)
        op = cmd[0]
        if op in (0x01, 0x04, 0x0B):                      # BEGIN, BEGIN_FILE, BEGIN_STREAM
            if len(cmd) < 9:
                raise AttError("invalid attribute value length")
            if self._state != "idle":
                return self._status(ota="error", why="busy")
            self._size, self._crc = struct.unpack_from("<II", cmd, 1)
            self._kind, self._path = ("stream" if op == 0x0B else "app"), None
            if op == 0x04:
                pl = cmd[9] if len(cmd) > 9 else 0
                if pl == 0 or pl >= 96 or 10 + pl > len(cmd):
                    raise AttError("invalid attribute value length")
                self._path = cmd[10:10 + pl].decode()
                if not P.pet_accepts_path(self._path):
                    return self._status(ota="error", why="bad path")
                self._kind = "file"
            self._received, self._abort, self._end = 0, False, False
            self._state = "starting"
            self._spawn(self._transfer())
        elif op == 0x02:
            self._end = True
        elif op == 0x03:
            self._abort = True
        elif op == 0x07:
            self.progress.append(min(cmd[1], 100))
        elif op in (0x05, 0x06, 0x08):
            name = cmd[2:2 + cmd[1]].decode()
            if op == 0x05:
                self._spawn(self._select(name))
            elif op == 0x06:
                self._spawn(self._run_app(name))
            elif f"sounds/{name}.ima" not in self.fs:
                self._status(ota="error", why=f"no sound {name}")
            else:
                self._status(ota="play", name=name, bytes=len(self.fs[f"sounds/{name}.ima"]))
        elif op == 0x0A:
            self._spawn(self._selftest())
        elif op == 0x0C:
            if len(cmd) < 4:
                raise AttError("invalid attribute value length")
            ms = max(100, min(10_000, cmd[1] | cmd[2] << 8))
            self._spawn(self._listen(ms, bool(cmd[3] & 1) and P.OTA_MIC in self._subs))
        else:
            raise AttError("request not supported")

    def _data(self, chunk: bytes) -> None:
        """data_access(): bytes into the stream buffer; overflow = the host ignored the window."""
        self.max_chunk = max(self.max_chunk, len(chunk))
        if len(chunk) > 520 or self._state != "receiving":
            return                                         # too big for the pet's buffer / stray bytes
        self._received += len(chunk)
        self.max_in_flight = max(self.max_in_flight, self._received - self._last_ack)
        if self._received > self._size:
            self._abort = True
            return
        if len(self._sb) + len(chunk) > self.SB_SIZE:
            self.overflowed = self._abort = True
            return
        if self.corrupt and not self._corrupted and self._received > self._size // 2:
            chunk = bytes([chunk[0] ^ 0xFF]) + chunk[1:]
            self._corrupted = True
        self._sb += chunk
        self._data_ev.set()

    def _ack(self, off: int) -> None:
        self._last_ack = off
        self._status(ota="ack", off=off)

    def _fail(self, why: str) -> None:
        self._state = "idle"
        self._status(ota="error", why=why)

    async def _wait_end(self) -> None:
        t = 0.0
        while not self._end and not self._abort and t < self.end_wait:
            await asyncio.sleep(0.005)
            t += 0.005

    async def _transfer(self) -> None:
        """ota_task / file_transfer / stream_transfer."""
        await asyncio.sleep(0)                             # the BEGIN write is answered first
        if self.refuse and self._kind == "file":
            return self._fail(self.refuse)
        self._sb = bytearray()
        self._data_ev = asyncio.Event()
        self._last_ack = 0
        written = crc = last_ack = 0
        idle = 0.0
        data = bytearray()
        self._state = "receiving"
        self._status(ota="ready", size=self._size, window=self.window)
        while written < self._size:
            if self._abort:
                if self._kind == "stream":
                    break
                return self._fail("too many bytes" if self._received > self._size else "aborted")
            if self.hang_after is not None and written >= self.hang_after:
                await asyncio.Event().wait()               # a frozen pet: no acks, nothing
            if self.drop_after is not None and written >= self.drop_after:
                self._state = "idle"
                return self._lose_link()
            if not self._sb:
                self._data_ev.clear()
                try:
                    await asyncio.wait_for(self._data_ev.wait(), self.idle_tick)
                except asyncio.TimeoutError:
                    idle += self.idle_tick
                    if idle >= self.idle_limit:
                        if self._kind == "stream":
                            break
                        return self._fail("timed out waiting for data")
                    self._ack(written)                     # re-ack: a lost ack must not stall the host
                continue
            idle = 0.0
            chunk = bytes(self._sb[:self.CHUNK])
            del self._sb[:len(chunk)]
            await asyncio.sleep(self.drain_delay)          # flash write / playback time
            if self._kind == "app" and len(data) < self.HDR <= len(data) + len(chunk):
                why = self._check_image(bytes(data + chunk))
                if why:
                    return self._fail(why)
            crc = zlib.crc32(chunk, crc)
            data += chunk
            written += len(chunk)
            if written - last_ack >= self.ACK_EVERY or written == self._size:
                last_ack = written
                self._ack(written)
        if self._kind == "stream":
            await self._wait_end()
            self._state = "idle"
            if self._abort or written < self._size:
                return self._status(ota="error", why="stream cut short")
            self.streams.append(bytes(data))
            return self._status(ota="streamed", bytes=written, crc=crc == self._crc)
        self._state = "finishing"
        await self._wait_end()
        if not self._end:
            return self._fail("aborted" if self._abort else "no END from the host")
        if crc != self._crc:
            return self._fail("crc mismatch")
        if self._kind == "file":
            self.fs[self._path] = bytes(data)
            self._state = "idle"
            return self._status(ota="saved", path=self._path)
        self._status(ota="done")
        self._spawn(self._reboot(P.describe(bytes(data))))

    def _check_image(self, hdr: bytes) -> str | None:
        """check_desc(): an app image of this project, before any more is written."""
        if hdr[0] != 0xE9 or struct.unpack_from("<I", hdr, P.APP_DESC_OFFSET)[0] != P.APP_DESC_MAGIC:
            return "not an ESP app image"
        project = P.project_name(hdr) or ""
        return None if project == self.project else f"wrong firmware ({project[:24]})"

    async def _reboot(self, desc) -> None:
        await asyncio.sleep(self.reboot_delay)             # let "done" leave, then esp_restart()
        self._booting = asyncio.Event()
        self._state = "idle"
        self.reboots += 1
        self._lose_link()
        await asyncio.sleep(self.boot_time)
        if not self.rollback:                              # the new image marks itself valid
            self.slot = "ota_1" if self.slot == "ota_0" else "ota_0"
            self.version = desc.version
        self._booting.set()

    async def _select(self, name: str) -> None:
        await asyncio.sleep(0.005)                         # the main thread applies it
        if name in self.builtin_packs or f"characters/{name}/manifest.json" in self.fs:
            self.pack = name
            self._status(ota="pack", name=name)
        else:
            self._status(ota="error", why=f"pack {name} did not load")

    async def _run_app(self, name: str) -> None:
        await asyncio.sleep(0.005)
        if name and f"apps/{name}.wasm" not in self.fs:
            return self._status(ota="error", why=f"app {name}: no such app")
        self.app = name
        self._status(ota="app", name=name)

    async def _selftest(self) -> None:
        await asyncio.sleep(0.01)
        if self.has_mic:
            self._notify(P.OTA_CONTROL, b'{"ota":"selftest","mic":true,"heard":true,"db":37.5,"quiet":22,"tone":191}')
        else:
            self._notify(P.OTA_CONTROL, b'{"ota":"selftest","mic":false,"heard":false,"db":0.0,"quiet":0,"tone":0}')

    async def _listen(self, ms: int, send: bool) -> None:
        """do_listen() + mic_audio(): the header, then each 256-byte block, each split into
        notifications of MTU-3; then the "heard" line."""
        await asyncio.sleep(0.01)                          # recording, scaled down
        if not self.has_mic:
            return self._status(ota="error", why="no mic")
        n = 16 * ms
        pcm = [int(8000 * math.sin(2 * math.pi * 1000 * i / 16_000)) for i in range(n)]
        if send:
            clip = P.psnd_encode(pcm)
            self.last_listen = clip
            m = self.mtu - 3
            for piece in [clip[:16]] + [clip[i:i + 256] for i in range(16, len(clip), 256)]:
                for off in range(0, len(piece), m):
                    self._notify(P.OTA_MIC, piece[off:off + m])
        if not self.drop_heard:
            self._status(ota="heard", ms=ms, samples=n, rms=412, peak=9120, freq=1000, db=23.4, floor=31, sent=send)


TONE = [0] * 4000 + [int(12_000 * math.sin(2 * math.pi * 300 * i / 16_000)) for i in range(8000)] + [0] * 4000


class TransferEngine(unittest.IsolatedAsyncioTestCase):
    async def make(self, **kw) -> tuple[SimPet, Pet]:
        sim = SimPet(**kw)
        pet = Pet(link=sim)
        # the Mac app's timeouts, scaled from seconds to tenths
        pet.stall_s, pet.reboot_s, pet.settle_s, pet.reply_s, pet.listen_s, pet.tail_s = 0.4, 3.0, 0.01, 2.0, 1.0, 0.3
        await pet.open()
        self.addAsyncCleanup(sim.shutdown)
        return sim, pet

    def pack_dir(self, files: dict, name: str = "neo") -> Path:
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        d = Path(tmp.name) / name
        d.mkdir()
        for k, v in files.items():
            (d / k).write_bytes(v)
        return d

    # ---- reading ----
    async def test_info_on_connect(self):
        sim, pet = await self.make()
        fw = pet.firmware
        self.assertEqual((fw.project, fw.board, fw.slot, fw.state, fw.packs, fw.pack),
                         ("oraclepet_lcd28", "2.8in ILI9341", "ota_0", "valid", ["atlas", "siamese"], "atlas"))

    # ---- full transfers ----
    async def test_pack_full_transfer(self):
        sim, pet = await self.make(drain_delay=0.005)              # a pet slower than the link: the window fills
        files = {"idle.gif": os.urandom(40_000), "busy.gif": os.urandom(30_000),
                 "manifest.json": b'{"name":"neo"}', "notes.txt": b"not part of a pack"}
        seen = []
        pet.on_progress = seen.append
        out = await pet.pack(self.pack_dir(files))
        self.assertEqual(out.kind, "pack")
        self.assertRegex(out.message, r"^Pet now shows neo · 68 KB in \d+ s$")
        for k in ("idle.gif", "busy.gif", "manifest.json"):
            self.assertEqual(sim.fs[f"characters/neo/{k}"], files[k])
        self.assertNotIn("characters/neo/notes.txt", sim.fs)
        begins = [c[10:].decode() for c in sim.commands if c[0] == 0x04]
        self.assertEqual(begins, ["characters/neo/busy.gif", "characters/neo/idle.gif",
                                  "characters/neo/manifest.json"])     # sorted, manifest last
        self.assertEqual(sim.commands[-1], P.select_pack("neo"))
        self.assertEqual((sim.pack, pet.firmware.pack), ("neo", "neo"))   # control re-read after "pack"
        self.assertIn("neo", pet.firmware.packs)
        self.assertEqual(sim.progress, sorted(sim.progress))
        self.assertEqual(sim.progress[-1], 100)
        self.assertLessEqual(sim.max_in_flight, 16 * 1024)             # the window is never exceeded
        self.assertGreater(sim.max_in_flight, 12 * 1024)               # and it was really used
        self.assertLessEqual(sim.max_chunk, 182)                       # chunks of the negotiated size
        self.assertFalse(sim.overflowed)
        self.assertNotIn(P.ABORT, sim.commands)
        self.assertTrue({"starting", "sending", "finishing"} <= {p.phase for p in seen})
        self.assertEqual(seen[-1].overall, 1.0)

    async def test_window_is_never_exceeded(self):
        for window in (2048, 4096, 6000, 16 * 1024):
            with self.subTest(window=window):
                sim, pet = await self.make(window=window, mtu=247)
                wasm = b"\0asm" + os.urandom(30_000)
                out = await pet.app(wasm, name="w")
                self.assertEqual(sim.fs["apps/w.wasm"], wasm)
                self.assertLessEqual(sim.max_in_flight, window)
                self.assertGreaterEqual(sim.max_in_flight, min(window, 244))
                self.assertLessEqual(sim.max_chunk, 244)
                self.assertRegex(out.message, r"^Pet runs w · 30004 bytes in \d+ s$")

    async def test_full_ota_reboots_onto_the_other_slot(self):
        sim, pet = await self.make()
        phases = []
        pet.on_progress = lambda p: phases.append(p.phase)
        out = await pet.ota(make_image(version="v9.9.9"))
        self.assertEqual(out.kind, "updated")
        self.assertRegex(out.message, r"^Updated in \d+ s: now v9\.9\.9 on ota_1$")
        self.assertEqual((sim.reboots, pet.firmware.slot, pet.firmware.version), (1, "ota_1", "v9.9.9"))
        self.assertEqual(sim.commands[0][0], 0x01)                     # BEGIN, not BEGIN_FILE
        self.assertEqual(sim.progress, [])                             # a lone full OTA: the pet draws its own bar
        self.assertEqual(phases[-1], "rebooting")

    async def test_rollback_is_reported(self):
        sim, pet = await self.make(rollback=True)
        with self.assertRaises(PetError) as cm:
            await pet.ota(make_image())
        self.assertEqual(str(cm.exception), "Pet came back on ota_0 again: the new image was rolled back")

    async def test_image_checks_before_sending(self):
        sim, pet = await self.make(project="jc3248_pet_idf")
        cases = [(make_image("oraclepet_lcd28"), "That firmware is for the 2.8in ILI9341 board; this pet is JC3248 3.5in"),
                 (bytes(4096), "Not an ESP32 app image"),
                 (make_image("esp32_other"), "Not a pet firmware: esp32_other")]
        for img, msg in cases:
            with self.subTest(msg=msg), self.assertRaises(PetError) as cm:
                await pet.ota(img)
            self.assertEqual(str(cm.exception), msg)
        self.assertEqual(sim.commands, [])                             # nothing reached the pet

    async def test_the_pet_refuses_another_boards_image(self):
        # firmware that does not report its project: the pet's own header check stops it
        sim, pet = await self.make(project="jc3248_pet_idf", report_project=False)
        with self.assertRaises(PetError) as cm:
            await pet.ota(make_image("oraclepet_lcd28"))
        self.assertEqual(str(cm.exception), "Pet refused the update: wrong firmware (oraclepet_lcd28)")
        self.assertEqual(sim.reboots, 0)

    async def test_empty_file_goes_through(self):
        sim, pet = await self.make()
        await pet.pack(self.pack_dir({"manifest.json": b"{}", "empty.json": b""}))
        self.assertEqual(sim.fs["characters/neo/empty.json"], b"")    # END right after "ready"

    # ---- streams and speech ----
    async def test_stream(self):
        sim, pet = await self.make()
        pcm = [int(9000 * math.sin(2 * math.pi * 440 * i / 16_000)) for i in range(24_000)]
        clip = P.psnd_encode(pcm)
        out = await pet.stream(pcm)
        self.assertEqual(out.kind, "streamed")
        self.assertEqual(sim.streams, [clip])
        self.assertEqual(sim.commands[0], P.begin_stream(len(clip), P.crc32(clip)))
        self.assertTrue(out.details["crc_ok"])
        self.assertRegex(out.message, rf"^Pet played the clip · streamed {len(clip) // 1024} KB, sound after \d+\.\d\d s$")
        self.assertTrue(sim.progress)       # as in the Mac app: a stream has a path ("@stream"), so PROGRESS goes out

    async def test_say_uses_this_os_speech(self):
        sim, pet = await self.make()
        with mock.patch("oraclepet.tts.synthesize", return_value=TONE) as synth:
            out = await pet.say("  hello  ", language="en")
        synth.assert_called_once_with("hello", "en", None)
        self.assertRegex(out.message, r"^Pet says “hello” · streamed \d+ KB, sound after \d+\.\d\d s$")
        self.assertEqual(sim.streams, [P.psnd_encode(P.tidy(TONE))])  # trimmed, normalised, encoded
        with self.assertRaisesRegex(PetError, "^Nothing to say$"):
            await pet.say("   ")
        with mock.patch("oraclepet.tts.synthesize", return_value=[0] * 16_000):
            with self.assertRaisesRegex(PetError, "silent"):
                await pet.say("hello")

    # ---- faults ----
    async def test_stall_aborts_and_drops_the_queue(self):
        sim, pet = await self.make(hang_after=8192)
        t0 = time.monotonic()
        a = asyncio.ensure_future(pet.app(b"\0asm" + os.urandom(40_000), name="big"))
        await asyncio.sleep(0)
        b = asyncio.ensure_future(pet.select_pack("atlas"))           # queued behind it
        ra, rb = await asyncio.gather(a, b, return_exceptions=True)
        self.assertEqual(str(ra), "Update stalled: no progress from the pet for 0.4 s")
        self.assertEqual(str(rb), "Update cancelled")                 # the queue goes too, as in the Mac app
        self.assertEqual(sim.commands[-1], P.ABORT)
        self.assertNotIn(P.select_pack("atlas"), sim.commands)
        self.assertLess(time.monotonic() - t0, 2.0)

    async def test_error_notification(self):
        sim, pet = await self.make(refuse="storage full")
        with self.assertRaises(PetError) as cm:
            await pet.pack(self.pack_dir({"idle.gif": b"GIF89a", "manifest.json": b"{}"}))
        self.assertEqual(str(cm.exception), "Pet refused the update: storage full")
        self.assertEqual(sum(1 for c in sim.commands if c[0] == 0x04), 1)    # the rest of the job is dropped
        sim.refuse = None
        out = await pet.select_pack("siamese")                                # and the pet is usable again
        self.assertRegex(out.message, r"^Pet now shows siamese · 0 KB in \d+ s$")

    async def test_busy_pet(self):
        sim, pet = await self.make()
        sim._state = "receiving"                                   # another host's transfer
        with self.assertRaisesRegex(PetError, "^Pet refused the update: busy$"):
            await pet.app(b"\0asm1234", name="x")

    async def test_crc_mismatch(self):
        sim, pet = await self.make(corrupt=True)
        with self.assertRaisesRegex(PetError, "^Pet refused the update: crc mismatch$"):
            await pet.app(b"\0asm" + os.urandom(20_000), name="bad")
        self.assertNotIn("apps/bad.wasm", sim.fs)
        sim2, pet2 = await self.make(corrupt=True)
        out = await pet2.stream(P.psnd_encode([0] * 16_000))       # a stream reports it instead of failing
        self.assertFalse(out.details["crc_ok"])
        self.assertTrue(out.message.endswith(" (CRC mismatch)"))

    async def test_link_lost_mid_transfer(self):
        sim, pet = await self.make(drop_after=8192)
        with self.assertRaises(PetError) as cm:
            await pet.app(b"\0asm" + os.urandom(40_000), name="x")
        self.assertEqual(str(cm.exception), LINK_LOST)
        with self.assertRaisesRegex(PetError, "^Not connected to the pet$"):
            await pet.run("x")

    async def test_bluetooth_error_aborts_the_pet(self):
        sim, pet = await self.make(write_error_after=8192)
        with self.assertRaisesRegex(PetError, "^Bluetooth error during the update: write failed$"):
            await pet.app(b"\0asm" + os.urandom(40_000), name="x")
        self.assertEqual(sim.commands[-1], P.ABORT)                    # freed now, not after its own timeout

    async def test_cancel(self):
        sim, pet = await self.make(hang_after=4096)
        pet.stall_s = 5.0
        job = asyncio.ensure_future(pet.app(b"\0asm" + os.urandom(30_000), name="x"))
        await asyncio.sleep(0.1)
        pet.cancel()
        with self.assertRaisesRegex(PetError, "^Update cancelled$"):
            await job
        self.assertEqual(sim.commands[-1], P.ABORT)

    async def test_the_simulator_catches_a_host_that_ignores_the_window(self):
        # proves the window assertions above can fail: a greedy host overruns the pet
        sim, pet = await self.make(drain_delay=0.01)
        data = os.urandom(60_000)
        await sim.write(P.OTA_CONTROL, P.begin_file(len(data), P.crc32(data), "apps/greedy.wasm"), True)
        await asyncio.sleep(0.01)                                  # "ready"
        for i in range(0, len(data), 182):
            await sim.write(P.OTA_DATA, data[i:i + 182], False)    # never waits for an ack
        self.assertGreater(sim.max_in_flight, 16 * 1024)
        self.assertTrue(sim.overflowed)

    # ---- queueing ----
    async def test_jobs_queue_and_share_one_bar(self):
        sim, pet = await self.make()
        d = self.pack_dir({"idle.gif": os.urandom(50_000), "manifest.json": b"{}"})
        wasm = b"\0asm" + os.urandom(20_000)
        a = asyncio.ensure_future(pet.pack(d))
        await asyncio.sleep(0)
        b = asyncio.ensure_future(pet.app(wasm, name="hello"))     # asked for while the pack runs
        ra, rb = await asyncio.gather(a, b)
        self.assertEqual((ra.kind, rb.kind), ("pack", "app"))
        self.assertRegex(rb.message, r"^Pet runs hello · 20004 bytes in \d+ s$")
        ops = [c[0] for c in sim.commands if c[0] != 0x07]
        self.assertEqual(ops, [0x04, 0x02, 0x04, 0x02, 0x05, 0x04, 0x02, 0x06])   # in order, never interleaved
        # one bar across both jobs: the app's part starts where the pack's ended
        app_begin = [i for i, c in enumerate(sim.commands) if c[0] == 0x04][-1]
        pack_pct = [c[1] for c in sim.commands[:app_begin] if c[0] == 0x07]
        app_pct = [c[1] for c in sim.commands[app_begin:] if c[0] == 0x07]
        share = int(50_002 / (50_002 + 20_004) * 100)
        self.assertEqual(max(pack_pct), share)
        self.assertTrue(min(app_pct) >= share and app_pct[-1] == 100)
        self.assertEqual(sim.progress, sorted(sim.progress))

    # ---- commands ----
    async def test_command_replies_and_refusals(self):
        sim, pet = await self.make()
        self.assertEqual((await pet.play("say")).message, "Pet plays the sound")
        with self.assertRaisesRegex(PetError, "^Pet refused the update: no sound nope$"):
            await pet.play("nope")
        with self.assertRaisesRegex(PetError, "^Pet refused the update: pack nope did not load$"):
            await pet.select_pack("nope")
        with self.assertRaisesRegex(PetError, "^Pet refused the update: app ghost: no such app$"):
            await pet.run("ghost")
        self.assertEqual((await pet.stop_app()).message, "App stopped")
        self.assertEqual(sim.commands[-1], P.run_app(""))
        n = len(sim.commands)
        with self.assertRaisesRegex(PetError, "is not a pack name"):
            await pet.select_pack("Bad Name")
        with self.assertRaisesRegex(PetError, "^hello is not a WebAssembly module$"):
            await pet.app(b"MZ\x90\x00", name="hello")
        with self.assertRaisesRegex(PetError, "lower-case"):
            await pet.pack(self.pack_dir({"Idle.gif": b"x", "manifest.json": b"{}"}))
        with self.assertRaisesRegex(PetError, "not a pack folder"):
            await pet.pack(self.pack_dir({"idle.gif": b"x"}, name="nomanifest"))
        self.assertEqual(len(sim.commands), n)                     # refused here, nothing sent

    async def test_selftest(self):
        sim, pet = await self.make()
        r = await pet.selftest()
        self.assertEqual(r["summary"], "Heard by its mic: +37.5 dB at 1 kHz (rms 191 vs 22 quiet)")
        self.assertEqual(sim.commands, [P.SELFTEST])
        sim2, pet2 = await self.make(has_mic=False)
        self.assertEqual((await pet2.selftest())["summary"], "Beeped (this pet has no mic to check itself)")

    async def test_listen_levels_and_audio(self):
        sim, pet = await self.make(mtu=185)                         # a 256-byte block spans two notifications
        levels, samples = await pet.listen(500)
        self.assertEqual(levels, {"ms": 500, "samples": 8000, "rms": 412, "peak": 9120, "freq": 1000,
                                  "db": 23.4, "floor": 31, "sent": True})
        self.assertEqual(sim.commands[-1], P.listen(500, True))
        self.assertEqual(len(samples), 8000)
        self.assertEqual(samples, P.psnd_decode(sim.last_listen))
        levels, samples = await pet.listen(50, send_audio=False)  # clamped to 100 ms
        self.assertIsNone(samples)
        self.assertEqual((levels["ms"], sim.commands[-1]), (100, P.listen(100, False)))

    async def test_listen_without_a_mic(self):
        sim, pet = await self.make(has_mic=False)
        with self.assertRaisesRegex(PetError, "^This pet has no mic$"):
            await pet.listen(500)

    async def test_listen_on_firmware_without_the_mic_characteristic(self):
        sim, pet = await self.make(mic_char=False)
        levels, samples = await pet.listen(200)
        self.assertIsNone(samples)
        self.assertEqual(sim.commands[-1], P.listen(200, False))

    async def test_listen_when_heard_never_comes(self):
        sim, pet = await self.make(drop_heard=True)
        pet.listen_s = 5.0                                         # so only tail_s can end it early
        t0 = time.monotonic()
        with self.assertRaises(PetError) as cm:
            await pet.listen(200)
        self.assertEqual(str(cm.exception), "No 'heard' reply from the pet (3200 samples of audio arrived)")
        self.assertEqual(len(cm.exception.samples), 3200)             # the audio is not lost
        self.assertLess(time.monotonic() - t0, 2.5)                    # tail_s after the audio, not listen_s

    # ---- NUS ----
    async def test_heartbeat_chunks_and_flushes(self):
        sim, pet = await self.make(mtu=23)
        beat = P.Heartbeat(3, 1, 2, msg="wait: neo", epoch=1_792_547_421, tzoff=25_200, who="neo")
        line = await pet.heartbeat(beat)
        self.assertEqual(bytes(sim.rx), line)
        self.assertEqual(sim.rx_writes, [20] * (len(line) // 20) + ([len(line) % 20] if len(line) % 20 else []))
        reads = sim.reads
        await pet.close()
        self.assertEqual(sim.reads, reads + 1)       # a read answered after the writes: they left first

    async def test_events(self):
        sim, pet = await self.make()
        got = []
        pet.on_event(got.append)
        sim.tx(b'{"evt":"ta')
        sim.tx(b'p"}\n{"evt":"say","text":"hi"}\n')
        await asyncio.sleep(0.01)
        self.assertEqual(got, [{"evt": "tap"}, {"evt": "say", "text": "hi"}])
        self.assertEqual(list(pet.events), got)
        pet.speak_events = True                      # pet.say() in a wasm app: the host speaks it
        with mock.patch("oraclepet.tts.synthesize", return_value=TONE):
            sim.tx(b'{"evt":"say","text":"hello"}\n')
            for _ in range(300):
                if sim.streams:
                    break
                await asyncio.sleep(0.01)
        self.assertEqual(sim.streams, [P.psnd_encode(P.tidy(TONE))])


class _Char:
    def __init__(self, n: int):
        self.max_write_without_response_size = n


class _Peripheral:
    """CoreBluetooth's CBPeripheral, as far as BleLink looks at it."""

    def __init__(self, busy: int):
        self.busy, self.polls = busy, 0

    def canSendWriteWithoutResponse(self) -> bool:      # noqa: N802 (the Objective-C name)
        self.polls += 1
        return self.polls > self.busy


class _Client:
    """The part of bleak.BleakClient that BleLink uses."""

    def __init__(self, n: int = 247, busy: int = 0, mac: bool = True):
        chars = {P.OTA_DATA: _Char(n), P.NUS_RX: _Char(n)}
        self.services = type("S", (), {"get_characteristic": lambda _s, u: chars.get(u)})()
        self.chars = chars
        self._backend = type("B", (), {"_peripheral": _Peripheral(busy) if mac else None})()
        self.is_connected = True
        self.writes: list = []

    async def write_gatt_char(self, uuid, data, response):
        self.writes.append((uuid, bytes(data), response))


class BleLinkOffline(unittest.IsolatedAsyncioTestCase):
    """BleLink's own logic with a stand-in for bleak (no radio): flow control, sizes, matching."""

    async def test_waits_for_corebluetooth_before_each_data_write(self):
        link = BleLink()
        link._client = c = _Client(busy=3)
        await link.write(P.OTA_DATA, b"x", False)
        self.assertEqual(c._backend._peripheral.polls, 4)          # waited until it could send
        await link.write(P.OTA_CONTROL, b"\x0a", True)              # with response: no wait
        self.assertEqual(c._backend._peripheral.polls, 4)
        self.assertEqual(c.writes, [(P.OTA_DATA, b"x", False), (P.OTA_CONTROL, b"\x0a", True)])
        link._client = _Client(mac=False)                           # BlueZ / WinRT: no such property
        await link.write(P.OTA_DATA, b"y", False)
        self.assertEqual(link._client.writes, [(P.OTA_DATA, b"y", False)])

    def test_write_size_is_clamped(self):
        link = BleLink()
        link._client = c = _Client(n=514)
        self.assertEqual(link.max_write(P.OTA_DATA), 512)           # the pet takes at most 512 a write
        c.chars[P.OTA_DATA].max_write_without_response_size = 182
        self.assertEqual(link.max_write(P.OTA_DATA), 182)
        c.chars[P.OTA_DATA].max_write_without_response_size = 10
        self.assertEqual(link.max_write(P.OTA_DATA), 20)
        self.assertEqual(link.max_write(P.OTA_MIC), 20)             # unknown characteristic: the minimum
        self.assertTrue(link.has(P.NUS_RX) and not link.has(P.OTA_MIC))

    def test_pet_matching(self):
        m = _matcher("d350")
        self.assertTrue(m("Claude-D350", "x"))
        self.assertFalse(m("Claude-AD48", "x") or m("Other-D350", "x"))
        anyone = _matcher(None)
        self.assertTrue(anyone("Claude-AD48", "x"))
        self.assertFalse(anyone("Magic Keyboard", "x"))
        mac = _matcher("AA:BB:CC:DD:EE:FF")
        self.assertTrue(mac("", "aa:bb:cc:dd:ee:ff"))
        self.assertFalse(mac("Claude-EEFF", "11:22:33:44:55:66"))
        self.assertTrue(_matcher("12345678-9ABC-DEF0-1234-56789ABCDEF0")("?", "12345678-9abc-def0-1234-56789abcdef0"))
        self.assertTrue(_is_status(b'{"ota":"ack","off":1}'))
        self.assertFalse(_is_status(b'{"ver":"6ac87a5","slot":"ota_1"}'))

    async def test_disconnect_reaches_the_pet_once(self):
        link = BleLink()
        link._loop = asyncio.get_running_loop()
        link._client = client = _Client()
        lost = []
        link.on_lost = lambda: lost.append(1)
        link._disconnected(_Client())                               # an old client after a reconnect: ignored
        link._disconnected(client)
        await asyncio.sleep(0)
        self.assertEqual(lost, [1])
        link._closing = True                                        # our own disconnect is not a loss
        link._disconnected(client)
        await asyncio.sleep(0)
        self.assertEqual(lost, [1])


if __name__ == "__main__":
    unittest.main()
