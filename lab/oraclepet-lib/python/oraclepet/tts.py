"""Text to speech for the pet, with what each OS already has (no pip packages).

The pet has no synthesis of its own (PROTOCOL.md 6): the host renders the text, and Pet.say()
turns it into a PSND clip. This module only gets 16 kHz mono int16 samples out of the OS:

    macOS    say -o x.wav --file-format=WAVE --data-format=LEI16@16000  (Thai text: the Kanya voice)
    Linux    espeak-ng --stdout  (or espeak), 22.05 kHz, resampled here
    Windows  PowerShell + System.Speech (SAPI) to a 16 kHz WAV

The WAV reader and the resampler are pure Python, so the result is the same on every OS.
"""
from __future__ import annotations

import base64
import functools
import math
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from array import array
from pathlib import Path

from .protocol import SAMPLE_RATE, is_thai


class SpeechError(RuntimeError):
    """The OS could not render the text."""


def language_of(text: str, language: str | None = None) -> str:
    """"th" / "en" / another primary subtag: the given language (th-TH -> th), else Thai when
    the text has Thai characters, else English (PetVoice's rule)."""
    if language:
        return re.split(r"[-_]", language.strip().lower())[0] or "en"
    return "th" if is_thai(text) else "en"


def synthesize(text: str, language: str | None = None, voice: str | None = None,
               rate: int = SAMPLE_RATE) -> list[int]:
    """Speak `text` with this OS's synthesizer; 16 kHz (rate) mono int16 samples."""
    if not text.strip():
        return []
    lang = language_of(text, language)
    with tempfile.TemporaryDirectory(prefix="oraclepet-tts-") as tmp:
        if sys.platform == "darwin":
            data = _macos(text, lang, voice, Path(tmp))
        elif sys.platform.startswith("win"):
            data = _windows(text, lang, voice, Path(tmp))
        else:
            data = _espeak(text, lang, voice)
    pcm, sr = read_wav(data)
    return resample(pcm, sr, rate)


# ---- macOS ----
@functools.lru_cache(maxsize=1)
def _mac_voices() -> list[tuple[str, str]]:
    """(name, locale) from `say -v ?`, e.g. ("Kanya", "th_TH"). Takes ~1 s, so once per process."""
    try:
        out = subprocess.run(["say", "-v", "?"], capture_output=True, text=True, timeout=20).stdout
    except (OSError, subprocess.SubprocessError):
        return []
    voices = []
    for line in out.splitlines():
        m = re.match(r"^(.+?)\s+([a-z]{2,3}_[A-Za-z0-9]+)\s+#", line)
        if m:
            voices.append((m.group(1).split(" (")[0].strip(), m.group(2)))   # "Kanya (Thai (Thailand))" -> "Kanya"
    return voices


def _mac_voice(lang: str) -> str | None:
    if lang == "en":
        return "Samantha"               # without it, say quietly uses the system voice: fine for English
    # a missing voice also falls back silently, so look for a Thai one rather than guess
    names = [n for n, loc in _mac_voices() if loc.lower().startswith(lang + "_")]
    preferred = {"th": "Kanya"}.get(lang)
    if preferred in names:
        return preferred
    return names[0] if names else None


def _macos(text: str, lang: str, voice: str | None, tmp: Path) -> bytes:
    if shutil.which("say") is None:
        raise SpeechError("macOS `say` not found")
    src, out = tmp / "text.txt", tmp / "speech.wav"
    src.write_text(text, encoding="utf-8")          # a file, so text starting with "-" is not an option
    cmd = ["say", "-o", str(out), "--file-format=WAVE", "--data-format=LEI16@16000", "-f", str(src)]
    v = voice or _mac_voice(lang)
    if v:
        cmd[1:1] = ["-v", v]
    # `say` sometimes hangs for good (seen ~1 run in 10 on macOS 27): a phrase renders in well
    # under a second, so give each try 15 s, as PetVoice gives the synthesizer, and try twice.
    r = _run(cmd, tries=2, timeout=15, text=True)
    if r.returncode != 0 or not out.exists():
        raise SpeechError(f"say failed: {r.stderr.strip() or r.returncode}")
    return out.read_bytes()


def _run(cmd: list, *, tries: int = 1, timeout: float = 120, **kw) -> subprocess.CompletedProcess:
    """subprocess.run that retries a hung synthesizer, and reports failures as SpeechError
    (Pet.say turns that into a PetError) instead of leaking TimeoutExpired / OSError."""
    for attempt in range(1, tries + 1):
        try:
            return subprocess.run(cmd, capture_output=True, timeout=timeout, **kw)
        except subprocess.TimeoutExpired:
            if attempt == tries:
                raise SpeechError(f"{Path(cmd[0]).name} did not finish in {timeout:g} s "
                                  f"({tries} tries)") from None
        except OSError as e:
            raise SpeechError(f"{Path(cmd[0]).name} could not run: {e}") from e
    raise AssertionError("unreachable")


# ---- Linux: espeak-ng ----
def _espeak(text: str, lang: str, voice: str | None) -> bytes:
    exe = shutil.which("espeak-ng") or shutil.which("espeak")
    if exe is None:
        raise SpeechError("no speech synthesizer: install espeak-ng (apt install espeak-ng)")
    v = voice or {"en": "en-us"}.get(lang, lang)
    for args in (["-v", v], []):                    # an unknown voice: fall back to the default one
        r = _run([exe, "--stdout", "-b", "1", *args, "--stdin"], input=text.encode("utf-8"), timeout=120)
        if r.returncode == 0 and len(r.stdout) > 44:
            return r.stdout
    raise SpeechError(f"{Path(exe).name} failed: {r.stderr.decode(errors='replace').strip() or r.returncode}")


