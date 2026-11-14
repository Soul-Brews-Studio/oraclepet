"""The wire format against the golden vectors, and against the Swift PetProtocol's own tests.

golden/vectors.json is generated from the Swift PetProtocol that ships in the OraclePet Mac
app (tools/gen_vectors.sh); every byte must come out the same here. SwiftParity ports the
protocol cases of lab/desk-pet-mac/Tests/PetProtocolTests. Offline: no Bluetooth.

    python -m unittest discover -s tests -v
"""
from __future__ import annotations

import json
import math
import os
import struct
import unittest
from pathlib import Path

from oraclepet import protocol as P

HERE = Path(__file__).resolve().parent
VECTORS = Path(os.environ.get("ORACLEPET_VECTORS", HERE.parents[1] / "golden" / "vectors.json"))
V = json.loads(VECTORS.read_text(encoding="utf-8"))

# vector command name -> how this library builds it
COMMANDS = {
    "begin": lambda a: P.begin(a["size"], a["crc"]),
    "begin_file": lambda a: P.begin_file(a["size"], a["crc"], a["path"]),
    "begin_stream": lambda a: P.begin_stream(a["size"], a["crc"]),
    "end": lambda a: P.END,
    "abort": lambda a: P.ABORT,
    "select_pack": lambda a: P.select_pack(a["name"]),
    "run_app": lambda a: P.run_app(a["name"]),
    "progress": lambda a: P.progress(a["pct"]),
    "play": lambda a: P.play(a["name"]),
    "selftest": lambda a: P.SELFTEST,
    "listen": lambda a: P.listen(a["ms"], a["send_audio"]),
}


def crc32_table(data: bytes) -> int:
    """ota.c's table CRC, written out, to show zlib.crc32 is the same function."""
    table = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = 0xEDB88320 ^ (c >> 1) if c & 1 else c >> 1
        table.append(c)
    crc = 0xFFFFFFFF
    for b in data:
        crc = table[(crc ^ b) & 0xFF] ^ (crc >> 8)
    return crc ^ 0xFFFFFFFF


class GoldenVectors(unittest.TestCase):
    def test_every_section_is_covered(self):
        # a new section in vectors.json needs a test here before it counts as reproduced
        self.assertEqual(set(V), {"commands", "crc32", "generated_by", "heartbeat", "ima_index", "ima_step",
                                  "image", "projects", "protocol", "psnd", "status", "uuids"})
        self.assertEqual(V["protocol"], P.PROTOCOL_VERSION)

    def test_ima_tables(self):
        self.assertEqual(list(P.IMA_STEP), V["ima_step"])
        self.assertEqual(list(P.IMA_INDEX), V["ima_index"])

    def test_crc32(self):
        for c in V["crc32"]:
            with self.subTest(text=c["text_utf8"]):
                data = c["text_utf8"].encode("utf-8")
                self.assertEqual(P.crc32(data), c["crc32"])
                self.assertEqual(crc32_table(data), c["crc32"])

    def test_commands(self):
        for c in V["commands"]:
            with self.subTest(name=c["name"], args=c["args"]):
                self.assertIn(c["name"], COMMANDS, "a command this library cannot build")
                self.assertEqual(COMMANDS[c["name"]](c["args"]).hex(), c["hex"])

    def test_heartbeat_line(self):
        hb = P.Heartbeat(**V["heartbeat"]["args"])
        self.assertEqual(hb.json_line(), V["heartbeat"]["line"].encode("utf-8"))

    def test_psnd_encode(self):
        for c in V["psnd"]:
            with self.subTest(clip=c["name"]):
                self.assertEqual(P.psnd_encode(c["pcm"]).hex(), c["clip_hex"])

    def test_psnd_decode(self):
        for c in V["psnd"]:
            with self.subTest(clip=c["name"]):
                clip = bytes.fromhex(c["clip_hex"])
                self.assertEqual(P.psnd_decode(clip), c["decoded"])
                self.assertEqual(P.psnd_header(clip), (16000, len(c["pcm"])))
                self.assertEqual(len(clip), P.psnd_size(len(c["pcm"])))

    def test_image(self):
        img = bytes.fromhex(V["image"]["hex"])
        self.assertEqual(P.project_name(img), V["image"]["project"])
        d = P.describe(img)
        self.assertEqual((d.project, d.version, d.built, d.sha),
                         (V["image"]["project"], V["image"]["version"], V["image"]["built"], V["image"]["sha16"]))

    def test_projects(self):
        self.assertEqual(sorted(P.PET_PROJECTS), V["projects"])

    def test_status_lines(self):
        want = {
            "ready": {"kind": "ready", "size": 100, "window": 16384},
            "ack": {"kind": "ack", "off": 4096},
            "streamed": {"kind": "streamed", "bytes": 14000, "crc": True},
            "heard": {"kind": "heard", "ms": 3000, "samples": 48000, "rms": 412, "peak": 9120, "freq": 1000,
                      "db": 23.4, "floor": 31},
            "error": {"kind": "error", "why": "no mic"},
        }
        for s in V["status"]:
            with self.subTest(kind=s["kind"]):
                st = P.parse_status(s["line"].encode("utf-8"))
                self.assertEqual(st["kind"], s["kind"])
                self.assertEqual(st, want[s["kind"]])

    def test_uuids(self):
        self.assertEqual(V["uuids"], {"nus_service": P.NUS_SERVICE, "nus_rx": P.NUS_RX, "nus_tx": P.NUS_TX,
                                      "ota_service": P.OTA_SERVICE, "ota_control": P.OTA_CONTROL,
                                      "ota_data": P.OTA_DATA, "ota_mic": P.OTA_MIC})


