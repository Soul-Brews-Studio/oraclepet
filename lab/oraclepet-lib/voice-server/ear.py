"""The server's ear: is there speech in an utterance, and what was said. Local and free.

- Speech: Silero VAD (ONNX, ~2 MB, ~5 ms per second of audio on an M-series CPU). It tells a voice
  from a fan, the WiFi's bursts, or a closing door, which an energy threshold cannot.
- Words: Typhoon Whisper turbo (Thai) on the GPU through MLX (~0.5 s for a short sentence on m5).
  Only the speech parts are transcribed; an utterance with no speech costs nothing.

Models: ~/OraclePet/models/silero_vad.onnx (downloaded once from snakers4/silero-vad) and the
Hugging Face cache for chayapats/typhoon-whisper-turbo-mlx. Both load at server start.
"""
from __future__ import annotations

import os
import re
import threading
import urllib.request
from pathlib import Path

SR = 16000
SILERO_URL = "https://github.com/snakers4/silero-vad/raw/master/src/silero_vad/data/silero_vad.onnx"
SILERO = Path(os.environ.get("ORACLEPET_SILERO", Path.home() / "OraclePet" / "models" / "silero_vad.onnx"))
# Typhoon Whisper turbo (typhoon-ai, MIT; Thai fine-tune of whisper-large-v3-turbo) in an MLX build.
# On a Google Meet call recorded by the pet it got the Thai right where the generic model did not
# ("กทม." vs "กอธรรมมอ์", "แสดง" vs "แซดง") at the same ~0.5 s per utterance on m5's GPU.
WHISPER = os.environ.get("ORACLEPET_WHISPER", "chayapats/typhoon-whisper-turbo-mlx")


_LOOP = re.compile(r"(.{1,4}?)\1{5,}")


def clean_text(text: str) -> str:
    """Whisper loops on noise ("ตกตกตกตก…", "กันกันกัน…"): keep one copy of any 1-4 character unit
    repeated more than 5 times; if a loop was all there was, nothing is left. A real one-word
    answer ("ครับ") is kept."""
    t = _LOOP.sub(lambda m: m.group(1), text).strip()
    if len(t) <= 4 and len(text.strip()) > 3 * max(len(t), 1):     # only a loop was there
        return ""
    return t


class Ear:
    def __init__(self, start: float = 0.5, stop: float = 0.35):
        self.start, self.stop = start, stop
        self._lock = threading.Lock()          # one model run at a time
        self._vad = None

    def _session(self):
        if self._vad is None:
            import onnxruntime as ort
            if not SILERO.exists():
                SILERO.parent.mkdir(parents=True, exist_ok=True)
                urllib.request.urlretrieve(SILERO_URL, SILERO)
            self._vad = ort.InferenceSession(str(SILERO))
        return self._vad

    def speech(self, pcm: list[int]) -> list[tuple[float, float]]:
        """Speech regions (start s, end s), from Silero's per-32 ms probability with hysteresis."""
        import numpy as np
        with self._lock:
            s = self._session()
            x = np.asarray(pcm, dtype=np.float32) / 32768.0
            state = np.zeros((2, 1, 128), dtype=np.float32)
            ctx = np.zeros(64, dtype=np.float32)
            sr = np.array(SR, dtype=np.int64)
            probs = []
            for i in range(0, len(x) - 511, 512):
                out, state = s.run(None, {"input": np.concatenate([ctx, x[i:i + 512]])[None, :], "state": state, "sr": sr})
                ctx = x[i + 448:i + 512]
                probs.append(float(out[0][0]))
        regions, on, a = [], False, 0
        for k, p in enumerate(probs):
            if not on and p > self.start:
                on, a = True, k
            elif on and p < self.stop:
                on = False
                regions.append((a * 512 / SR, k * 512 / SR))
        if on:
            regions.append((a * 512 / SR, len(probs) * 512 / SR))
        return regions

    def words(self, pcm: list[int], regions: list[tuple[float, float]], language: str = "th") -> str:
        """Whisper on the speech parts only (each widened by 0.2 s, joined with 0.3 s of silence)."""
        import numpy as np
        import mlx_whisper
        x = np.asarray(pcm, dtype=np.float32) / 32768.0
        gap = np.zeros(int(0.3 * SR), dtype=np.float32)
        parts = []
        for a, b in regions:
            parts += [x[max(0, int((a - 0.2) * SR)):int((b + 0.2) * SR)], gap]
        audio = np.concatenate(parts) if parts else x
        with self._lock:
            r = mlx_whisper.transcribe(audio, path_or_hf_repo=WHISPER, language=language, fp16=True,
                                       condition_on_previous_text=False)
        return clean_text(str(r.get("text", "")))