# ---- Windows: System.Speech through PowerShell ----
_PS = r"""
Add-Type -AssemblyName System.Speech
$s = New-Object System.Speech.Synthesis.SpeechSynthesizer
if ($env:ORACLEPET_VOICE) { $s.SelectVoice($env:ORACLEPET_VOICE) }
else {
  foreach ($v in $s.GetInstalledVoices()) {
    if ($v.Enabled -and $v.VoiceInfo.Culture.Name -like ($env:ORACLEPET_LANG + '*')) { $s.SelectVoice($v.VoiceInfo.Name); break }
  }
}
$fmt = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo(16000,
  [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen, [System.Speech.AudioFormat.AudioChannel]::Mono)
$s.SetOutputToWaveFile($env:ORACLEPET_WAV, $fmt)
$s.Speak([System.IO.File]::ReadAllText($env:ORACLEPET_TEXT, [System.Text.Encoding]::UTF8))
$s.Dispose()
"""


def _windows(text: str, lang: str, voice: str | None, tmp: Path) -> bytes:
    exe = shutil.which("powershell") or shutil.which("pwsh")    # Windows PowerShell has System.Speech
    if exe is None:
        raise SpeechError("PowerShell not found")
    src, out = tmp / "text.txt", tmp / "speech.wav"
    src.write_text(text, encoding="utf-8")
    env = {**os.environ, "ORACLEPET_TEXT": str(src), "ORACLEPET_WAV": str(out),
           "ORACLEPET_LANG": lang, "ORACLEPET_VOICE": voice or ""}
    script = base64.b64encode(_PS.encode("utf-16-le")).decode("ascii")   # -EncodedCommand: no quoting issues
    r = _run([exe, "-NoProfile", "-NonInteractive", "-EncodedCommand", script], env=env, text=True, timeout=120)
    if r.returncode != 0 or not out.exists():
        raise SpeechError(f"System.Speech failed: {r.stderr.strip() or r.returncode}")
    return out.read_bytes()


# ---- WAV in, resample ----
def read_wav(data: bytes) -> tuple[list[int], int]:
    """A RIFF/WAVE file -> (mono int16 samples, sample rate). PCM 8/16/24/32-bit or float
    32/64, any channel count (averaged), WAVE_FORMAT_EXTENSIBLE, unknown chunks skipped, and
    a streamed file whose sizes are placeholders (espeak-ng --stdout)."""
    if len(data) < 12 or data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise SpeechError("not a WAV file")
    fmt = body = None
    off = 12
    while off + 8 <= len(data):
        cid, size = data[off:off + 4], struct.unpack_from("<I", data, off + 4)[0]
        start = off + 8
        if cid == b"fmt ":
            tag, channels, rate, _, _, bits = struct.unpack_from("<HHIIHH", data, start)
            if tag == 0xFFFE and size >= 40:            # extensible: the real tag opens the sub-format GUID
                tag = struct.unpack_from("<H", data, start + 24)[0]
            fmt = (tag, channels, rate, bits)
        elif cid == b"data":
            end = start + size if 0 < size <= len(data) - start else len(data)
            body = data[start:end]
            break
        off = start + size + (size & 1)
    if fmt is None or body is None:
        raise SpeechError("WAV without fmt or data")
    tag, channels, rate, bits = fmt
    width = bits // 8
    if channels < 1 or width < 1:
        raise SpeechError("bad WAV format")
    body = body[:len(body) - len(body) % (width * channels)]
    if tag == 1 and bits == 16:
        a = array("h")
        a.frombytes(body)
        if sys.byteorder == "big":
            a.byteswap()
        raw = a.tolist()
    elif tag == 1 and bits == 8:
        raw = [(b - 128) << 8 for b in body]
    elif tag == 1 and bits == 24:
        raw = [int.from_bytes(body[i:i + 3], "little", signed=True) >> 8 for i in range(0, len(body), 3)]
    elif tag == 1 and bits == 32:
        raw = [v >> 16 for v in struct.unpack(f"<{len(body) // 4}i", body)]
    elif tag == 3 and bits in (32, 64):
        floats = struct.unpack(f"<{len(body) // width}{'f' if bits == 32 else 'd'}", body)
        raw = [max(-32768, min(32767, int(round(f * 32767)))) for f in floats]
    else:
        raise SpeechError(f"unsupported WAV format (tag {tag}, {bits} bits)")
    if channels > 1:
        raw = [sum(raw[i:i + channels]) // channels for i in range(0, len(raw), channels)]
    return raw, rate


def resample(pcm: list[int], src: int, dst: int = SAMPLE_RATE) -> list[int]:
    """Linear interpolation from src Hz to dst Hz (speech; no anti-alias filter)."""
    if src == dst or not pcm:
        return list(pcm)
    n = int(len(pcm) * dst / src)
    step = src / dst
    last = len(pcm) - 1
    out = []
    for i in range(n):
        x = i * step
        j = int(x)
        a = pcm[min(j, last)]
        b = pcm[min(j + 1, last)]
        v = a + (b - a) * (x - j)
        out.append(max(-32768, min(32767, math.floor(v + 0.5))))
    return out
