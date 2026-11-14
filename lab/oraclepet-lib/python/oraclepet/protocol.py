"""OraclePet protocol v1: the bytes on the wire, nothing about Bluetooth.

A line-for-line port of the Swift PetProtocol module that ships in the OraclePet Mac app
(lab/desk-pet-mac/Sources/PetProtocol: OTA.swift, PetSound.swift, Heartbeat.swift). The spec
is ../../PROTOCOL.md; ../../golden/vectors.json pins every layout, and tests/test_vectors.py
checks this module against it byte for byte. Pure Python, no dependencies.
"""
from __future__ import annotations

import json
import math
import re
import struct
import sys
import time
import zlib
from array import array
from bisect import bisect_right
from collections.abc import Iterable, Sequence
from dataclasses import dataclass, field
from datetime import datetime
from typing import Any

from . import _gcb

PROTOCOL_VERSION = 1

# ---- GATT (PROTOCOL.md section 2): everything shares the NUS base UUID ----
NUS_SERVICE = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"        # host -> pet: heartbeat lines
NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"        # pet -> host: {"evt":...} lines
OTA_SERVICE = "6e400010-b5a3-f393-e0a9-e50e24dcca9e"
OTA_CONTROL = "6e400011-b5a3-f393-e0a9-e50e24dcca9e"   # read: what the pet runs; write: commands; notify: status
OTA_DATA = "6e400012-b5a3-f393-e0a9-e50e24dcca9e"      # write without response: transfer bytes
OTA_MIC = "6e400013-b5a3-f393-e0a9-e50e24dcca9e"       # notify: the mic's audio after LISTEN

NAME_PREFIX = "Claude-"          # the pet advertises "Claude-<last two MAC bytes>"

# Pet firmware projects, one per board (lab/jc3248-pet-idf/main/board.h).
PET_PROJECTS = {
    "jc3248_pet_idf": "JC3248 3.5in",
    "oraclepet_lcd28": "2.8in ILI9341",
    "oraclepet_sc01": "WT32-SC01 Plus",
}


def board_name(project: str) -> str:
    return PET_PROJECTS.get(project, project)


# Boards with a microphone (PROTOCOL.md 4.6). Every firmware has the mic characteristic; only
# these boards have a mic behind it (the others answer LISTEN with "no mic").
MIC_PROJECTS = frozenset({"oraclepet_lcd28"})


def has_mic(project: str) -> bool:
    return project in MIC_PROJECTS


# ---- CRC-32 (zlib: poly 0xEDB88320, init and final XOR 0xFFFFFFFF), as ota.c computes it ----
def crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


# ---- commands on the control characteristic (PROTOCOL.md 4.2), little-endian ----
END = b"\x02"
ABORT = b"\x03"
SELFTEST = b"\x0a"
STREAM_PATH = "@stream"          # path marker for a streamed job in the transfer queue (as OTA.streamPath)


def begin(size: int, crc: int) -> bytes:
    """0x01 BEGIN: a full app image follows on the data characteristic."""
    return struct.pack("<BII", 0x01, size, crc)


def begin_file(size: int, crc: int, path: str) -> bytes:
    """0x04 BEGIN_FILE: one file into the pet's LittleFS ("characters/<pack>/<file>", ...)."""
    p = path.encode("utf-8")
    if len(p) > 255:
        raise ValueError("path longer than 255 bytes")
    return struct.pack("<BIIB", 0x04, size, crc, len(p)) + p


def begin_stream(size: int, crc: int) -> bytes:
    """0x0B BEGIN_STREAM: a PSND clip the pet plays while it arrives."""
    return struct.pack("<BII", 0x0B, size, crc)


def _named(op: int, name: str) -> bytes:
    n = name.encode("utf-8")
    if len(n) > 255:
        raise ValueError("name longer than 255 bytes")
    return bytes((op, len(n))) + n


def select_pack(name: str) -> bytes:
    """0x05 SELECT_PACK: show that animation pack."""
    return _named(0x05, name)


def run_app(name: str) -> bytes:
    """0x06 RUN_APP: run apps/<name>.wasm; an empty name stops the app."""
    return _named(0x06, name)


def play(name: str) -> bytes:
    """0x08 PLAY: play sounds/<name>.ima."""
    return _named(0x08, name)


def progress(pct: int) -> bytes:
    """0x07 PROGRESS: the host's overall %, which the pet draws."""
    return bytes((0x07, max(0, min(100, int(pct)))))


