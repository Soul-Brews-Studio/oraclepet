"""Pet: one OraclePet over Bluetooth LE, driven the way the OraclePet Mac app drives it.

The transfer engine follows lab/desk-pet-mac/Sources/PetCore/PetLink.swift step for step:
BEGIN / BEGIN_FILE / BEGIN_STREAM, wait for "ready", write-without-response chunks of the
negotiated size never more than `window` bytes past the last ack, PROGRESS for packs, apps and
streams, END once every byte is acked, then the outcome ("saved", "streamed", "done" and the
reboot, "pack", "app", "play"). No progress for 20 s aborts the job and the jobs queued behind
it; a job asked for while another runs waits its turn. Messages are the Mac app's.

Bluetooth sits behind a small Link interface. BleLink is bleak (macOS, Linux, Windows); the
tests plug in a simulated pet, so the engine is tested without a radio.
"""
from __future__ import annotations

import asyncio
import collections
import logging
import re
import sys
from collections.abc import Callable, Sequence
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from . import protocol as P

log = logging.getLogger("oraclepet")

NO_OTA = "The pet's firmware has no OTA service yet: flash it once over USB"
LINK_LOST = "Link lost during the update; the pet keeps what it had"


class PetError(Exception):
    """Something the pet or this host refused. The message is the one the Mac app shows."""

    def __init__(self, message: str, *, samples: list | None = None):
        super().__init__(message)
        self.samples = samples          # listen(): audio that arrived before the failure


@dataclass
class Found:
    """A pet seen while scanning."""
    name: str
    address: str                        # a MAC on Linux/Windows, a CoreBluetooth UUID on macOS
    rssi: int | None = None
    device: Any = field(default=None, repr=False)


@dataclass
class Outcome:
    """How a job ended: kind is "updated", "saved", "pack", "app", "play" or "streamed"."""
    kind: str
    message: str
    bytes: int = 0
    seconds: float = 0.0
    details: dict = field(default_factory=dict)

    def __str__(self) -> str:
        return self.message


@dataclass
class Progress:
    """Where a job is, for a progress bar (on_progress)."""
    phase: str                          # "starting", "sending", "finishing", "rebooting"
    overall: float                      # 0..1 of the batch: this job and the jobs queued behind it
    part: float                         # 0..1 of the current file
    acked: int                          # bytes of this job the pet has acked
    total: int                          # bytes in this job
    file_index: int
    file_count: int
    path: str | None                    # None = app image, "@stream", or the LittleFS path


# ---------------------------------------------------------------------------------------------
# Link: the GATT operations a Pet needs
# ---------------------------------------------------------------------------------------------
class Link:
    """What a Pet needs from a connection. Notification callbacks run on the event loop;
    on_lost is called (on the loop) when the link drops."""

    name: str = ""
    address: str = ""
    on_lost: Callable[[], None] | None = None

    @property
    def connected(self) -> bool:
        raise NotImplementedError

    def has(self, uuid: str) -> bool:
        raise NotImplementedError

    def max_write(self, uuid: str) -> int:
        """Largest write-without-response payload (negotiated ATT MTU - 3)."""
        raise NotImplementedError

    async def read(self, uuid: str) -> bytes:
        raise NotImplementedError

    async def write(self, uuid: str, data: bytes, response: bool) -> None:
        raise NotImplementedError

    async def subscribe(self, uuid: str, callback: Callable[[bytes], None]) -> None:
        raise NotImplementedError

    async def reconnect(self, timeout: float) -> None:
        """Find the same pet again (after its reboot) and restore the subscriptions."""
        raise NotImplementedError

    async def close(self) -> None:
        raise NotImplementedError


_ADDRESS = re.compile(r"([0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2}|[0-9A-Fa-f]{8}(-[0-9A-Fa-f]{4}){3}-[0-9A-Fa-f]{12}")


def _matcher(target: str | None) -> Callable[[str, str], bool]:
    """--pet: an address (MAC, or macOS UUID), else a name suffix ("D350"); None = any pet."""
    t = (target or "").strip()
    if _ADDRESS.fullmatch(t):
        return lambda name, address: address.lower() == t.lower()
    suffix = t.upper()
    return lambda name, address: name.startswith(P.NAME_PREFIX) and name.upper().endswith(suffix)


def _adv_name(device: Any, adv: Any) -> str:
    return getattr(adv, "local_name", None) or getattr(device, "name", None) or ""


async def scan(timeout: float = 5.0, target: str | None = None) -> list[Found]:
    """Every pet advertising "Claude-*" (a connected pet does not advertise), strongest first."""
    from bleak import BleakScanner

    match = _matcher(target)
    seen = await BleakScanner.discover(timeout=timeout, return_adv=True)
    pets = [Found(_adv_name(d, a), d.address, getattr(a, "rssi", None), d)
            for d, a in seen.values() if match(_adv_name(d, a), d.address)]
    return sorted(pets, key=lambda f: -(f.rssi if f.rssi is not None else -999))


