"""Speech plumbing: the pure-Python WAV reader and resampler (the same on every OS), the
language rule, and one live run of this OS's synthesizer when it has one. Offline."""
from __future__ import annotations

import math
import shutil
import struct
import subprocess
import sys
import unittest
from unittest import mock

from oraclepet import protocol as P
from oraclepet import tts


def wav_file(samples, rate=16_000, channels=1, bits=16, tag=1, extensible=False, data_size=None, extra=b""):
    width = bits // 8
    if tag == 3:
        body = struct.pack(f"<{len(samples)}{'f' if bits == 32 else 'd'}", *samples)
    elif bits == 8:
        body = bytes(s + 128 for s in samples)
    elif bits == 24:
        body = b"".join(s.to_bytes(3, "little", signed=True) for s in samples)
    else:
        body = struct.pack(f"<{len(samples)}{'h' if bits == 16 else 'i'}", *samples)
    if extensible:
        guid_tail = b"\x00\x00\x00\x00\x10\x00\x80\x00\x00\xaa\x00\x38\x9b\x71"
        fmt = struct.pack("<HHIIHHHHI", 0xFFFE, channels, rate, rate * channels * width, channels * width, bits,
                          22, bits, 0) + struct.pack("<H", tag) + guid_tail
    else:
        fmt = struct.pack("<HHIIHH", tag, channels, rate, rate * channels * width, channels * width, bits)
    chunks = b"fmt " + struct.pack("<I", len(fmt)) + fmt + extra
    chunks += b"data" + struct.pack("<I", len(body) if data_size is None else data_size) + body
    return b"RIFF" + struct.pack("<I", 4 + len(chunks)) + b"WAVE" + chunks


class WavAndResample(unittest.TestCase):
    def test_round_trip_with_our_writer(self):
        pcm = [0, 1, -1, 32767, -32768, 1234]
        self.assertEqual(tts.read_wav(P.wav(pcm)), (pcm, 16_000))

    def test_formats(self):
        pcm = [0, 1000, -1000, 32767, -32768]
        self.assertEqual(tts.read_wav(wav_file(pcm))[0], pcm)
        self.assertEqual(tts.read_wav(wav_file(pcm, extensible=True))[0], pcm)
        self.assertEqual(tts.read_wav(wav_file([0, 100, -100], bits=8))[0], [0, 100 << 8, -100 << 8])
        self.assertEqual(tts.read_wav(wav_file([256, -512, 8_388_607], bits=24))[0], [1, -2, 32767])
        self.assertEqual(tts.read_wav(wav_file([65536, -131072], bits=32))[0], [1, -2])
        self.assertEqual(tts.read_wav(wav_file([0.0, 0.5, -1.0], bits=32, tag=3))[0], [0, 16384, -32767])
        self.assertEqual(tts.read_wav(wav_file([0.0, 1.0], bits=64, tag=3))[0], [0, 32767])
        stereo = tts.read_wav(wav_file([100, 300, -100, -300], channels=2, rate=22_050))
        self.assertEqual(stereo, ([200, -200], 22_050))

    def test_odd_layouts(self):
        pcm = [5, 6, 7, 8]
        # macOS `say` puts an Apple FLLR chunk before the data; odd-sized chunks are padded
        self.assertEqual(tts.read_wav(wav_file(pcm, extra=b"FLLR" + struct.pack("<I", 3) + b"abc\0"))[0], pcm)
        # espeak-ng --stdout cannot seek back: the data size is a placeholder
        self.assertEqual(tts.read_wav(wav_file(pcm, data_size=0xFFFFFFFF))[0], pcm)
        self.assertEqual(tts.read_wav(wav_file(pcm, data_size=0))[0], pcm)
        with self.assertRaises(tts.SpeechError):
            tts.read_wav(b"not a wav file at all")
        with self.assertRaises(tts.SpeechError):
            tts.read_wav(wav_file(pcm, bits=16, tag=2))           # ADPCM WAV: not ours to decode

    def test_resample(self):
        self.assertEqual(tts.resample([1, 2, 3], 16_000), [1, 2, 3])
        up = tts.resample([0, 100], 8_000, 16_000)
        self.assertEqual(up, [0, 50, 100, 100])
        tone = [int(10_000 * math.sin(2 * math.pi * 440 * i / 22_050)) for i in range(22_050)]
        out = tts.resample(tone, 22_050, 16_000)
        self.assertEqual(len(out), 16_000)                       # one second stays one second
        ref = [int(10_000 * math.sin(2 * math.pi * 440 * i / 16_000)) for i in range(16_000)]
        err = max(abs(a - b) for a, b in zip(out, ref, strict=True))
        self.assertLess(err, 300)                                # linear interpolation of a 440 Hz tone

    def test_language(self):
        self.assertEqual(tts.language_of("สวัสดีครับ"), "th")
        self.assertEqual(tts.language_of("hello"), "en")
        self.assertEqual(tts.language_of("hello", "th-TH"), "th")
        self.assertEqual(tts.language_of("สวัสดี", "en_US"), "en")

    def test_mac_voice_list(self):
        listing = ("Albert              en_US    # Hello! My name is Albert.\n"
                   "Eddy (English (US)) en_US    # Hello! My name is Eddy.\n"
                   "Kanya (Thai (Thailand)) th_TH    # สวัสดี! ฉันชื่อกันยา\n"
                   "Narisa              th_TH    # สวัสดี\n")
        run = mock.Mock(return_value=mock.Mock(stdout=listing))
        self.addCleanup(tts._mac_voices.cache_clear)
        tts._mac_voices.cache_clear()
        with mock.patch("subprocess.run", run):
            self.assertEqual(tts._mac_voices()[:3], [("Albert", "en_US"), ("Eddy", "en_US"), ("Kanya", "th_TH")])
            self.assertEqual(tts._mac_voice("th"), "Kanya")
            self.assertEqual(tts._mac_voice("en"), "Samantha")      # no listing needed for English
            self.assertIsNone(tts._mac_voice("ja"))                 # no Japanese voice: the system voice
        self.assertEqual(run.call_count, 1)                         # listed once per process
        tts._mac_voices.cache_clear()
        with mock.patch("subprocess.run", mock.Mock(return_value=mock.Mock(stdout=listing.replace("Kanya", "Other")))):
            self.assertEqual(tts._mac_voice("th"), "Other")


