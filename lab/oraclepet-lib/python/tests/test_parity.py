"""Edge cases against the Swift PetProtocol itself, beyond golden/vectors.json.

tests/parity_vectors.json comes from the OraclePetParity target (lab/desk-pet-mac), built on the Swift sources
the Mac app ships (tests/swift/gen_parity.sh). It pins what the golden file leaves open:
PSND clips of 0, 1, 503, 504, 505, 1008, 1009 samples (the block edge, the clamps, the encoder
state carried into the next block's header), damaged clips, tidy() rounding and the length cap,
heartbeat escaping and clipping with Thai, emoji, controls and empty strings, name and path
lengths, LISTEN/PROGRESS clamping and status-line defaults. Offline: no Bluetooth.
"""
from __future__ import annotations

import json
import struct
import unittest
from pathlib import Path

from oraclepet import protocol as P

V = json.loads((Path(__file__).resolve().parent / "parity_vectors.json").read_text(encoding="utf-8"))

COMMANDS = {
    "begin": lambda a: P.begin(a["size"], a["crc"]),
    "begin_file": lambda a: P.begin_file(a["size"], a["crc"], a["path"]),
    "begin_stream": lambda a: P.begin_stream(a["size"], a["crc"]),
    "select_pack": lambda a: P.select_pack(a["name"]),
    "run_app": lambda a: P.run_app(a["name"]),
    "play": lambda a: P.play(a["name"]),
    "progress": lambda a: P.progress(a["pct"]),
    "listen": lambda a: P.listen(a["ms"], a["send_audio"]),
}


def swift_description(st: dict | None) -> str:
    """parse_status() written the way Swift prints the matching OTAStatus case."""
    if st is None:
        return "nil"
    k = st["kind"]

    def b(x: bool) -> str:
        return "true" if x else "false"

    if k == "ready":
        return f"ready(size: {st['size']}, window: {st['window']})"
    if k == "ack":
        return f"ack({st['off']})"
    if k == "streamed":
        return f"streamed(bytes: {st['bytes']}, crcOK: {b(st['crc'])})"
    if k == "heard":
        return (f"heard(PetProtocol.MicLevels(ms: {st['ms']}, samples: {st['samples']}, rms: {st['rms']}, "
                f"peak: {st['peak']}, freq: {st['freq']}, db: {st['db']!r}, floor: {st['floor']}, "
                f"sent: {b(st.get('sent', False))}))")
    if k == "selftest":
        return (f"selftest(mic: {b(st['mic'])}, heard: {b(st['heard'])}, db: {st['db']!r}, "
                f"quiet: {st['quiet']}, tone: {st['tone']})")
    if k in ("saved",):
        return f'saved("{st["path"]}")'
    if k in ("play", "pack", "app"):
        return f'{k}("{st["name"]}")'
    if k == "error":
        return f'error("{st["why"]}")'
    return k