async def find_pet(target: str | None = None, timeout: float = 10.0) -> Found | None:
    """The first pet that matches (by suffix or address), or None after timeout."""
    from bleak import BleakScanner

    match = _matcher(target)
    names: dict[str, str] = {}

    def want(d: Any, a: Any) -> bool:
        names[d.address] = _adv_name(d, a)
        return match(names[d.address], d.address)

    d = await BleakScanner.find_device_by_filter(want, timeout=timeout)
    return None if d is None else Found(names.get(d.address, d.name or ""), d.address, None, d)


def _is_status(value: bytes) -> bool:
    return bytes(value[:6]) == b'{"ota"'


class BleLink(Link):
    """A Link over bleak.

    macOS: CoreBluetooth drops a write-without-response when its queue is full, and bleak does
    not wait, so every data write first waits for canSendWriteWithoutResponse, as the Mac app's
    pump does. Control reads and notifications share one CoreBluetooth callback; status lines
    start {"ota", the read reply {"ver", which tells them apart.
    """

    def __init__(self) -> None:
        self._client: Any = None
        self._subs: dict[str, Callable[[bytes], None]] = {}
        self._closing = False
        self._loop: asyncio.AbstractEventLoop | None = None

    @classmethod
    async def open(cls, target: str | None = None, timeout: float = 10.0) -> BleLink:
        found = await find_pet(target, timeout)
        if found is None:
            what = f"pet matching {target!r}" if target else f"{P.NAME_PREFIX}* pet"
            raise PetError(f"No {what} found. Is it on, and free? A pet takes one host at a time: "
                           "disconnect it in the OraclePet app (or any other host) first.")
        link = cls()
        await link._connect(found, timeout)
        return link

    async def _connect(self, found: Found, timeout: float) -> None:
        from bleak import BleakClient

        self._loop = asyncio.get_running_loop()
        client = BleakClient(found.device, disconnected_callback=self._disconnected, timeout=timeout)
        await client.connect()
        self._client = client
        self.name = found.name or self.name
        self.address = found.address
        await self._wait_mtu()
        for uuid, callback in self._subs.items():
            await self._start_notify(uuid, callback)
        log.info("connected to %s (%s), write size %d", self.name, self.address, self.max_write(P.NUS_RX))

    def _disconnected(self, client: Any) -> None:
        if client is not self._client or self._closing:
            return
        if self._loop is not None and not self._loop.is_closed():
            self._loop.call_soon_threadsafe(self._lost)

    def _lost(self) -> None:
        if self.on_lost is not None:
            self.on_lost()

    async def _wait_mtu(self) -> None:
        # the MTU exchange may finish after connect; until then the write size reads 20
        ch = self._char(P.OTA_DATA) or self._char(P.NUS_RX)
        for _ in range(20):
            if ch is None or ch.max_write_without_response_size > 20:
                return
            await asyncio.sleep(0.05)

    def _char(self, uuid: str) -> Any:
        try:
            return self._client.services.get_characteristic(uuid)
        except Exception:
            return None

    @property
    def connected(self) -> bool:
        return self._client is not None and self._client.is_connected

    def has(self, uuid: str) -> bool:
        return self._client is not None and self._char(uuid) is not None

    def max_write(self, uuid: str) -> int:
        ch = self._char(uuid)
        n = ch.max_write_without_response_size if ch is not None else 20
        return max(20, min(512, n))         # the pet flattens a write into 520 bytes; ATT caps at 512

    async def read(self, uuid: str) -> bytes:
        return bytes(await self._client.read_gatt_char(uuid))

    async def write(self, uuid: str, data: bytes, response: bool) -> None:
        if not response:
            await self._wait_can_send()
        await self._client.write_gatt_char(uuid, data, response=response)

    async def _wait_can_send(self) -> None:
        peripheral = getattr(getattr(self._client, "_backend", None), "_peripheral", None)
        can_send = getattr(peripheral, "canSendWriteWithoutResponse", None)     # macOS only
        if can_send is None:
            return
        loop = asyncio.get_running_loop()
        deadline = loop.time() + 2.0
        while not can_send():
            if loop.time() > deadline:
                return                      # write anyway: the window and the CRC catch a drop
            await asyncio.sleep(0.001)

    async def subscribe(self, uuid: str, callback: Callable[[bytes], None]) -> None:
        self._subs[uuid] = callback
        await self._start_notify(uuid, callback)

    async def _start_notify(self, uuid: str, callback: Callable[[bytes], None]) -> None:
        def handler(_char: Any, data: bytearray) -> None:
            callback(bytes(data))

        if uuid == P.OTA_CONTROL and sys.platform == "darwin":
            try:
                await self._client.start_notify(uuid, handler, cb={"notification_discriminator": _is_status})
                return
            except TypeError:               # bleak < 1.0 has no discriminator
                pass
        await self._client.start_notify(uuid, handler)

    async def reconnect(self, timeout: float) -> None:
        old, self._client = self._client, None
        if old is not None:
            try:
                await old.disconnect()
            except Exception:
                pass
        found = await find_pet(self.address or self.name, timeout)
        if found is None:
            raise PetError(f"{self.name or 'The pet'} is not advertising")
        await self._connect(found, timeout)

    async def close(self) -> None:
        self._closing = True
        if self._client is not None:
            try:
                await self._client.disconnect()
            except Exception:
                pass