class HungSynthesizer(unittest.TestCase):
    """macOS `say` sometimes never exits: retry once, then a SpeechError (not TimeoutExpired)."""

    def test_a_hang_is_retried(self):
        ok = mock.Mock(returncode=0)
        run = mock.Mock(side_effect=[subprocess.TimeoutExpired("say", 15), ok])
        with mock.patch("subprocess.run", run):
            self.assertIs(tts._run(["say"], tries=2, timeout=15), ok)
        self.assertEqual(run.call_count, 2)

    def test_two_hangs_are_a_speech_error(self):
        run = mock.Mock(side_effect=subprocess.TimeoutExpired("say", 15))
        with mock.patch("subprocess.run", run), self.assertRaises(tts.SpeechError):
            tts._run(["/usr/bin/say"], tries=2, timeout=15)
        self.assertEqual(run.call_count, 2)

    def test_a_missing_program_is_a_speech_error(self):
        with mock.patch("subprocess.run", mock.Mock(side_effect=FileNotFoundError("espeak-ng"))), \
                self.assertRaises(tts.SpeechError):
            tts._run(["espeak-ng"])


@unittest.skipUnless(shutil.which("say") if sys.platform == "darwin" else
                     shutil.which("espeak-ng") or shutil.which("espeak") or shutil.which("powershell"),
                     "no speech synthesizer on this machine")
class LiveSynth(unittest.TestCase):
    def test_hello_becomes_a_clip(self):
        pcm = tts.synthesize("hello pet", "en")
        clip = P.psnd_encode(P.tidy(pcm))
        self.assertGreater(len(pcm), 4000)                       # a quarter second at least
        self.assertLessEqual(len(P.tidy(pcm)), 6 * 16_000)
        self.assertEqual(P.psnd_header(clip)[0], 16_000)


if __name__ == "__main__":
    unittest.main()
