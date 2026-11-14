# OraclePet BLE protocol (v1)

The oracle pet (ESP32-S3: the JC3248 3.5in board `jc3248_pet_idf`, the 2.8in ILI9341 board
`oraclepet_lcd28`) speaks plain bytes over Bluetooth LE. Any host that can do BLE GATT can drive
it: the OraclePet Mac app (Swift), the Python library (`python/`, bleak: macOS, Linux, Windows),
the web page (`web/`, Web Bluetooth: Chrome/Edge on any OS, Android). This file is the single
source of truth; `golden/vectors.json` pins every byte layout with test vectors, and every
implementation's tests must reproduce them.

All integers are **little-endian**. Strings are UTF-8 without a terminator. Firmware sources:
`lab/jc3248-pet-idf/main/{ble.c,ota.c,ota.h,audio.c,petapp.c}`.

## 1. Discovery

- The pet's name is **`Claude-XXXX`** (the last two bytes of its MAC in hex, e.g. `Claude-AD48`).
  The name and the NUS service UUID are in the scan response (scan actively, or filter on the
  NUS UUID); the advertisement itself carries Improv (§2).
- One central at a time. A second host must wait until the first disconnects.
- Open link: no pairing, no bonding.

## 2. Services

| Service | UUID | Characteristics |
|---|---|---|
| NUS (Nordic UART) | `6e400001-b5a3-f393-e0a9-e50e24dcca9e` | RX `6e400002-…` write / write-without-response (host → pet); TX `6e400003-…` notify (pet → host) |
| Pet OTA | `6e400010-b5a3-f393-e0a9-e50e24dcca9e` | control `6e400011-…` read + write (with response) + notify; data `6e400012-…` write-without-response; mic `6e400013-…` notify |

All UUIDs share the NUS base `6e40xxxx-b5a3-f393-e0a9-e50e24dcca9e`.