def listen(ms: int, send_audio: bool) -> bytes:
    """0x0C LISTEN: record the mic for ms (clamped to 100..10000); bit 0 = send the audio back."""
    m = max(100, min(10_000, int(ms)))
    return bytes((0x0C, m & 0xFF, m >> 8, 1 if send_audio else 0))


# Paths the pet accepts for BEGIN_FILE: ota.c safe_path(), byte for byte, so a host can
# refuse early instead of hearing "bad path" halfway through a pack.
_PATH_CHARS = re.compile(rb"[a-z0-9_.\-]*")
_NAME = re.compile(r"[a-z0-9_-]{1,23}")


def pet_accepts_path(path: str) -> bool:
    p = path.encode("utf-8")
    if b".." in p or len(p) >= 96:                     # s_path[96] in ota.c
        return False
    if p.startswith(b"apps/") or p.startswith(b"sounds/"):
        ext, pre = (b".wasm", 5) if p.startswith(b"apps/") else (b".ima", 7)
        if len(p) < pre + 1 + len(ext) or len(p) - pre - len(ext) > 23 or not p.endswith(ext):
            return False
        rest = p[pre:]
        return b"/" not in rest and _PATH_CHARS.fullmatch(rest) is not None
    if not p.startswith(b"characters/"):
        return False
    return p.count(b"/") == 2 and _PATH_CHARS.fullmatch(p.replace(b"/", b"")) is not None


def valid_name(name: str) -> bool:
    """Pack and app names the Mac app accepts: ^[a-z0-9_-]{1,23}$."""
    return _NAME.fullmatch(name) is not None


# ---- status lines the pet notifies on control (PROTOCOL.md 4.3; OTAStatus.parse) ----
def _json_object(data: bytes | str) -> dict | None:
    try:
        o = json.loads(data)
    except (ValueError, UnicodeDecodeError):
        return None
    return o if isinstance(o, dict) else None


def _int(v: Any, default: Any) -> Any:
    # JSONSerialization + `as? Int`: integers (and integral floats) only
    if isinstance(v, bool):
        return default
    if isinstance(v, int):
        return v
    if isinstance(v, float) and v.is_integer():
        return int(v)
    return default


def _float(v: Any, default: float = 0.0) -> float:
    if isinstance(v, bool) or not isinstance(v, (int, float)):
        return default
    return float(v)


def _str(v: Any, default: str = "") -> str:
    return v if isinstance(v, str) else default


def _bool(v: Any) -> bool:
    return v is True or (isinstance(v, int) and not isinstance(v, bool) and v == 1)


def parse_status(data: bytes | str) -> dict | None:
    """One control notification -> {"kind": ..., fields}, or None when it is not a status line.

    Defaults follow OTAStatus.parse: a missing field reads as 0 / "" / False.
    """
    o = _json_object(data)
    if o is None:
        return None
    kind = o.get("ota")
    if not isinstance(kind, str):
        return None
    if kind == "ready":
        return {"kind": kind, "size": _int(o.get("size"), 0), "window": _int(o.get("window"), 4096)}
    if kind == "ack":
        off = _int(o.get("off"), None)
        return None if off is None else {"kind": kind, "off": off}
    if kind == "done":
        return {"kind": kind}
    if kind == "saved":
        return {"kind": kind, "path": _str(o.get("path"))}
    if kind in ("pack", "app"):
        return {"kind": kind, "name": _str(o.get("name"))}
    if kind == "play":
        return {"kind": kind, "name": _str(o.get("name")), "bytes": _int(o.get("bytes"), 0)}
    if kind == "streamed":
        return {"kind": kind, "bytes": _int(o.get("bytes"), 0), "crc": _bool(o.get("crc"))}
    if kind == "selftest":
        return {"kind": kind, "mic": _bool(o.get("mic")), "heard": _bool(o.get("heard")),
                "db": _float(o.get("db")), "quiet": _int(o.get("quiet"), 0), "tone": _int(o.get("tone"), 0)}
    if kind == "heard":
        out = {"kind": kind}
        for k in ("ms", "samples", "rms", "peak", "freq"):
            out[k] = _int(o.get(k), 0)
        out["db"] = _float(o.get("db"))
        out["floor"] = _int(o.get("floor"), 0)
        if "sent" in o:                                # newer firmware says whether audio followed
            out["sent"] = _bool(o.get("sent"))
        return out
    if kind == "error":
        return {"kind": kind, "why": _str(o.get("why"), "unknown error")}
    return None


