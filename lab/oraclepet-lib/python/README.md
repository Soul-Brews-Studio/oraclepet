# oraclepet (Python)

Drive the OraclePet desk pet over Bluetooth LE from macOS, Linux or Windows: firmware updates,
animation packs, pet apps, speech, the mic, heartbeats. It sends the same bytes as the OraclePet
Mac app and runs transfers the same way (window, acks, END, outcomes, 20 s stall). The spec is
[`../PROTOCOL.md`](../PROTOCOL.md); the tests reproduce [`../golden/vectors.json`](../golden/vectors.json)
byte for byte.

Python 3.10+, one dependency: [bleak](https://github.com/hbldh/bleak) (>= 0.22).

## Install

```bash
cd lab/oraclepet-lib/python
uv venv && uv pip install -e .          # or: python3 -m venv .venv && .venv/bin/pip install -e .
.venv/bin/oraclepet --help              # or: .venv/bin/python -m oraclepet --help
```

**One host at a time.** A pet takes one central, and a connected pet stops advertising. Disconnect
it in the OraclePet app (or stop `fleet_bridge.py` / `usage_bridge.py`) before using this.

### macOS

- **Bluetooth permission**: the terminal app you run it from (Terminal, iTerm, Ghostty, VS Code, …)
  must be allowed in **System Settings > Privacy & Security > Bluetooth**. Without it the scan finds
  nothing or bleak reports Bluetooth as unauthorized. Restart the terminal after allowing it.
- Addresses are CoreBluetooth UUIDs, not MACs. `--pet D350` (the name suffix) is simpler.
- Speech: `say`; Thai text gets the Kanya voice.

### Linux

- BlueZ with `bluetoothd` running, and a user allowed to use it (usually the `bluetooth` group).
  BlueZ 5.62+ reports the MTU; older BlueZ writes 20 bytes at a time (slower, still correct).
- Speech: `sudo apt install espeak-ng`. Thai text asks espeak-ng for its `th` voice and falls back
  to the default voice; macOS and Windows sound better for Thai.

### Windows

- Windows 10 (1709+) or 11 with Bluetooth LE; Python from python.org.
- Speech: Windows PowerShell + System.Speech (SAPI). Thai needs a Thai voice installed
  (Settings > Time & language > Speech), otherwise the default voice reads it.

## Command line

Every command takes `--pet SUFFIX` (`D350` for `Claude-D350`, or an address; `$ORACLEPET_PET` works
too). Without it, the first pet found is used.

```bash
oraclepet scan                                          # pets advertising, strongest first
oraclepet info --pet D350                               # board, firmware, slot, packs, apps (--json: raw)
oraclepet ota ../../jc3248-pet-idf/build-lcd28/oraclepet_lcd28.bin --pet D350
oraclepet pack ../../buddy/characters/neo --pet D350    # copy a pack (manifest.json + GIFs), show it
oraclepet app ../../pet-sdk/build/talker.wasm           # copy an app and run it (--no-run: copy only)
oraclepet run talker
oraclepet stop
oraclepet select siamese
oraclepet play say                                      # sounds/say.ima on the pet
oraclepet say "hello from the terminal"
oraclepet say "สวัสดีครับ"                                # Thai text: a Thai voice (--lang th|en to choose)
oraclepet selftest                                      # beep; a pet with a mic says if it heard it
oraclepet listen --ms 3000 --wav heard.wav              # the mic: levels, and the recording (lcd28)
oraclepet heartbeat --state attention --msg "wait: neo" --who neo
oraclepet heartbeat --dry-run                           # print the exact line, no Bluetooth
```

`ota` refuses an image for the other board (the pet's `project` must match the image's) and skips
a build the pet already runs (`--force` sends it anyway); after the reboot it checks the pet came
back on the other slot. `-v` logs every command and status line. Exit status: 0 done, 1 failed (the
message says why, in the Mac app's words), 130 interrupted.

## Library

```python
import asyncio
from oraclepet import Pet, PetError

async def main():
    async with Pet("D350") as pet:              # Pet() = the first pet; Pet("AA:BB:...") = by address
        fw = pet.firmware                       # read on connect
        print(fw.board, fw.version, fw.slot, fw.packs, fw.apps)
        pet.on_progress = lambda p: print(f"{p.overall:.0%}", end="\r")
        print(await pet.pack("../../buddy/characters/neo"))
        print(await pet.say("hello"))
        levels, samples = await pet.listen(3000)    # samples: 16 kHz int16, as a list
        await pet.heartbeat(total=1, running=1, waiting=0, msg="busy")

asyncio.run(main())
```

Jobs: `ota(image)`, `pack(folder)`, `app(wasm, run=True)`, `run(name)`, `stop_app()`,
`select_pack(name)`, `play(name)`, `stream(clip)`, `say(text, language=None)`. Each returns an
`Outcome` (`.message` is the Mac app's line) or raises `PetError`. A job asked for while another
runs waits its turn; `cancel()` drops the queue and aborts the running one. Also: `info()`,
`selftest()`, `listen(ms, send_audio=True)`, `heartbeat(...)`, `on_event(callback)` for
`{"evt":"tap"}` / `{"evt":"say"}`, and `Pet(speak_events=True)` to speak what wasm apps `pet.say()`.

The wire format alone, no Bluetooth:

```python
from oraclepet import protocol as P

P.begin_file(len(data), P.crc32(data), "characters/neo/idle.gif")   # command bytes
clip = P.psnd_encode(P.tidy(pcm)); P.psnd_decode(clip)               # PSND IMA ADPCM
P.Heartbeat(3, 1, 2, msg="wait: neo", who="neo").json_line()         # the exact NUS line
P.describe(open("oraclepet_lcd28.bin", "rb").read())                 # version, built, sha, project
P.parse_status(b'{"ota":"ack","off":4096}')                          # {"kind": "ack", "off": 4096}
```

## How a transfer runs

The same steps as `PetLink.swift` in the Mac app:

1. BEGIN / BEGIN_FILE / BEGIN_STREAM with the size and CRC-32 (write with response).
2. Wait for `ready`, then write chunks of the negotiated size (MTU - 3) without response, never
   more than `window` bytes beyond the last `ack`.
3. On each `ack`: advance, send PROGRESS with the overall % for packs, apps and streams (one bar
   across queued jobs), and once every byte is acked, END.
4. The outcome: `saved` (next file, then SELECT_PACK / RUN_APP / PLAY), `streamed`, or `done`: the
   pet reboots, and the library waits for the link to drop, finds the pet again and checks the slot.
5. No status from the pet for 20 s: ABORT, the job fails and the queued jobs are dropped. An
   `error` notification fails the job with the pet's reason.

Where it differs, on purpose:

- macOS: CoreBluetooth drops a write-without-response when its queue is full, and bleak does not
  wait, so `BleLink` waits for `canSendWriteWithoutResponse` before each chunk, as the Mac app does.
- Chunks are capped at 512 bytes (the pet copies a write into a 520-byte buffer).
- An empty file gets END right after `ready` (no ack will ever come for zero bytes).
- `listen()` stops waiting 3 s after the audio when no `heard` line arrives; the audio is kept
  (`PetError.samples`; `listen --wav` saves it).
- Pack, file and app names are checked against the pet's rules before anything is sent.

## Tests

```bash
.venv/bin/python -m unittest discover -s tests -v
```

- `tests/test_vectors.py`: every section of `golden/vectors.json` (CRC-32, every command, the
  heartbeat line, PSND bytes and decoded samples, image fields, status lines, UUIDs), plus the
  protocol cases of the Swift `PetProtocolTests`.
- `tests/test_transfer.py`: `SimPet`, the pet side of `ota.c` in-process. Full transfers, the
  window never exceeded, stalls, error notifications, CRC mismatch, link loss, cancel, queued jobs
  sharing one bar, the reboot and rollback, selftest, listen, heartbeat chunking, events; and
  `BleLink`'s own logic against a stand-in for bleak.
- `tests/test_parity.py`: edge cases answered by the Swift PetProtocol itself
  (`tests/parity_vectors.json`, regenerated with `tests/swift/gen_parity.sh` on a Mac): PSND clips
  of 0, 1, 503, 504, 505, 1008, 1009 samples and damaged clips, `tidy`, heartbeat escaping and
  clipping (Thai, emoji, controls, empty strings), name/path/LISTEN/PROGRESS limits, status defaults.
- `tests/test_adversarial.py`: a pet that sends duplicate, stale, late and lost acks, a `ready` for
  another size, windows smaller than one write, mic audio in odd notification sizes; each run checks
  one END per transfer, no data outside a transfer, and the window.
- `tests/test_tts.py`: the WAV reader and resampler, voice choice, a hung synthesizer (retried,
  then a `SpeechError`), and one live run of this OS's speech when it has one.
- `tests/test_graphemes.py`: heartbeat `msg`/`who` clipping on Arabic prepend marks, Devanagari
  conjuncts, ZWJ, Hangul jamo, flags and Thai, against lines Swift wrote
  (`tests/grapheme_cases.json`, `tests/swift/gen_grapheme_cases.sh`). The breaker uses
  `oraclepet/_gcb.py`, the same Swift-derived table as `core/oraclepet.c`
  (`tools/gen_gcb.py` copies it; the test fails if the two drift).

All offline: no test opens a Bluetooth connection.