class SwiftEdgeCases(unittest.TestCase):
    def test_psnd_encode_around_the_block_edge(self):
        for c in V["psnd"]:
            with self.subTest(clip=c["name"]):
                clip = P.psnd_encode(c["pcm"])
                self.assertEqual(clip.hex(), c["clip_hex"])
                n = len(c["pcm"])
                self.assertEqual(len(clip), P.psnd_size(n))
                self.assertEqual(len(clip), 16 + 256 * -(-n // 504))

    def test_psnd_decode_around_the_block_edge(self):
        for c in V["psnd"]:
            with self.subTest(clip=c["name"]):
                self.assertEqual(P.psnd_decode(bytes.fromhex(c["clip_hex"])), c["decoded"])

    def test_block_headers_carry_the_encoder_state(self):
        # each block header = the decoder's state after the previous block's last sample
        for c in V["psnd"]:
            clip = bytes.fromhex(c["clip_hex"])
            dec = c["decoded"]
            for blk in range(1, (len(clip) - 16) // 256):
                with self.subTest(clip=c["name"], block=blk):
                    pred, index = struct.unpack_from("<hB", clip, 16 + 256 * blk)
                    self.assertEqual(pred, dec[504 * blk - 1])
                    self.assertEqual(clip[16 + 256 * blk + 3], 0)
                    self.assertLessEqual(index, 88)

    def test_damaged_clips_decode_like_swift(self):
        for i, c in enumerate(V["psnd_damaged"]):
            with self.subTest(case=i, length=len(c["clip_hex"]) // 2):
                self.assertEqual(P.psnd_decode(bytes.fromhex(c["clip_hex"])), c["decoded"])

    def test_tidy(self):
        for i, c in enumerate(V["tidy"]):
            with self.subTest(case=i, n=len(c["pcm"])):
                kw = {}
                if "peak" in c:
                    kw["peak"] = c["peak"]
                if "max_seconds" in c:
                    kw["max_seconds"] = c["max_seconds"]
                self.assertEqual(P.tidy(c["pcm"], **kw), c["tidy"])

    def test_heartbeat_escaping_and_clipping(self):
        for c in V["heartbeat"]:
            with self.subTest(text=c["text"]):
                if c.get("all_optional"):
                    hb = P.Heartbeat(0, 0, 0, "", epoch=0, tzoff=0, pct5h=0, reset5h=1, pct7d=100,
                                     reset7d=604_800, who="")
                else:
                    hb = P.Heartbeat(1, 0, 0, c["text"], epoch=1792547421, tzoff=-18000, who=c["text"])
                self.assertEqual(hb.msg, c["msg"])
                self.assertEqual(hb.who, c["who"])
                line = hb.json_line()
                self.assertEqual(line, c["line"].encode("utf-8"))
                self.assertLessEqual(len(hb.msg.encode("utf-8")), P.MSG_LIMIT)
                self.assertEqual(json.loads(line)["msg"], hb.msg)     # still valid JSON

    def test_commands(self):
        for c in V["commands"]:
            with self.subTest(name=c["name"], args=c["args"]):
                self.assertEqual(COMMANDS[c["name"]](c["args"]).hex(), c["hex"])

    def test_crc32(self):
        for c in V["crc32"]:
            with self.subTest(n=len(c["hex"]) // 2):
                self.assertEqual(P.crc32(bytes.fromhex(c["hex"])), c["crc32"])

    def test_status_defaults(self):
        for c in V["status"]:
            with self.subTest(line=c["line"]):
                self.assertEqual(swift_description(P.parse_status(c["line"].encode())), c["swift"])


class FirmwareLimits(unittest.TestCase):
    """What ota.c's ctrl_access() and safe_path() accept, so the host can refuse first."""

    def test_names(self):
        self.assertTrue(P.valid_name("a"))
        self.assertTrue(P.valid_name("z" * 23))                 # s_pack_req[24] / s_app_req[24] / nm[24]
        self.assertFalse(P.valid_name("z" * 24))
        self.assertFalse(P.valid_name(""))
        self.assertFalse(P.valid_name("แมว"))                    # 9 UTF-8 bytes, and not a-z
        self.assertFalse(P.valid_name("Neo"))

    def test_paths(self):
        ok = ["characters/p/" + "f" * 82, "apps/" + "a" * 23 + ".wasm", "sounds/x.ima", "characters/neo/idle.gif"]
        bad = ["characters/p/" + "f" * 83,                       # 96 bytes: s_path[96] holds 95
               "apps/" + "a" * 24 + ".wasm", "apps/.wasm", "sounds/.ima", "sounds/x.wav",
               "characters/แมว/idle.gif", "characters/neo/../x", "characters/neo", "characters/a/b/c",
               "apps/x/y.wasm", "Characters/neo/a.gif", ""]
        for p in ok:
            with self.subTest(path=p):
                self.assertTrue(P.pet_accepts_path(p))
        for p in bad:
            with self.subTest(path=p):
                self.assertFalse(P.pet_accepts_path(p))

    def test_begin_file_refuses_what_the_length_byte_cannot_hold(self):
        with self.assertRaises(ValueError):
            P.begin_file(0, 0, "x" * 256)
        self.assertEqual(P.begin_file(0, 0, "x" * 255)[9], 255)
        with self.assertRaises(ValueError):
            P.select_pack("x" * 256)


if __name__ == "__main__":
    unittest.main()