# ---- what the pet runs (control read, PROTOCOL.md 4.1; PetFirmware) ----
@dataclass
class PetFirmware:
    version: str
    slot: str = ""
    built: str = ""
    sha: str = ""            # prefix of the ELF SHA-256 (the pet reports 9 hex digits)
    project: str = ""        # which board: "jc3248_pet_idf" / "oraclepet_lcd28"
    state: str = ""          # "pending" until a new image proves itself, then "valid"
    pack: str = ""
    packs: list = field(default_factory=list)
    free_kb: int = 0
    app: str = ""
    apps: list = field(default_factory=list)
    app_error: str = ""
    raw: dict = field(default_factory=dict, repr=False, compare=False)

    @property
    def board(self) -> str:
        return board_name(self.project)

    def same_build(self, other: PetFirmware) -> bool:
        """By hash when both sides have >= 8 digits (on the common prefix), else version + build time."""
        n = min(len(self.sha), len(other.sha))
        if n >= 8:
            return self.sha[:n] == other.sha[:n]
        return self.version == other.version and self.built == other.built

    @classmethod
    def parse(cls, data: bytes | str) -> PetFirmware | None:
        o = _json_object(data)
        if o is None or not isinstance(o.get("ver"), str):
            return None
        return cls(version=o["ver"], slot=_str(o.get("slot"), "?"), built=_str(o.get("built")),
                   sha=_str(o.get("sha")), project=_str(o.get("project")), state=_str(o.get("state")),
                   pack=_str(o.get("pack")), packs=_names(o.get("packs")), free_kb=_int(o.get("free"), 0),
                   app=_str(o.get("app")), apps=_names(o.get("apps")), app_error=_str(o.get("apperr")), raw=o)


def _names(v: Any) -> list:
    return sorted(x for x in v if isinstance(x, str)) if isinstance(v, list) else []


# ---- firmware images (PROTOCOL.md 4.5; OTA.projectName / OTA.describe) ----
APP_DESC_OFFSET = 24 + 8         # esp_image_header_t + first segment header
APP_DESC_MAGIC = 0xABCD5432


def _cstr(b: bytes) -> str:
    return b.split(b"\0", 1)[0].decode("utf-8", "replace")


def project_name(image: bytes) -> str | None:
    """The project an ESP app image was built from, or None when it is not an app image."""
    d = APP_DESC_OFFSET
    if len(image) <= d + 80 or image[0] != 0xE9:
        return None
    if struct.unpack_from("<I", image, d)[0] != APP_DESC_MAGIC:
        return None
    return _cstr(image[d + 48:d + 80])


def describe(image: bytes) -> PetFirmware | None:
    """Version, build time and SHA prefix of an image, shaped like what the pet reports."""
    d = APP_DESC_OFFSET
    project = project_name(image)
    if project is None or len(image) < d + 152:
        return None
    built = f"{_cstr(image[d + 96:d + 112])} {_cstr(image[d + 80:d + 96])}"   # date + time
    return PetFirmware(version=_cstr(image[d + 16:d + 48]), slot="", built=built,
                       sha=image[d + 144:d + 152].hex(), project=project)


# ---- flow control for write-without-response (OTAWindow) ----
class OtaWindow:
    """Never more than `window` bytes beyond the last ack: the protocol's only flow control."""

    def __init__(self, total: int, window: int):
        self.total = total
        self.window = window
        self.sent = 0
        self.acked = 0

    def next(self, max_len: int) -> tuple[int, int] | None:
        """The next (start, end) to send, at most max_len long; None when full or all sent."""
        if self.sent >= self.total or self.sent - self.acked >= self.window or max_len <= 0:
            return None
        n = min(max_len, self.total - self.sent, self.window - (self.sent - self.acked))
        r = (self.sent, self.sent + n)
        self.sent += n
        return r

    def ack(self, off: int) -> None:
        self.acked = max(self.acked, min(off, self.sent))

    @property
    def all_acked(self) -> bool:
        return self.acked == self.total

    @property
    def fraction(self) -> float:
        return 0.0 if self.total == 0 else self.acked / self.total


# ---- PSND clips: IMA ADPCM, 16 kHz mono (PROTOCOL.md 5; PetSound) ----
SAMPLE_RATE = 16_000
BLOCK_BYTES = 256
BLOCK_SAMPLES = 504

IMA_STEP = (
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
)
IMA_INDEX = (-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8)