The pet also carries the standard **Improv Wi-Fi** service (`00467768-6228-2272-4663-277478268000`,
https://www.improv-wifi.com/ble/), so any Improv client can give it a WiFi network. It
advertises the Improv UUID and service data (0x4677) in the advertisement; the name and the
NUS UUID are in the scan response.

## 3. NUS: heartbeat and events

### 3.1 Heartbeat (host → pet, RX)

One JSON object per line, terminated by `\n`. The pet reassembles lines across writes (a line
may be split over several ATT writes; keep each write ≤ the negotiated MTU − 3).

```json
{"total":3,"running":1,"waiting":2,"msg":"wait: neo","epoch":1792547421,"tzoff":25200,"pct5h":8,"reset5h":14100,"who":"neo"}
```

| key | type | meaning |
|---|---|---|
| `total` | int | agents awake (pet state rule below) |
| `running` | int | agents working |
| `waiting` | int | agents waiting for the human |
| `msg` | string ≤ 63 UTF-8 bytes | status line under the HUD |
| `epoch`, `tzoff` | int | UTC seconds and local offset in seconds (clock) |
| `pct5h`, `reset5h`, `pct7d`, `reset7d` | int, optional | usage windows |
| `who` | string ≤ 20 chars, optional | name drawn under the pet (the working/waiting oracle) |

Pet state rule: `waiting > 0` → attention; else `running > 0` → busy; else `total == 0` → sleep;
else idle. A heartbeat older than 30 s counts as idle. Send one every ~5 s.

### 3.2 Events (pet → host, TX)

One JSON object per line, `\n`-terminated:

- `{"evt":"tap"}`: the screen was touched.
- `{"evt":"say","text":"…"}`: a pet app called `pet.say(text)`; the host should speak it (§6).

## 4. Pet OTA service

### 4.1 Reading control: what the pet runs

Reading `6e400011` returns one JSON object (≤ 512 bytes):

```json
{"ver":"6ac87a5-dirty","project":"oraclepet_lcd28","built":"Oct 21 2026 13:23:37","slot":"ota_1",
 "sha":"657a5f60a","state":"valid","pack":"siamese","packs":["atlas","siamese"],"free":1244,
 "app":"talker","apps":["hello","talker"],"apperr":""}
```

`project` identifies the board; a host must never send one board's firmware to the other.
`sha` is a prefix of the ELF SHA-256 (the pet reports 9 hex digits; compare on the common prefix).
`state` is `pending` until a new image proves itself, then `valid`.

### 4.2 Commands (write to control, with response)

| byte | name | payload | effect / reply |
|---|---|---|---|
| `0x01` | BEGIN | u32 size, u32 crc32 | full OTA of an app image; then data; `{"ota":"done"}` and reboot |
| `0x02` | END | — | all bytes acked: finish the transfer |
| `0x03` | ABORT | — | stop the transfer |
| `0x04` | BEGIN_FILE | u32 size, u32 crc32, u8 len, path | one file into LittleFS; `{"ota":"saved","path":…}` |
| `0x05` | SELECT_PACK | u8 len, name | show that animation pack; `{"ota":"pack","name":…}` |
| `0x06` | RUN_APP | u8 len, name (len 0 = stop) | run `apps/<name>.wasm`; `{"ota":"app","name":…}` |
| `0x07` | PROGRESS | u8 pct (0–100) | the pet draws the host's overall % |
| `0x08` | PLAY | u8 len, name | play `sounds/<name>.ima`; `{"ota":"play","name":…,"bytes":N}` |
| `0x0A` | SELFTEST | — | 1 kHz beep; with a mic: `{"ota":"selftest","mic":true,"heard":…,"db":…,"quiet":…,"tone":…}` |
| `0x0B` | BEGIN_STREAM | u32 size, u32 crc32 | a PSND clip played while it arrives; `{"ota":"streamed","bytes":N,"crc":true}` |
| `0x0C` | LISTEN | u16 ms (100–10000), u8 flags | record the mic for `ms`; reply `{"ota":"heard",…}` (§4.6); flags bit 0 = also send the audio on the mic characteristic |
| `0x0D` | WIFI_CONFIG | u8 len, SSID · u8 len, password · u8 len, server URL | WiFi for the voice stream (VOICE.md); `{"ota":"wifi","state":…}`. An empty URL means find the server by mDNS |
| `0x0E` | VOICE | u8 mode (0 off, 1 stream speech) | `{"ota":"voice","mode":…,"state":…}`; `{"ota":"error","why":"no mic"}` on a board without one |

crc32 is zlib CRC-32 (poly `0xEDB88320`, init and final XOR `0xFFFFFFFF`) over the whole payload.

Paths for BEGIN_FILE (lower-case `a-z 0-9 - _ .`, no `..`):
- `characters/<pack>/<file>` (exactly two slashes): animation packs (manifest.json + GIFs)
- `apps/<name>.wasm`: pet apps (name ≤ 23 chars)
- `sounds/<name>.ima`: PSND clips (name ≤ 23 chars)

### 4.3 Notifications on control

One JSON object per notification:

| `ota` | fields | meaning |
|---|---|---|
| `ready` | `size`, `window` | the transfer is open: send at most `window` bytes beyond the last ack |
| `ack` | `off` | `off` bytes have left the link buffer so far (repeated every ~0.5 s while idle) |
| `done` | — | firmware accepted; the pet reboots in ~1 s |
| `saved` | `path` | a file transfer landed |
| `pack` / `app` / `play` | `name` | as in §4.2 |
| `streamed` | `bytes`, `crc` | a stream finished |
| `selftest` | `mic`, `heard`, `db`, `quiet`, `tone` | speaker self-test result |
| `heard` | see §4.6 | a LISTEN finished |
| `error` | `why` | the job stopped; the pet keeps what it had |

### 4.4 Transfers (BEGIN / BEGIN_FILE / BEGIN_STREAM)

1. Write the BEGIN command. Wait for `ready` (fail after ~20 s of no progress).
2. Write data to `6e400012` (write-without-response) in chunks ≤ the max write length, in order,
   never more than `window` bytes beyond the last `ack.off`.
3. On each `ack`, advance and keep sending. When `ack.off == size`, write `0x02` END.
4. Wait for the outcome: `done` (BEGIN), `saved` (BEGIN_FILE), `streamed` (BEGIN_STREAM).

The window is the only flow control: write-without-response has none of its own. A stream is
throttled to the playback rate by the pet withholding acks while its audio buffer is full.

### 4.5 Firmware images

An app image begins with `0xE9`. At offset 32 (image header 24 + first segment header 8) is the
`esp_app_desc_t`: u32 magic `0xABCD5432`, then at +16 `version[32]`, +48 `project_name[32]`,
+80 `time[16]`, +96 `date[16]`, +144 `app_elf_sha256[32]` (NUL-padded strings). A host checks
the magic, that `project_name` is a pet project (`jc3248_pet_idf`, `oraclepet_lcd28`), and that it
matches the pet's `project` before sending.

### 4.6 LISTEN (the pet's microphone)

`0x0C`, u16 ms, u8 flags. Boards with a mic (lcd28) record `ms` milliseconds at 16 kHz and reply:

```json
{"ota":"heard","ms":3000,"samples":48000,"rms":412,"peak":9120,"freq":1000,"db":23.4,"floor":31,"sent":true}
```

- `rms`, `peak`: of the recording (int16 full scale 32767).
- `freq`: the strongest frequency between 150 and 4000 Hz (Goertzel bins every 50 Hz).
- `db`: that bin's power over the median bin (how clearly a tone stands out).
- `floor`: the RMS of the quietest 100 ms.

With flags bit 0 set (and the host subscribed to the mic characteristic `6e400013`), the audio
itself follows on that characteristic as a PSND clip (§5): the 16-byte header (sample count =
`samples`) then the 256-byte blocks, as one byte stream split into notifications of at most
MTU − 3 bytes. **Concatenate the notifications in order**; do not assume one block per
notification. The `heard` reply on control comes after the last audio byte and says
`"sent":true` if the whole stream went out. Boards without a mic reply `{"ota":"error","why":"no mic"}`. Every firmware has the mic
characteristic, so it does not tell whether a mic is there: the board does. Today only
`oraclepet_lcd28` has one.

## 5. PSND sound clips (IMA ADPCM)

```
header (16 bytes): "PSND", u8 version = 1, u8 codec = 1 (IMA ADPCM), u16 0,
                   u32 sample rate (16000), u32 sample count
block (256 bytes): i16 predictor, u8 step index (0..88), u8 0,
                   252 bytes = 504 samples, 4 bits each, LOW nibble first
```

The last block may hold fewer samples (the header's count says how many). Every block restarts
the decoder from its own header, so a bad block cannot spoil the rest.

Decoder step (also used by the encoder to track its prediction, which makes them bit-exact):

```
step = STEP[index]; diff = step >> 3
if nib & 1: diff += step >> 2
if nib & 2: diff += step >> 1
if nib & 4: diff += step
pred = pred - diff if nib & 8 else pred + diff;  clamp pred to [-32768, 32767]
index += INDEX[nib];                              clamp index to [0, 88]
```

Encoder, per sample `s` (state `pred`, `index`, starting at 0, 0 and carried across blocks; each
block header stores the state *before* its first sample):

```
diff = s - pred; nib = 0; if diff < 0: nib = 8; diff = -diff
t = STEP[index]; if diff >= t: nib |= 4; diff -= t
t >>= 1;         if diff >= t: nib |= 2; diff -= t
t >>= 1;         if diff >= t: nib |= 1
apply the decoder step with nib
```

`STEP` (89 entries) and `INDEX` (16 entries) are the standard IMA tables, listed in
`golden/vectors.json` and in every implementation.

Hosts prepare speech as 16 kHz mono int16, trim quiet ends (|s| ≤ 300), keep 20 ms around the
speech, normalise the peak to 0.9 full scale, and cap the length (6 s for a clip stored with
BEGIN_FILE; a stream may run up to 30 s, the pet's limit for one PSND clip).

## 6. Speech

The pet has no speech synthesis. A host turns text into a PSND clip (macOS `AVSpeechSynthesizer`
or `say`; Linux `espeak-ng`; Windows SAPI) and sends it as a stream (`0x0B`) so the pet starts
talking within ~0.15 s. Use a Thai voice when the text contains Thai (U+0E00–U+0E7F).

## 7. Versioning

This is protocol v1. A new command takes a new byte; existing bytes keep their meaning. The pet
answers unknown commands with ATT error "request not supported".
