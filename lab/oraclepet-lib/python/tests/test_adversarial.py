"""The transfer engine against a pet that misbehaves in ways ota.c allows: duplicate, stale,
late and lost acks, a "ready" for another size, windows smaller than one write, mic audio cut
into odd notification sizes, a zero-sample clip, an error with a job queued behind it.

Each case checks the rules of PROTOCOL.md 4.4: data only after "ready" and only on the data
characteristic without response, commands on control with response, never more than `window`
bytes beyond the last ack, exactly one END and only once ack.off == size. Offline: SimPet from
test_transfer, no Bluetooth.
"""
from __future__ import annotations

import asyncio
import os
import unittest

from oraclepet import protocol as P
from oraclepet.pet import Pet, PetError

from test_transfer import SimPet


class Watched(SimPet):
    """SimPet that records the order of everything the host does, and can garble its acks."""

    def __init__(self, *, ack_mode: str = "", **kw):
        super().__init__(**kw)
        self.ack_mode = ack_mode
        self.log: list[tuple] = []            # ("cmd", byte) / ("data", n, state)
        self.acks_sent = 0
        self.stray = 0                        # data bytes written outside a transfer

    def _control(self, cmd: bytes) -> None:
        self.log.append(("cmd", cmd[0]))
        super()._control(cmd)

    def _data(self, chunk: bytes) -> None:
        self.log.append(("data", len(chunk), self._state))
        if self._state != "receiving":
            self.stray += len(chunk)
        super()._data(chunk)

    def _ack(self, off: int) -> None:
        self.acks_sent += 1
        mode = self.ack_mode
        if mode == "lossy" and self.acks_sent % 2 == 0 and off < self._size:
            # the pet thinks it acked; the host never hears it. Not the final ack: ota.c sends
            # that once and never repeats it, so losing it leaves both sides waiting until the
            # pet's "no END from the host" (a protocol limit, not something a host can fix)
            self._last_ack = off
            return
        super()._ack(off)
        if mode == "chaos":
            super()._ack(off)                                  # a duplicate
            self._status(ota="ack", off=max(0, off - 4096))    # a stale, lower one
            loop = asyncio.get_running_loop()
            loop.call_later(0.02, lambda: self._status(ota="ack", off=off))   # a late one, maybe after END