# ---------------------------------------------------------------------------------------------
# Pet
# ---------------------------------------------------------------------------------------------
_LOST = object()                        # inbox markers: the link dropped / cancel() was called
_CANCEL = object()


class _Stalled(Exception):
    pass


class _Lost(Exception):
    pass


class _Cancelled(Exception):
    pass


class _Ticket:
    """A job waiting for the pet (PetLink.otaPending): its bytes count toward the batch bar."""

    def __init__(self, nbytes: int):
        self.bytes = nbytes


class _Inbox:
    """Control notifications in arrival order, for the one job that runs at a time."""

    def __init__(self) -> None:
        self._items: collections.deque = collections.deque()
        self._event = asyncio.Event()

    def put(self, item: Any) -> None:
        self._items.append(item)
        self._event.set()

    def clear(self) -> None:
        self._items.clear()
        self._event.clear()

    async def get(self, timeout: float) -> Any:
        loop = asyncio.get_running_loop()
        deadline = loop.time() + timeout
        while not self._items:
            self._event.clear()
            left = deadline - loop.time()
            if left <= 0:
                raise asyncio.TimeoutError
            await asyncio.wait_for(self._event.wait(), left)
        return self._items.popleft()


def _load(x: Any) -> bytes:
    if isinstance(x, (bytes, bytearray, memoryview)):
        return bytes(x)
    return Path(x).read_bytes()