def _clamp16(v: int) -> int:
    return -32768 if v < -32768 else 32767 if v > 32767 else v


def _ima_step(nib: int, pred: int, index: int) -> tuple[int, int]:
    """The decoder step. The encoder tracks its prediction with it too, so both are bit-exact."""
    s = IMA_STEP[index]
    diff = s >> 3
    if nib & 1:
        diff += s >> 2
    if nib & 2:
        diff += s >> 1
    if nib & 4:
        diff += s
    pred = _clamp16(pred - diff if nib & 8 else pred + diff)
    index = min(88, max(0, index + IMA_INDEX[nib]))
    return pred, index


def psnd_encode(pcm: Sequence[int], sample_rate: int = SAMPLE_RATE) -> bytes:
    """16 kHz mono int16 samples -> a PSND clip (16-byte header + 256-byte blocks)."""
    out = bytearray(b"PSND\x01\x01\x00\x00")
    out += struct.pack("<II", sample_rate, len(pcm))
    pred = index = 0                                  # carried across blocks
    for start in range(0, len(pcm), BLOCK_SAMPLES):
        block = bytearray(BLOCK_BYTES)
        struct.pack_into("<hB", block, 0, pred, index)    # the state before this block's first sample
        for k, s in enumerate(pcm[start:start + BLOCK_SAMPLES]):
            diff = _clamp16(int(s)) - pred
            nib = 0
            if diff < 0:
                nib = 8
                diff = -diff
            t = IMA_STEP[index]
            if diff >= t:
                nib |= 4
                diff -= t
            t >>= 1
            if diff >= t:
                nib |= 2
                diff -= t
            t >>= 1
            if diff >= t:
                nib |= 1
            pred, index = _ima_step(nib, pred, index)
            block[4 + k // 2] |= nib << ((k & 1) * 4)     # low nibble first
        out += block
    return bytes(out)


def psnd_header(clip: bytes) -> tuple[int, int] | None:
    """(sample rate, sample count) of a PSND v1 IMA clip, or None."""
    if len(clip) < 16 or clip[:4] != b"PSND" or clip[4] != 1 or clip[5] != 1:
        return None
    return struct.unpack_from("<II", clip, 8)


def psnd_size(samples: int) -> int:
    """Bytes of a whole clip holding `samples` (the last block is padded to 256)."""
    return 16 + -(-samples // BLOCK_SAMPLES) * BLOCK_BYTES


def psnd_decode(clip: bytes) -> list[int] | None:
    """A PSND clip -> samples (what the pet plays), or None when it is not one.

    Every block restarts from its own header; a short clip decodes as far as it goes.
    """
    b = bytes(clip)
    head = psnd_header(b)
    if head is None:
        return None
    total = head[1]
    out: list[int] = []
    off = 16
    while len(out) < total and off + 4 <= len(b):
        pred = struct.unpack_from("<h", b, off)[0]
        index = min(88, b[off + 2])
        for k in range(min(BLOCK_SAMPLES, total - len(out))):
            byte = off + 4 + k // 2
            if byte >= len(b):
                return out
            pred, index = _ima_step((b[byte] >> ((k & 1) * 4)) & 0x0F, pred, index)
            out.append(pred)
        off += BLOCK_BYTES
    return out


def _round_half_away(x: float) -> int:
    # Swift's .rounded() (Python's round() is banker's rounding); exact for doubles
    a = abs(x)
    f = math.floor(a)
    r = int(f) + (1 if a - f >= 0.5 else 0)
    return r if x >= 0 else -r


def tidy(pcm: Sequence[int], peak: float = 0.9, max_seconds: float = 6.0) -> list[int]:
    """Speech prep (PROTOCOL.md 5): trim quiet ends (|s| <= 300) keeping 20 ms around the
    speech, cap the length, normalise the peak to `peak` of full scale. [] when all quiet."""
    quiet = 300
    first = next((i for i, s in enumerate(pcm) if abs(s) > quiet), None)
    if first is None:
        return []
    last = next(i for i in range(len(pcm) - 1, -1, -1) if abs(pcm[i]) > quiet)
    pad = SAMPLE_RATE // 50
    s = list(pcm[max(0, first - pad):min(len(pcm) - 1, last + pad) + 1])
    cap = int(max_seconds * SAMPLE_RATE)
    if len(s) > cap:
        s = s[:cap]
    m = max(abs(x) for x in s)
    g = 32767 * peak / max(m, 1)
    return [_clamp16(_round_half_away(x * g)) for x in s]


def is_thai(text: str) -> bool:
    """Whether text should get a Thai voice (any U+0E00..U+0E7F)."""
    return any("฀" <= ch <= "๿" for ch in text)


def wav(pcm: Sequence[int], sample_rate: int = SAMPLE_RATE) -> bytes:
    """16-bit mono WAV, for saving what the pet's mic heard (PetSound.wav)."""
    a = array("h", (_clamp16(int(x)) for x in pcm))
    if sys.byteorder == "big":
        a.byteswap()
    body = a.tobytes()
    head = struct.pack("<4sI4s4sIHHIIHH4sI", b"RIFF", 36 + len(body), b"WAVE", b"fmt ", 16, 1, 1,
                       sample_rate, sample_rate * 2, 2, 16, b"data", len(body))
    return head + body


# ---- grapheme clusters: Swift counts Characters when it clips msg and who ----
# The same UAX #29 breaker as core/oraclepet.c (cluster_len), on the same table read off
# Swift's own String breaking (_gcb.py, tools/gen_gcb.py), so Indic conjuncts, prepend marks
# and emoji ZWJ sequences cut exactly where the Mac app cuts them.
(_OTHER, _CTRL, _EXT, _EXT_ONLY, _LINK, _SPACING, _PRE, _PICT, _CONS,
 _CR, _LF, _ZWJ, _RI, _L, _V, _T, _LV, _LVT) = range(18)


def _gcb_class(c: int) -> int:
    if c == 0x0D:
        return _CR
    if c == 0x0A:
        return _LF
    if c < 0x20 or c == 0x7F:
        return _CTRL
    if c < 0x80:
        return _OTHER
    if c == 0x200D:
        return _ZWJ
    if 0x1F1E6 <= c <= 0x1F1FF:
        return _RI
    if 0x1100 <= c <= 0x115F or 0xA960 <= c <= 0xA97C:
        return _L
    if 0x1160 <= c <= 0x11A7 or 0xD7B0 <= c <= 0xD7C6:
        return _V
    if 0x11A8 <= c <= 0x11FF or 0xD7CB <= c <= 0xD7FB:
        return _T
    if 0xAC00 <= c <= 0xD7A3:
        return _LVT if (c - 0xAC00) % 28 else _LV
    i = bisect_right(_gcb.STARTS, c) - 1                # the last run starting <= c
    return _gcb.CLASSES[i] if i >= 0 else _OTHER


def _extend(g: int) -> bool:
    return g in (_EXT, _EXT_ONLY, _LINK)


def graphemes(s: str) -> list[str]:
    """Split into user-perceived characters exactly like Swift's String (UAX #29 extended
    grapheme clusters, as the Swift runtime the table was read from implements them)."""
    out: list[str] = []
    prev = pict = pict_zwj = cons = link = ri = 0
    for ch in s:
        cur = _gcb_class(ord(ch))
        if not out:
            join = False
        elif prev == _CR:
            join = cur == _LF                                                   # GB3-5
        elif prev in (_CTRL, _LF) or cur in (_CTRL, _CR, _LF):
            join = False
        elif prev == _L:
            join = cur in (_L, _V, _LV, _LVT, _ZWJ, _SPACING) or _extend(cur)  # GB6
        elif prev in (_LV, _V) and cur in (_V, _T):
            join = True                                                         # GB7
        elif prev in (_LVT, _T) and cur == _T:
            join = True                                                         # GB8
        elif _extend(cur) or cur in (_ZWJ, _SPACING):
            join = True                                                         # GB9, 9a
        elif prev == _PRE:
            join = True                                                         # GB9b
        elif cur == _CONS and cons and link:
            join = True                                                         # GB9c
        elif cur == _PICT and pict_zwj:
            join = True                                                         # GB11
        else:
            join = cur == _RI and prev == _RI and ri == 1                       # GB12/13
        if join:
            out[-1] += ch
            pict_zwj = cur == _ZWJ and pict
            pict = cur == _PICT or (pict and _extend(cur))
            if cur == _CONS:
                cons, link = 1, 0
            elif cur == _LINK:
                link = cons
            elif cur not in (_EXT, _ZWJ):
                cons = link = 0
            ri += cur == _RI
        else:                                                    # a new cluster starts fresh
            out.append(ch)
            pict, pict_zwj = cur == _PICT, False
            cons, link = cur == _CONS, 0
            ri = cur == _RI
        prev = cur
    return out


def clip_utf8(s: str, limit: int) -> str:
    """At most `limit` UTF-8 bytes without splitting a character (Heartbeat.clip)."""
    out, n = [], 0
    for g in graphemes(s):
        b = len(g.encode("utf-8", "surrogatepass"))
        if n + b > limit:
            break
        out.append(g)
        n += b
    return "".join(out)


# ---- heartbeat line (PROTOCOL.md 3.1; Heartbeat.jsonLine) ----
MSG_LIMIT = 63          # the firmware copies msg into char[64]
WHO_LIMIT = 20          # drawn at 16 px a glyph across 320 px

STATE_COUNTS = {        # the smallest counts that make the pet show each state (PetState.counts)
    "idle": (1, 0, 0),
    "busy": (1, 1, 0),
    "attention": (1, 0, 1),
    "sleep": (0, 0, 0),
}


def pet_state(total: int, running: int, waiting: int) -> str:
    """waiting > 0 -> attention, running > 0 -> busy, total == 0 -> sleep, else idle."""
    if waiting > 0:
        return "attention"
    if running > 0:
        return "busy"
    return "sleep" if total == 0 else "idle"


def local_tzoff(at: float | None = None) -> int:
    """Local UTC offset in seconds (GMT+7 -> 25200), as the pet's clock expects."""
    d = datetime.fromtimestamp(time.time() if at is None else at).astimezone()
    return int(d.utcoffset().total_seconds())


def _quote(s: str) -> str:
    # Heartbeat.quote: escape " \ \n \t and other controls; everything else raw UTF-8
    out = ['"']
    for ch in s:
        if ch == '"':
            out.append('\\"')
        elif ch == "\\":
            out.append("\\\\")
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\t":
            out.append("\\t")
        elif ord(ch) < 0x20:
            out.append(f"\\u{ord(ch):04x}")
        elif 0xD800 <= ord(ch) <= 0xDFFF:           # a lone surrogate cannot be UTF-8
            out.append("�")
        else:
            out.append(ch)
    out.append('"')
    return "".join(out)


@dataclass
class Heartbeat:
    """One NUS RX line. epoch/tzoff default to now and the local offset."""
    total: int
    running: int
    waiting: int
    msg: str = ""
    epoch: int | None = None
    tzoff: int | None = None
    pct5h: int | None = None
    reset5h: int | None = None
    pct7d: int | None = None
    reset7d: int | None = None
    who: str | None = None

    def __post_init__(self) -> None:
        self.msg = clip_utf8(self.msg, MSG_LIMIT)
        if self.who is not None:
            self.who = "".join(graphemes(self.who)[:WHO_LIMIT])
        if self.epoch is None:
            self.epoch = int(time.time())
        if self.tzoff is None:
            self.tzoff = local_tzoff(self.epoch)

    @classmethod
    def for_state(cls, state: str, **kw: Any) -> Heartbeat:
        total, running, waiting = STATE_COUNTS[state]
        return cls(total, running, waiting, **kw)

    @property
    def state(self) -> str:
        return pet_state(self.total, self.running, self.waiting)

    def json_line(self) -> bytes:
        """The exact bytes the pet receives: fixed key order, optional keys only when set, "\\n"."""
        parts = [f'"total":{int(self.total)}', f'"running":{int(self.running)}', f'"waiting":{int(self.waiting)}',
                 f'"msg":{_quote(clip_utf8(self.msg, MSG_LIMIT))}', f'"epoch":{int(self.epoch)}',
                 f'"tzoff":{int(self.tzoff)}']
        for key in ("pct5h", "reset5h", "pct7d", "reset7d"):
            v = getattr(self, key)
            if v is not None:
                parts.append(f'"{key}":{int(v)}')
        if self.who is not None:
            parts.append(f'"who":{_quote(self.who)}')
        return ("{" + ",".join(parts) + "}\n").encode("utf-8")


def event_lines(buffer: bytearray, data: bytes) -> Iterable[dict]:
    """Feed NUS TX bytes; yield each complete {"evt":...} object (lines may span notifications)."""
    buffer += data
    while True:
        nl = buffer.find(b"\n")
        if nl < 0:
            break
        line = bytes(buffer[:nl])
        del buffer[:nl + 1]
        o = _json_object(line)
        if o is not None and isinstance(o.get("evt"), str):
            yield o
    if len(buffer) > 4096:                            # a peer that never sends "\n"
        buffer.clear()