class Adversarial(unittest.IsolatedAsyncioTestCase):
    async def make(self, **kw) -> tuple[Watched, Pet]:
        sim = Watched(**kw)
        pet = Pet(link=sim)
        pet.stall_s, pet.reboot_s, pet.settle_s, pet.reply_s, pet.listen_s, pet.tail_s = 0.4, 3.0, 0.01, 2.0, 1.0, 0.3
        await pet.open()
        self.addAsyncCleanup(sim.shutdown)
        return sim, pet

    def check_rules(self, sim: Watched, transfers: int) -> None:
        self.assertEqual(sim.stray, 0, "data outside a transfer")
        self.assertFalse(sim.overflowed)
        self.assertLessEqual(sim.max_in_flight, sim.window)
        self.assertEqual(sum(1 for e in sim.log if e == ("cmd", 0x02)), transfers, "one END per transfer")
        # no data between END and the next BEGIN
        open_ = False
        for e in sim.log:
            if e[0] == "cmd" and e[1] in (0x01, 0x04, 0x0B):
                open_ = True
            elif e == ("cmd", 0x02):
                open_ = False
            elif e[0] == "data":
                self.assertTrue(open_, "data after END")

    async def test_duplicate_stale_and_late_acks(self):
        sim, pet = await self.make(ack_mode="chaos", drain_delay=0.002)
        wasm = b"\0asm" + os.urandom(50_000)
        out = await pet.app(wasm, name="chaos")
        self.assertEqual(sim.fs["apps/chaos.wasm"], wasm)
        self.assertEqual(out.kind, "app")
        await asyncio.sleep(0.05)                              # the late acks land while idle
        self.check_rules(sim, 1)
        self.assertNotIn(P.ABORT, sim.commands)

    async def test_late_acks_do_not_disturb_the_next_job(self):
        sim, pet = await self.make(ack_mode="chaos")
        a, b = b"\0asm" + os.urandom(9_000), b"\0asm" + os.urandom(9_000)
        await asyncio.gather(pet.app(a, name="a", run=False), pet.app(b, name="b", run=False))
        self.assertEqual((sim.fs["apps/a.wasm"], sim.fs["apps/b.wasm"]), (a, b))
        self.check_rules(sim, 2)

    async def test_lost_acks_are_recovered_by_the_idle_re_ack(self):
        sim, pet = await self.make(ack_mode="lossy", window=4096)
        data = os.urandom(40_000)
        out = await pet.stream(P.psnd_encode([0] * 10))              # a tiny stream, then a big file
        self.assertEqual(out.kind, "streamed")
        files = {"idle.gif": data, "manifest.json": b"{}"}
        d = await self._pack(files)
        out = await pet.pack(d)
        self.assertEqual(sim.fs["characters/neo/idle.gif"], data)
        self.check_rules(sim, 3)

    async def _pack(self, files: dict):
        import tempfile
        from pathlib import Path
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        d = Path(tmp.name) / "neo"
        d.mkdir()
        for k, v in files.items():
            (d / k).write_bytes(v)
        return d

    async def test_window_smaller_than_one_write(self):
        for window in (1, 7, 100, 181):
            with self.subTest(window=window):
                sim, pet = await self.make(window=window, mtu=185)
                wasm = b"\0asm" + os.urandom(3_000 if window >= 100 else 40)   # one idle re-ack per window
                await pet.app(wasm, name="tiny", run=False)
                self.assertEqual(sim.fs["apps/tiny.wasm"], wasm)
                self.assertLessEqual(sim.max_chunk, window)
                self.check_rules(sim, 1)

    async def test_a_ready_for_another_size_is_ignored(self):
        sim, pet = await self.make()
        real = sim._status

        def wrong_size(**o):
            if o.get("ota") == "ready":
                o = {**o, "size": o["size"] + 1}
            real(**o)

        sim._status = wrong_size
        # the pet re-acks 0 while it waits, which keeps the host's stall timer quiet, so the
        # pet's own idle timeout ends it (as with the Mac app)
        with self.assertRaisesRegex(PetError, "^Pet refused the update: timed out waiting for data$"):
            await pet.app(b"\0asm" + bytes(100), name="x", run=False)
        self.assertEqual(sum(e[1] for e in sim.log if e[0] == "data"), 0)   # nothing was sent

    async def test_an_error_lets_the_queued_job_run(self):
        # PetLink: "error" fails this job only; otaPending still runs (unlike a stall or cancel)
        _sim, pet = await self.make()
        first = pet.play("nosuch")
        second = pet.play("say")
        r1, r2 = await asyncio.gather(first, second, return_exceptions=True)
        self.assertIsInstance(r1, PetError)
        self.assertEqual(str(r1), "Pet refused the update: no sound nosuch")
        self.assertEqual(r2.kind, "play")

    async def test_zero_sample_clip(self):
        sim, pet = await self.make()
        out = await pet.stream([])
        self.assertEqual(sim.streams, [P.psnd_encode([])])
        self.assertEqual(len(sim.streams[0]), 16)
        self.assertEqual(out.kind, "streamed")
        self.check_rules(sim, 1)

    async def test_mic_stream_in_odd_notification_sizes(self):
        # one byte stream: the 16-byte header and the blocks need not line up with notifications
        sizes = [1, 7, 15, 2, 250, 20, 3, 256, 509, 13]
        for ms in (100, 1009, 3000):            # 1600, 16144, 48000 samples
            with self.subTest(ms=ms):
                sim, pet = await self.make()

                async def listen(ms_: int, send: bool, sim=sim) -> None:
                    await asyncio.sleep(0.01)
                    n = 16 * ms_
                    pcm = [((i * 7919) % 20001) - 10000 for i in range(n)]
                    clip = P.psnd_encode(pcm)
                    sim.last_listen = clip
                    off = k = 0
                    while off < len(clip):
                        step = sizes[k % len(sizes)]
                        sim._notify(P.OTA_MIC, clip[off:off + step])
                        off, k = off + step, k + 1
                    sim._status(ota="heard", ms=ms_, samples=n, rms=1, peak=2, freq=150, db=0.5, floor=0, sent=True)

                sim._listen = listen
                levels, samples = await pet.listen(ms)
                self.assertEqual(levels["samples"], 16 * ms)
                self.assertEqual(samples, P.psnd_decode(sim.last_listen))
                self.assertEqual(len(samples), 16 * ms)

    async def test_heartbeat_thai_split_at_20_bytes(self):
        sim, pet = await self.make(mtu=23)
        line = await pet.heartbeat(total=1, running=1, waiting=0, msg="กำลังทำงาน \"ด่วน\" \\ ok", who="แมวน้อย",
                                   epoch=1, tzoff=25200)
        self.assertEqual(bytes(sim.rx), line)
        self.assertTrue(all(n <= 20 for n in sim.rx_writes))
        self.assertTrue(line.endswith(b"\n"))
        self.assertEqual(line.count(b"\n"), 1)


if __name__ == "__main__":
    unittest.main()