class Pet:
    """One OraclePet.

        async with Pet("D350") as pet:          # name suffix, address, or None = the first pet
            print(await pet.info())
            print(await pet.say("hello"))

    Jobs (ota, pack, app, run, stop_app, select_pack, play, stream, say) run one at a time;
    one asked for while another runs waits its turn, as in the Mac app.
    """

    stall_s = 20.0          # no progress from the pet for this long: ABORT, the job fails
    reboot_s = 60.0         # after "done" the pet must be back on the new image within this
    settle_s = 0.5          # a queued job starts this long after the previous one settled
    reply_s = 20.0          # selftest reply
    listen_s = 10.0         # LISTEN: quiet allowed after the recording, and between audio chunks
    tail_s = 3.0            # LISTEN: "heard" follows the last audio block at once

    def __init__(self, target: str | None = None, *, link: Link | None = None, timeout: float = 10.0,
                 speak_events: bool = False):
        self.target = target
        self.link = link
        self.timeout = timeout
        self.speak_events = speak_events            # pet.say() in a wasm app -> speak it, as the Mac does
        self.firmware: P.PetFirmware | None = None
        self.on_progress: Callable[[Progress], None] | None = None
        self.events: collections.deque = collections.deque(maxlen=100)   # recent {"evt":...} lines
        self._event_callbacks: list[Callable[[dict], None]] = []
        self._tx = bytearray()
        self._inbox = _Inbox()
        self._lock = asyncio.Lock()
        self._gen = 0                               # bumped by cancel(): queued jobs are dropped
        self._pending: list[_Ticket] = []
        self._tasks: set = set()
        self._unflushed = False
        # batch bookkeeping, as PetLink: one overall % across a job and the ones queued behind it
        self._in_batch = False
        self._batch_total = 0
        self._batch_done = 0
        self._job_index = 0
        self._job_count = 0
        self._last_pct = -1
        # the running job
        self._phase = "idle"
        self._job_total = 0
        self._bytes_done = 0
        self._acked = 0
        self._part = 0.0
        self._file_index = 0
        self._file_count = 0
        self._path: str | None = None
        self._started = 0.0
        self._first_ack: float | None = None
        self._last_progress = 0.0
        # LISTEN audio, as it arrives on the mic characteristic
        self._mic = bytearray()
        self._mic_at: float | None = None

    # ---- connection ----
    async def __aenter__(self) -> Pet:
        await self.open()
        return self

    async def __aexit__(self, *exc: Any) -> None:
        await self.close()

    async def open(self) -> Pet:
        """Connect (unless a link was given), subscribe, and read what the pet runs."""
        if self.link is None:
            self.link = await BleLink.open(self.target, self.timeout)
        link = self.link
        link.on_lost = self._link_lost
        try:
            if link.has(P.NUS_TX):
                await link.subscribe(P.NUS_TX, self._on_tx)
            if link.has(P.OTA_CONTROL):
                await link.subscribe(P.OTA_CONTROL, self._on_control)
                if link.has(P.OTA_MIC):
                    await link.subscribe(P.OTA_MIC, self._on_mic)
                try:
                    await self.info()
                except Exception as e:
                    log.warning("could not read what the pet runs: %s", e)
        except BaseException:
            await link.close()                      # do not leave the pet held by a half-open link
            raise
        return self

    async def close(self) -> None:
        for t in list(self._tasks):
            t.cancel()
        if self.link is not None and self.link.connected and self._unflushed:
            await self.flush()
        if self.link is not None:
            await self.link.close()

    async def flush(self) -> None:
        """Make sure writes-without-response (heartbeats) left before disconnecting: a read is
        answered only after them. Without an OTA service, wait half a second instead."""
        self._unflushed = False
        try:
            if self.link.has(P.OTA_CONTROL):
                await asyncio.wait_for(self.link.read(P.OTA_CONTROL), 3)
                return
        except Exception:
            pass
        await asyncio.sleep(0.5)

    @property
    def name(self) -> str:
        return self.link.name if self.link else ""

    @property
    def address(self) -> str:
        return self.link.address if self.link else ""

    def on_event(self, callback: Callable[[dict], None]) -> None:
        """Call back with each {"evt":"tap"} / {"evt":"say","text":...} the pet sends."""
        self._event_callbacks.append(callback)

    def cancel(self) -> None:
        """Drop the queued jobs and abort the running one (PetLink.cancelFirmwareUpdate)."""
        self._gen += 1
        if self._lock.locked():
            self._inbox.put(_CANCEL)

    # ---- notifications ----
    def _on_control(self, data: bytes) -> None:
        st = P.parse_status(data)
        if st is not None:
            log.debug("pet  %s", data.decode("utf-8", "replace"))
            self._inbox.put(st)
            return
        fw = P.PetFirmware.parse(data)              # a read reply delivered as a notification
        if fw is not None:
            self.firmware = fw

    def _on_mic(self, data: bytes) -> None:
        self._mic += data
        self._mic_at = asyncio.get_running_loop().time()

    def _on_tx(self, data: bytes) -> None:
        for evt in P.event_lines(self._tx, data):
            log.debug("event %s", evt)
            self.events.append(evt)
            for cb in list(self._event_callbacks):
                try:
                    cb(evt)
                except Exception:
                    log.exception("event callback failed")
            if self.speak_events and evt.get("evt") == "say" and isinstance(evt.get("text"), str):
                self._spawn(self.say(evt["text"]))

    def _link_lost(self) -> None:
        log.info("link lost")
        self._inbox.put(_LOST)

    def _spawn(self, coro: Any) -> None:
        task = asyncio.ensure_future(coro)
        self._tasks.add(task)

        def done(t: asyncio.Task) -> None:
            self._tasks.discard(t)
            if not t.cancelled() and t.exception() is not None:
                log.warning("%s", t.exception())

        task.add_done_callback(done)

    # ---- reading what the pet runs ----
    async def info(self) -> P.PetFirmware:
        """Read the control characteristic: firmware, board, packs, apps (PROTOCOL.md 4.1)."""
        if self.link is None or not self.link.has(P.OTA_CONTROL):
            raise PetError(NO_OTA)
        for _ in range(3):
            data = await self.link.read(P.OTA_CONTROL)
            fw = P.PetFirmware.parse(data)
            if fw is not None:
                self.firmware = fw
                return fw
            self._on_control(data)                  # a status line that raced the read
        raise PetError("The pet did not say what it runs")

    # ---- jobs (PetLink.startJob) ----
    async def ota(self, image: bytes | str | Path) -> Outcome:
        """Full OTA: send an app image; the pet reboots into it. Refuses another board's image."""
        data = _load(image)
        name = P.project_name(data)
        if name is None:
            raise PetError("Not an ESP32 app image")
        if name not in P.PET_PROJECTS:
            raise PetError(f"Not a pet firmware: {name}")
        mine = self.firmware.project if self.firmware else ""
        if mine and mine != name:           # each board has its own project; the pet refuses the other's
            raise PetError(f"That firmware is for the {P.board_name(name)} board; this pet is {P.board_name(mine)}")
        return await self._job([(None, data)])

    async def pack(self, folder: str | Path, name: str | None = None) -> Outcome:
        """Partial OTA: copy an animation pack (manifest.json + GIFs) to the pet and show it."""
        d = Path(folder)
        if not (d / "manifest.json").is_file():
            raise PetError(f"{d.name} is not a pack folder (manifest.json + GIFs)")
        name = (name or d.name).lower()
        if not P.valid_name(name):
            raise PetError(f"{name!r} is not a pack name: a-z 0-9 - _, at most 23")
        jobs = []
        for f in sorted((f for f in d.iterdir() if f.is_file() and f.suffix in (".gif", ".json")),
                        key=lambda f: f.name):
            path = f"characters/{name}/{f.name}"
            if not P.pet_accepts_path(path):
                raise PetError(f"{f.name}: the pet takes lower-case file names only (a-z 0-9 - _ .)")
            jobs.append((path, f.read_bytes()))
        # manifest last: the pet skips a pack without one until it is complete
        jobs = [j for j in jobs if not j[0].endswith("/manifest.json")] + \
               [j for j in jobs if j[0].endswith("/manifest.json")]
        return await self._job(jobs, select=name)

    async def app(self, wasm: bytes | str | Path, run: bool = True, name: str | None = None) -> Outcome:
        """Partial OTA of a pet app: copy the .wasm to apps/<name>.wasm and (run) start it."""
        stem = None if isinstance(wasm, (bytes, bytearray, memoryview)) else Path(wasm).stem
        data = _load(wasm)
        name = (name or stem or "").lower()
        if not P.valid_name(name):
            raise PetError(f"{name!r} is not an app name: a-z 0-9 - _, at most 23")
        if data[:4] != b"\0asm":
            raise PetError(f"{name} is not a WebAssembly module")
        return await self._job([(f"apps/{name}.wasm", data)], run=name if run else None)

    async def run(self, name: str) -> Outcome:
        """Run an app already on the pet."""
        if not P.valid_name(name):
            raise PetError(f"{name!r} is not an app name")
        return await self._job([], run=name)

    async def stop_app(self) -> Outcome:
        return await self._job([], run="")

    async def select_pack(self, name: str) -> Outcome:
        """Show a pack already on the pet."""
        if not P.valid_name(name):
            raise PetError(f"{name!r} is not a pack name")
        return await self._job([], select=name)

    async def play(self, name: str) -> Outcome:
        """Play sounds/<name>.ima from the pet's storage."""
        if not P.valid_name(name):
            raise PetError(f"{name!r} is not a sound name")
        return await self._job([], play=name)

    async def stream(self, clip: bytes | Sequence[int], saying: str = "") -> Outcome:
        """Stream a PSND clip (or 16 kHz int16 samples); the pet plays it while it arrives."""
        if not isinstance(clip, (bytes, bytearray, memoryview)):
            clip = P.psnd_encode(list(clip))
        clip = bytes(clip)
        if P.psnd_header(clip) is None:
            raise PetError("Not a PSND clip")
        return await self._job([(P.STREAM_PATH, clip)], saying=saying)

    async def say(self, text: str, language: str | None = None, voice: str | None = None) -> Outcome:
        """Speak through the pet: this OS's speech -> 16 kHz -> tidy -> PSND -> stream.
        language "th"/"en" (or th-TH, en-US); None = Thai when the text has Thai."""
        from . import tts

        t = text.strip()
        if not t:
            raise PetError("Nothing to say")
        t = "".join(P.graphemes(t)[:160])
        try:
            pcm = await asyncio.to_thread(tts.synthesize, t, language, voice)
        except tts.SpeechError as e:
            raise PetError(str(e)) from e
        pcm = P.tidy(pcm)
        if not pcm:
            raise PetError("The speech came out silent")
        return await self.stream(P.psnd_encode(pcm), saying=t)

    # ---- not transfers: selftest, listen, heartbeat ----
    async def selftest(self) -> dict:
        """The pet beeps 1 kHz; a pet with a mic says whether it heard it. Adds "summary"."""
        async with self._lock:
            self._require_control()
            self._inbox.clear()
            await self._try_command(P.SELFTEST)
            st = await self._reply("selftest", self.reply_s)
        r = {k: v for k, v in st.items() if k != "kind"}
        if not r["mic"]:
            r["summary"] = "Beeped (this pet has no mic to check itself)"
        elif r["heard"]:
            r["summary"] = f"Heard by its mic: {r['db']:+.1f} dB at 1 kHz (rms {r['tone']} vs {r['quiet']} quiet)"
        else:
            r["summary"] = f"Its mic heard nothing ({r['db']:+.1f} dB): speaker or amp off"
        return r

    async def listen(self, ms: int = 3000, send_audio: bool = True) -> tuple[dict, list | None]:
        """Record the pet's mic for ms (100..10000). Returns the "heard" metrics (ms, samples,
        rms, peak, freq, db, floor) and, with send_audio, the decoded int16 samples."""
        ms = max(100, min(10_000, int(ms)))
        async with self._lock:
            self._require_control()
            audio = send_audio and self.link.has(P.OTA_MIC)
            self._inbox.clear()
            self._mic = bytearray()
            self._mic_at = None
            await self._try_command(P.listen(ms, audio))
            loop = asyncio.get_running_loop()
            deadline = loop.time() + ms / 1000 + self.listen_s
            heard = None
            while heard is None:
                if self._mic_at is not None:        # the audio is coming: keep waiting while it flows
                    deadline = max(deadline, self._mic_at + self.listen_s)
                    if self._mic_complete():
                        deadline = min(deadline, self._mic_at + self.tail_s)
                left = deadline - loop.time()
                if left <= 0:
                    break
                try:
                    item = await self._inbox.get(min(left, 0.2))
                except asyncio.TimeoutError:
                    continue
                if item is _LOST:
                    raise PetError("Link lost while listening", samples=self._mic_samples())
                if item is _CANCEL:
                    raise PetError("Cancelled")
                if item["kind"] == "heard":
                    heard = item
                elif item["kind"] == "error":       # the JC3248 has no mic: {"why":"no mic"}
                    why = item["why"]
                    raise PetError("This pet has no mic" if why == "no mic" else f"Pet could not listen: {why}")
            if heard is not None and audio and heard.get("sent", True):
                end = loop.time() + 1.0             # the audio precedes "heard"; allow a straggler
                while not self._mic_complete() and loop.time() < end:
                    await asyncio.sleep(0.05)
        samples = self._mic_samples() if audio else None
        if heard is None:
            got = f" ({len(samples)} samples of audio arrived)" if samples else ""
            raise PetError(f"No 'heard' reply from the pet{got}", samples=samples)
        return {k: v for k, v in heard.items() if k != "kind"}, samples

    def _mic_complete(self) -> bool:
        head = P.psnd_header(bytes(self._mic[:16]))
        return head is not None and len(self._mic) >= P.psnd_size(head[1])

    def _mic_samples(self) -> list | None:
        return P.psnd_decode(bytes(self._mic)) if self._mic else None

    async def heartbeat(self, beat: P.Heartbeat | None = None, **fields: Any) -> bytes:
        """Send one heartbeat line on NUS RX (chunked to the write size); returns the bytes.
        Pass a Heartbeat, or its fields: heartbeat(total=1, running=1, waiting=0, msg="hi")."""
        if beat is None:
            beat = P.Heartbeat(**fields)
        elif fields:
            raise TypeError("pass a Heartbeat or its fields, not both")
        if self.link is None or not self.link.connected:
            raise PetError("Not connected to the pet")
        line = beat.json_line()
        n = self.link.max_write(P.NUS_RX)
        for i in range(0, len(line), n):
            await self.link.write(P.NUS_RX, line[i:i + n], False)
        self._unflushed = True
        return line

    # ---- the engine ----
    def _require_control(self) -> None:
        if self.link is None or not self.link.connected:
            raise PetError("Not connected to the pet")
        if not self.link.has(P.OTA_CONTROL):
            raise PetError(NO_OTA)

    def _require_ota(self) -> None:
        self._require_control()
        if not self.link.has(P.OTA_DATA):
            raise PetError(NO_OTA)

    async def _command(self, data: bytes) -> None:
        log.debug("host %s", data.hex())
        await self.link.write(P.OTA_CONTROL, data, True)

    async def _try_command(self, data: bytes) -> None:
        try:
            await self._command(data)
        except Exception as e:              # ATT "request not supported": older firmware
            raise PetError(f"The pet did not take the command ({e}); its firmware may predate it") from e

    async def _reply(self, kind: str, timeout: float) -> dict:
        loop = asyncio.get_running_loop()
        deadline = loop.time() + timeout
        while True:
            try:
                item = await self._inbox.get(max(0.0, deadline - loop.time()))
            except asyncio.TimeoutError:
                raise PetError(f"No {kind} reply from the pet in {timeout:g} s") from None
            if item is _LOST:
                raise PetError("Link lost")
            if item is _CANCEL:
                raise PetError("Cancelled")
            if item["kind"] == kind:
                return item
            if item["kind"] == "error":
                raise PetError(item["why"])

    async def _job(self, files: list, *, select: str | None = None, run: str | None = None,
                   play: str | None = None, saying: str = "") -> Outcome:
        """One job: files to send (path None = app image, "@stream" = live clip), then the pack
        to select, the app to run, the sound to play. Waits its turn behind a running job."""
        nbytes = sum(len(d) for _, d in files)
        gen = self._gen
        ticket = _Ticket(nbytes)
        queued = self._lock.locked()
        if queued:                                  # busy: wait, counted in the running batch
            self._pending.append(ticket)
            self._batch_total += nbytes
            self._job_count = self._job_index + len(self._pending)
            log.info("busy, queued (%d waiting)", len(self._pending))
        try:
            async with self._lock:
                self._pending = [t for t in self._pending if t is not ticket]
                try:
                    if gen != self._gen:            # cancel() or a stall dropped the queue
                        raise PetError("Update cancelled")
                    if queued:
                        await asyncio.sleep(self.settle_s)
                    self._require_ota()
                except BaseException:
                    if not self._pending:           # nothing ran: the batch ends here
                        self._in_batch = False
                    raise
                return await self._start(files, nbytes, select, run, play, saying)
        finally:
            self._pending = [t for t in self._pending if t is not ticket]

    async def _start(self, files: list, nbytes: int, select: str | None, run: str | None,
                     play: str | None, saying: str) -> Outcome:
        loop = asyncio.get_running_loop()
        if not self._in_batch:                      # a new batch: this job plus anything already queued
            self._batch_done = 0
            self._batch_total = nbytes + sum(t.bytes for t in self._pending)
        self._job_index = 1 if not self._pending and not self._in_batch else self._job_index + 1
        self._job_count = self._job_index + len(self._pending)
        self._in_batch = True
        self._file_index, self._file_count = 0, len(files)
        self._job_total, self._bytes_done, self._acked, self._part = nbytes, 0, 0, 0.0
        self._first_ack = None
        self._started = self._last_progress = loop.time()
        self._phase = "starting"
        before = self.firmware
        self._inbox.clear()
        try:
            return await self._run(files, select, run, play, saying, before)
        except _Stalled:
            self._gen += 1                          # like cancelFirmwareUpdate: the queue goes too
            await self._abort()
            raise PetError(f"Update stalled: no progress from the pet for {self.stall_s:g} s") from None
        except _Cancelled:
            await self._abort()
            raise PetError("Update cancelled") from None
        except _Lost:
            raise PetError(LINK_LOST) from None
        except asyncio.CancelledError:
            await self._abort()
            raise
        except PetError:
            raise
        except Exception as e:                      # a Bluetooth error mid-job
            if not self.link.connected:
                raise PetError(LINK_LOST) from e
            await self._abort()                     # free the pet now, not after its 15 s timeout
            raise PetError(f"Bluetooth error during the update: {e}") from e
        finally:
            self._batch_done += self._job_total     # this job counts as done for the batch bar
            self._acked = 0
            self._phase = "idle"
            if not self._pending:
                self._in_batch = False

    async def _run(self, files: list, select: str | None, run: str | None, play: str | None,
                   saying: str, before: P.PetFirmware | None) -> Outcome:
        for path, data in files:
            st = await self._send(path, data)
            self._bytes_done += len(data)
            if st["kind"] == "streamed":
                return self._streamed(st, saying)
            if st["kind"] == "done":
                return await self._rebooted(before)
        if select is not None:
            self._phase = "finishing"
            await self._command(P.select_pack(select))
            st = await self._expect("pack")
            await self._refresh()                   # the pack list changed
            if run is None and play is None:
                return self._outcome("pack", f"Pet now shows {st['name']} · {self._job_total // 1024} KB in "
                                             f"{int(self._elapsed())} s", name=st["name"])
        if run is not None:
            self._phase = "finishing"
            await self._command(P.run_app(run))
            st = await self._expect("app")
            await self._refresh()
            if play is None:
                sent = f" · {self._job_total} bytes in {int(self._elapsed())} s" if self._job_total > 0 else ""
                return self._outcome("app", f"Pet runs {st['name']}{sent}" if st["name"] else "App stopped",
                                     name=st["name"])
        if play is not None:
            self._phase = "finishing"
            await self._command(P.play(play))
            st = await self._expect("play")
            msg = (f"Pet says “{saying}” · {self._job_total // 1024} KB in {self._elapsed():.1f} s"
                   if saying else "Pet plays the sound")
            return self._outcome("play", msg, name=st["name"], clip_bytes=st["bytes"])
        return self._outcome("saved", f"Sent {self._job_total // 1024} KB in {int(self._elapsed())} s")

    async def _send(self, path: str | None, data: bytes) -> dict:
        """One transfer: BEGIN*, ready, windowed data, acks, END; returns saved/streamed/done."""
        loop = asyncio.get_running_loop()
        self._file_index += 1
        self._part = 0.0
        self._path = path
        self._phase = "starting"
        self._first_ack = None
        self._last_progress = loop.time()
        self._report()
        crc = P.crc32(data)
        if path == P.STREAM_PATH:
            cmd = P.begin_stream(len(data), crc)
        elif path is not None:
            cmd = P.begin_file(len(data), crc, path)
        else:
            cmd = P.begin(len(data), crc)
        log.info("OTA: %s %d bytes", path or "app image", len(data))
        await self._command(cmd)
        win: P.OtaWindow | None = None
        while True:
            try:
                st = await self._next()
            except _Lost:
                if path is None and self._phase == "finishing":
                    return {"kind": "done"}         # END went out; the "done" notify may not have
                raise
            kind = st["kind"]
            if kind == "ready":
                if self._phase != "starting" or st["size"] != len(data):
                    continue
                win = P.OtaWindow(len(data), st["window"])
                self._phase = "sending"
                self._report()
                if win.all_acked:                   # nothing to send, so no ack will come
                    await self._end()
                else:
                    await self._pump(win, data)
            elif kind == "ack":
                if self._phase != "sending" or win is None:
                    continue
                win.ack(st["off"])
                if self._first_ack is None and st["off"] > 0:
                    self._first_ack = loop.time()
                self._acked = self._bytes_done + win.acked
                self._part = win.fraction
                pct = int(self._overall() * 100)
                if (path is not None or self._job_count > 1) and pct != self._last_pct:
                    self._last_pct = pct            # packs, apps, streams: the pet draws the same %
                    await self._command(P.progress(pct))
                self._report()
                if win.all_acked:
                    await self._end()
                else:
                    await self._pump(win, data)
            elif kind in ("saved", "streamed", "done"):
                return st
            elif kind == "error":
                raise PetError(f"Pet refused the update: {st['why']}")

    async def _pump(self, win: P.OtaWindow, data: bytes) -> None:
        """Fill the window: as many chunks of the write size as the pet's acks allow."""
        n = self.link.max_write(P.OTA_DATA)
        while True:
            r = win.next(n)
            if r is None:
                return
            await self.link.write(P.OTA_DATA, data[r[0]:r[1]], False)

    async def _end(self) -> None:
        self._phase = "finishing"
        self._report()
        await self._command(P.END)

    async def _next(self) -> dict:
        """The next status for the running job; raises _Stalled after stall_s without any."""
        loop = asyncio.get_running_loop()
        while True:
            left = self._last_progress + self.stall_s - loop.time()
            if left <= 0:
                raise _Stalled()
            try:
                item = await self._inbox.get(left)
            except asyncio.TimeoutError:
                raise _Stalled() from None
            if item is _LOST:
                raise _Lost()
            if item is _CANCEL:
                raise _Cancelled()
            if item["kind"] in ("selftest", "heard"):
                continue                            # never part of a transfer
            self._last_progress = loop.time()
            return item

    async def _expect(self, kind: str) -> dict:
        while True:
            st = await self._next()
            if st["kind"] == kind:
                return st
            if st["kind"] == "error":
                raise PetError(f"Pet refused the update: {st['why']}")

    async def _rebooted(self, before: P.PetFirmware | None) -> Outcome:
        """After "done": the pet restarts ~1 s later. Wait for the link to drop, find the pet
        again and check it came back on the other slot (else the image was rolled back)."""
        loop = asyncio.get_running_loop()
        self._phase = "rebooting"
        self._report()
        deadline = loop.time() + self.reboot_s
        while self.link.connected:
            try:
                item = await self._inbox.get(max(0.0, deadline - loop.time()))
            except asyncio.TimeoutError:
                raise PetError("The pet did not come back after the update") from None
            if item is _LOST:
                break
            if item is _CANCEL:
                raise _Cancelled()
        self._inbox.clear()
        fw = None
        while fw is None:
            left = deadline - loop.time()
            if left <= 0:
                raise PetError("The pet did not come back after the update")
            try:
                await self.link.reconnect(min(left, 15.0))
                fw = await self.info()
            except asyncio.CancelledError:
                raise
            except Exception as e:
                log.info("pet not back yet: %s", e)
                await asyncio.sleep(min(1.0, max(0.0, deadline - loop.time())))
        if before is not None and before.slot == fw.slot:
            raise PetError(f"Pet came back on {fw.slot} again: the new image was rolled back")
        return self._outcome("updated", f"Updated in {int(self._elapsed())} s: now {fw.version} on {fw.slot}",
                             firmware=fw)

    def _streamed(self, st: dict, saying: str) -> Outcome:
        first = f"{self._first_ack - self._started:.2f}" if self._first_ack is not None else "?"
        who = f"Pet says “{saying}”" if saying else "Pet played the clip"
        msg = f"{who} · streamed {st['bytes'] // 1024} KB, sound after {first} s{'' if st['crc'] else ' (CRC mismatch)'}"
        return self._outcome("streamed", msg, crc_ok=st["crc"],
                             first_sound=None if self._first_ack is None else self._first_ack - self._started)

    async def _refresh(self) -> None:
        try:
            await self.info()
        except Exception as e:
            log.debug("refresh failed: %s", e)

    async def _abort(self) -> None:
        if self.link is not None and self.link.connected:
            try:
                await asyncio.wait_for(self.link.write(P.OTA_CONTROL, P.ABORT, True), 3)
            except Exception:
                pass

    def _elapsed(self) -> float:
        return asyncio.get_running_loop().time() - self._started

    def _overall(self) -> float:
        if self._batch_total == 0:
            return 0.0
        return min(1.0, (self._batch_done + self._acked) / self._batch_total)

    def _outcome(self, kind: str, message: str, **details: Any) -> Outcome:
        log.info("OTA: %s", message)
        return Outcome(kind, message, self._job_total, self._elapsed(), details)

    def _report(self) -> None:
        if self.on_progress is None:
            return
        try:
            self.on_progress(Progress(self._phase, self._overall(), self._part, self._acked, self._job_total,
                                      self._file_index, self._file_count, self._path))
        except Exception:
            log.exception("progress callback failed")