class SwiftParity(unittest.TestCase):
    """The protocol cases of lab/desk-pet-mac/Tests/PetProtocolTests, in Python."""

    def test_pet_state_rule(self):
        self.assertEqual(P.pet_state(3, 2, 1), "attention")          # waiting wins
        self.assertEqual(P.pet_state(3, 2, 0), "busy")
        self.assertEqual(P.pet_state(0, 0, 0), "sleep")
        self.assertEqual(P.pet_state(1, 0, 0), "idle")
        for state, counts in P.STATE_COUNTS.items():
            self.assertEqual(P.pet_state(*counts), state)
            self.assertEqual(P.Heartbeat.for_state(state, epoch=0, tzoff=0).state, state)

    def test_heartbeat_exact_line(self):
        hb = P.Heartbeat(3, 1, 2, msg='wait: neo, "x"', epoch=1_792_547_421, tzoff=25_200, pct5h=8, reset5h=14_100)
        line = hb.json_line()
        self.assertEqual(line, b'{"total":3,"running":1,"waiting":2,"msg":"wait: neo, \\"x\\"","epoch":1792547421,'
                               b'"tzoff":25200,"pct5h":8,"reset5h":14100}\n')
        self.assertIsInstance(json.loads(line), dict)                 # real JSON, one line
        self.assertEqual(line.count(b"\n"), 1)

    def test_msg_fits_the_firmware_buffer(self):
        thai = "แมว" * 40                                             # 3 bytes per code point
        hb = P.Heartbeat(1, 0, 0, msg=thai, epoch=0, tzoff=0)
        self.assertLessEqual(len(hb.msg.encode("utf-8")), P.MSG_LIMIT)
        self.assertTrue(thai.startswith(hb.msg))
        # never between a consonant and its vowel or tone mark (Swift clips on Characters)
        for msg in ("x" * 61 + "น้ำ", "x" * 62 + "ดี", "ab" * 40):
            clipped = P.Heartbeat(1, 0, 0, msg=msg, epoch=0, tzoff=0).msg
            self.assertLessEqual(len(clipped.encode()), 63)
            self.assertEqual(P.graphemes(clipped), P.graphemes(msg)[:len(P.graphemes(clipped))])
        self.assertEqual(P.Heartbeat(1, 0, 0, msg="x" * 61 + "น้ำ", epoch=0, tzoff=0).msg, "x" * 61)

    def test_who(self):
        hb = P.Heartbeat(1, 1, 0, msg="", epoch=0, tzoff=0, who="nexus")
        self.assertIn(b'"who":"nexus"}', hb.json_line())
        self.assertEqual(P.Heartbeat(1, 1, 0, epoch=0, tzoff=0, who="n" * 30).who, "n" * 20)
        self.assertNotIn(b"who", P.Heartbeat(1, 1, 0, epoch=0, tzoff=0).json_line())

    def test_quote_escapes_like_swift(self):
        hb = P.Heartbeat(0, 0, 0, msg='a\\b\n\t\r\x01é ไทย', epoch=0, tzoff=0)
        self.assertIn('"msg":"a\\\\b\\n\\t\\u000d\\u0001é ไทย"'.encode(), hb.json_line())

    def test_crc_and_begin(self):
        self.assertEqual(P.crc32(b"123456789"), 0xCBF43926)
        self.assertEqual(P.crc32(b""), 0)
        self.assertEqual(P.begin(0x0A0B0C, 0xDEADBEEF), bytes([1, 0x0C, 0x0B, 0x0A, 0, 0xEF, 0xBE, 0xAD, 0xDE]))

    def test_status_lines(self):
        self.assertEqual(P.parse_status(b'{"ota":"error","why":"crc mismatch"}'), {"kind": "error", "why": "crc mismatch"})
        self.assertIsNone(P.parse_status(b'{"evt":"tap"}'))
        self.assertIsNone(P.parse_status(b"not json"))
        self.assertEqual(P.PetFirmware.parse(b'{"ver":"v1-3","slot":"ota_1","built":"Oct 21 2026 11:31:00"}').slot, "ota_1")
        self.assertEqual(P.parse_status(b'{"ota":"selftest","mic":true,"heard":true,"db":37.5,"quiet":22,"tone":191}'),
                         {"kind": "selftest", "mic": True, "heard": True, "db": 37.5, "quiet": 22, "tone": 191})
        self.assertEqual(P.parse_status(b'{"ota":"play","name":"say","bytes":1234}'),
                         {"kind": "play", "name": "say", "bytes": 1234})

    def test_window_never_runs_ahead(self):
        w = P.OtaWindow(10_000, 4096)
        sent = 0
        while (r := w.next(512)) is not None:
            sent += r[1] - r[0]
        self.assertEqual(sent, 4096)                                  # full: waits for an ack
        w.ack(2048)
        self.assertEqual(w.next(4096), (4096, 6144))                  # only what the ack freed
        w.ack(99_999)                                                 # never past what was sent
        self.assertEqual(w.acked, 6144)
        while w.next(512) is not None:
            pass
        w.ack(10_000)
        self.assertTrue(w.all_acked and w.sent == 10_000)
        self.assertTrue(P.OtaWindow(0, 4096).all_acked)

    def test_project_name_from_image(self):
        img = bytearray(300)
        img[0] = 0xE9
        img[32:36] = bytes([0x32, 0x54, 0xCD, 0xAB])
        img[80:94] = b"jc3248_pet_idf"
        self.assertEqual(P.project_name(bytes(img)), "jc3248_pet_idf")
        self.assertIsNone(P.project_name(bytes(300)))
        self.assertIsNone(P.project_name(bytes(img[:112])))           # too short to hold the name
        self.assertIsNone(P.describe(bytes(img[:150])))               # name but no sha

    def test_real_build_if_present(self):
        for build in ("build", "build-lcd28"):
            for f in sorted((HERE.parents[2] / "jc3248-pet-idf" / build).glob("*.bin")):
                d = P.describe(f.read_bytes())
                if d is not None:                                     # bootloader/partition .bins are not app images
                    with self.subTest(image=str(f)):
                        self.assertIn(d.project, P.PET_PROJECTS)
                        self.assertTrue(d.version and "20" in d.built and len(d.sha) == 16)

    def test_same_build(self):
        pet = P.PetFirmware("v", "ota_1", "a", sha="e5893e747")
        self.assertTrue(pet.same_build(P.PetFirmware("v", "", "a", sha="e5893e7471c0ffee")))
        self.assertFalse(pet.same_build(P.PetFirmware("v", "", "a", sha="bd09fced0359d3da")))
        self.assertTrue(P.PetFirmware("v", "", "a").same_build(P.PetFirmware("v", "", "a")))

    def test_partial_ota_commands(self):
        b = P.begin_file(1, 2, "characters/neo/idle.gif")
        self.assertTrue(b[0] == 0x04 and b[9] == 23 and b[-23:] == b"characters/neo/idle.gif")
        self.assertEqual(P.select_pack("neo"), bytes([0x05, 3]) + b"neo")
        fw = P.PetFirmware.parse(b'{"ver":"v","slot":"ota_0","pack":"neo","packs":["siamese","neo"],"free":900,'
                                 b'"project":"oraclepet_lcd28","apps":["talker","hello"]}')
        self.assertEqual((fw.packs, fw.free_kb, fw.pack, fw.apps), (["neo", "siamese"], 900, "neo", ["hello", "talker"]))
        self.assertEqual(fw.board, "2.8in ILI9341")
        self.assertEqual(P.board_name("jc3248_pet_idf"), "JC3248 3.5in")
        self.assertNotIn("esp32_other", P.PET_PROJECTS)

    def test_pet_paths(self):
        ok = ["characters/neo/idle.gif", "characters/neo/manifest.json", "apps/hello.wasm", "sounds/say.ima",
              "apps/" + "a" * 23 + ".wasm"]
        bad = ["characters/neo/Idle.gif", "characters/neo.gif", "characters/a/b/c.gif", "apps/x/y.wasm",
               "apps/.wasm", "apps/" + "a" * 24 + ".wasm", "sounds/x.wav", "characters/../x/y", "other/x.gif",
               "characters/n/" + "x" * 90]
        for p in ok:
            self.assertTrue(P.pet_accepts_path(p), p)
        for p in bad:
            self.assertFalse(P.pet_accepts_path(p), p)

    def test_psnd_header_and_blocks(self):
        c = P.psnd_encode([0] * 1000)
        self.assertEqual(c[:16], bytes([0x50, 0x53, 0x4E, 0x44, 1, 1, 0, 0, 0x80, 0x3E, 0, 0, 0xE8, 0x03, 0, 0]))
        self.assertEqual(len(c), 16 + 2 * 256)                        # 1000 samples = 2 blocks of 504
        self.assertEqual(P.psnd_encode([]), c[:12] + bytes(4))

    def test_round_trip_tone_keeps_its_shape(self):
        pcm = [int(16_000 * math.sin(2 * math.pi * 440 * i / 16_000)) for i in range(16_000)]
        back = P.psnd_decode(P.psnd_encode(pcm))
        self.assertEqual(len(back), len(pcm))
        sig = sum(a * a for a in pcm)
        err = sum((a - b) ** 2 for a, b in zip(pcm, back, strict=True))
        self.assertGreater(10 * math.log10(sig / max(err, 1)), 20)

    def test_low_nibble_first(self):
        c = P.psnd_encode([1000, 1000])
        self.assertTrue(c[20] & 0x0F and c[20] >> 4)

    def test_decode_short_and_bad(self):
        clip = P.psnd_encode(list(range(0, 3000, 3)))
        self.assertEqual(len(P.psnd_decode(clip[:16 + 256 + 10])), 504 + 12)   # cut mid-block: as far as it goes
        self.assertIsNone(P.psnd_decode(b"RIFF" + clip[4:]))
        self.assertIsNone(P.psnd_decode(clip[:15]))

    def test_tidy_trims_and_scales(self):
        pcm = [0] * 1000 + [10_000, -10_000, 5_000] + [0] * 1000
        t = P.tidy(pcm)
        self.assertTrue(3 <= len(t) < len(pcm))
        self.assertEqual(len(t), 3 + 2 * 320)                         # 20 ms kept on each side
        self.assertEqual(max(abs(x) for x in t), round(32767 * 0.9))
        self.assertEqual(P.tidy([0, 300, -300] * 100), [])            # |s| <= 300 is quiet
        self.assertEqual(len(P.tidy([1000] * 200_000)), 96_000)       # capped at 6 s
        self.assertEqual(P.tidy([-32768, 0])[0], -29490)
        # Swift's .rounded() goes half away from zero; Python's round() would not
        self.assertEqual([P._round_half_away(x) for x in (2.5, -2.5, 1.5, -0.5, 0.49999999999999994, 3.2)],
                         [3, -3, 2, -1, 0, 3])
        self.assertTrue(P.is_thai("นีโอรอคุณอยู่") and not P.is_thai("neo needs you"))

    def test_stream_and_listen_commands(self):
        b = P.begin_stream(0x0102, 1)
        self.assertTrue(b[0] == 0x0B and len(b) == 9)
        self.assertEqual(P.listen(3000, True), bytes([0x0C, 0xB8, 0x0B, 1]))
        self.assertEqual(P.listen(50, False), bytes([0x0C, 0x64, 0x00, 0]))      # clamped to 100 ms
        self.assertEqual(P.listen(60_000, False), bytes([0x0C, 0x10, 0x27, 0]))  # and to 10 s
        self.assertEqual(P.progress(150), bytes([7, 100]))
        self.assertEqual(P.progress(-5), bytes([7, 0]))
        st = P.parse_status(b'{"ota":"heard","ms":3000,"samples":48000,"rms":412,"peak":9120,"freq":1000,'
                            b'"db":23.4,"floor":31,"sent":true}')
        self.assertTrue(st["freq"] == 1000 and st["rms"] == 412 and st["sent"] and abs(st["db"] - 23.4) < 0.01)

    def test_wav_header(self):
        w = P.wav([0, 1, -1])
        self.assertTrue(len(w) == 44 + 6 and w[:4] == b"RIFF" and w[8:12] == b"WAVE")
        self.assertEqual(struct.unpack_from("<3h", w, 44), (0, 1, -1))

    def test_events_span_notifications(self):
        buf = bytearray()
        self.assertEqual(list(P.event_lines(buf, b'{"evt":"ta')), [])
        self.assertEqual(list(P.event_lines(buf, b'p"}\n{"evt":"say","text":"hi"}\nnoise\n')),
                         [{"evt": "tap"}, {"evt": "say", "text": "hi"}])
        self.assertEqual(buf, b"")

    def test_graphemes(self):
        self.assertEqual(P.graphemes("สวัสดี"), ["ส", "วั", "ส", "ดี"])
        self.assertEqual(P.graphemes("น้ำ"), ["น้ำ"])
        self.assertEqual(len(P.graphemes("👍🏽🇹🇭🇯🇵é\r\n")), 5)
        self.assertEqual(len(P.graphemes("👨‍👩‍👧")), 1)
        self.assertEqual(len(P.graphemes("한국어")), 3)
        self.assertEqual(len(P.graphemes("각")), 1)    # conjoining jamo


if __name__ == "__main__":
    unittest.main()
