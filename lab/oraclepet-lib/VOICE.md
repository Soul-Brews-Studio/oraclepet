# OraclePet voice over WiFi (v1)

The pet's microphone, streamed to a server on the Mac (m5) over WiFi. The pet decides when
someone speaks (an energy voice-activity detector) and sends one *utterance* at a time; the
server saves each one with its transcript. BLE stays for control (PROTOCOL.md); WiFi carries
the audio.

```
pet mic 16 kHz ──VAD──▶ IMA ADPCM blocks ──WebSocket──▶ voice server on m5
                                                         ├─ <date>/<time>-<pet>-<id>.wav
                                                         ├─ <date>/<time>-<pet>-<id>.json (meta + text)
                                                         └─ log.jsonl (timeline)
```

## 1. Provisioning (over BLE, PROTOCOL.md §4.2)

| byte | name | payload | reply on control |
|---|---|---|---|
| `0x0D` | WIFI_CONFIG | u8 len, ssid · u8 len, password · u8 len, server URL | `{"ota":"wifi","state":"connecting"}` then `"up"` with `"ip"`, or `"error"` with `"why"` |
| `0x0E` | VOICE | u8 mode: 0 off, 1 stream speech | `{"ota":"voice","mode":1}` |

The pet stores SSID, password and URL in NVS and reconnects after a reboot. Control read
(§4.1) gains `"wifi":"up 192.168.1.23"` (or `"off"`, `"connecting"`, `"error: …"`) and
`"voice":1`. The password is never sent back.

## 2. WebSocket (pet → server)

URL from WIFI_CONFIG, e.g. `ws://m5.local:4801/pet`. One connection per pet. Text frames
are JSON, binary frames are audio.

1. After connecting, the pet sends
   `{"type":"hello","pet":"Claude-D350","project":"oraclepet_lcd28","fw":"…","sr":16000,"codec":"ima-psnd"}`.
2. An utterance:
   - `{"type":"start","id":7,"preroll_ms":300,"floor":12}`: `id` counts up from 1 per boot.
   - binary frames, each one or more whole **PSND blocks** (PROTOCOL.md §5: 256 bytes,
     i16 predictor, u8 index, u8 0, 252 bytes = 504 samples, low nibble first). Blocks are
     self-contained, so a lost frame costs only its own audio.
   - `{"type":"end","id":7,"samples":24192,"reason":"silence","rms":410,"peak":9000}`:
     `samples` is the exact count (the last block may be partial); `reason` is `silence`,
     `max` (30 s cap) or `stop` (voice mode turned off).
3. The server may answer `{"type":"heard","id":7,"text":"…"}`; the pet may show the text.

The pet pings every 10 s; the server answers pongs (standard WebSocket ping/pong).

## 3. The pet's VAD

The mic first goes through a 200 Hz high-pass (2nd order): below it on the ES3C28P board is
WiFi rumble, not speech. Then every 20 ms frame: RMS over the frame; the noise floor is the
quietest frame of the last 3 s, tracked all the time. Two thresholds (hysteresis):

- **Start**: the frame and the two before it all > max(2 × floor, floor + 60). WiFi noise on
  this board comes as single 20 ms bursts; a syllable lasts 60 ms or more. The utterance
  includes the 300 ms before that (pre-roll).
- **Hold**: a frame is *voiced* when it and the frame before it are > max(1.5 × floor,
  floor + 30). A single burst is never voiced.
- **End**: fewer than 3 of the last 60 frames (1.2 s) were voiced, or 30 s in total.

An utterance with under 260 ms of voiced frames ends with reason `short`, and the server drops
it. The server's Silero VAD decides what is speech, so the pet errs towards sending noise rather
than losing a quiet word.

Re-tuned 2026-10-21 on a Google Meet call heard from the Mac's speakers (floor about 10, speech
RMS 150 to 300). The previous gate (start and hold at max(2 × floor, floor + 120), end after
800 ms) sent 62% of the call, in pieces with a median gap of 0.8 s, and lost the soft
syllables in between.

Replayed on the same audio (`voice-server/tools/gate-sim.py`, the 7 utterances with speech
in the first 6 minutes after the update, 39.9 s): the new gate sends all 39.9 s, as the pet did.
The old gate would have sent 18.1 s (45%). It missed one quiet word entirely (RMS 47), and it cut
a 15 s sentence into 4 pieces and an 8.5 s one into 3. The cost: 12 of the 18 utterances in that
window were noise, which Silero rejected on the server (Mac CPU only, nothing transcribed).
`voice-server/tools/vad-stats.py` gives coverage and gaps for any time window.

## 4. The server

`voice-server/` in this folder (Python, `uv run`). Port 4801 by default. It writes to
`~/OraclePet/voice-log/` and serves a page at `http://127.0.0.1:4801/` listing the
utterances newest first, with an audio player, the transcript, pet, time, length and levels.
Transcription: Paxa STT (`https://api.paxalabs.com/v1/stt`, Thai first) when a key is in the
Keychain (`security find-generic-password -s co.laris.oraclepet.paxa -w`); otherwise the
text is left empty and can be filled later.
