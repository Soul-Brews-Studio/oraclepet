# OraclePet host library

One Bluetooth LE protocol for the oracle desk pet (ESP32-S3: the JC3248 3.5in board and the 2.8in
ILI9341 board), four implementations of it. [`PROTOCOL.md`](PROTOCOL.md) is the byte-exact spec;
[`golden/vectors.json`](golden/vectors.json) pins every layout with test vectors that the Swift
reference writes itself.

| | where | what it is |
|---|---|---|
| **Swift** (reference) | `../desk-pet-mac/Sources/PetProtocol`, `PetCore/PetLink.swift` | the OraclePet Mac app: the protocol module the vectors come from, and the transfer engine everything else copies |
| **C core** | [`core/`](core/README.md) | the protocol's bytes in C99: no libc, no malloc, no I/O, no clock. Builds natively, for MCUs, and to a 38 KB wasm module with no imports |
| **Python** | [`python/`](python/README.md) | a package and a CLI (`oraclepet`) over bleak: macOS, Linux, Windows |
| **Web** | [`web/`](web/README.md) | one ES module and a page over Web Bluetooth (Chrome, Edge, Android), plus a simulated pet |

The firmware side is `../jc3248-pet-idf/main/` (`ota.c`, `audio.c`, `ble.c`). Every test here is
offline: none scans, connects or flashes a pet.

## What each one does

| feature | Swift | C core | Python | Web |
|---|:-:|:-:|:-:|:-:|
| CRC-32, every command (§4.2), path and name rules | yes | yes | yes | yes |
| status, firmware info and event parsing (§3.2, §4.1, §4.3) | yes | yes (Swift's JSON rules) | yes | yes |
| firmware image check: magic, pet project, same board, same build | yes | yes (host calls it) | yes | yes |
| send window (§4.4) | yes | yes | yes | yes |
| transfer engine: ready, window, acks, END, outcome, queue, PROGRESS | yes | no (host drives it with the window) | yes | yes |
| 20 s stall watchdog with ABORT, 60 s reboot wait, rollback check | yes | no (no clock) | yes | yes |
| PSND encode / decode (§5) | yes | yes, also streaming | yes | yes |
| LISTEN audio reassembly (§4.6) | yes | yes, streaming (`opet_mic_*`) | yes | yes |
| heartbeat line, byte-exact clipping by character (§3.1) | yes | yes | yes | yes |
| speech prep: `tidy`, WAV, Thai detection (§5, §6) | yes | yes | yes | yes |
| speech synthesis | macOS (AVSpeech) | no | macOS `say`, Linux espeak-ng, Windows SAPI | no (browsers can't render speech to samples) |
| Thai pronunciation of English names (`ThaiSpeech`), translation | yes | no | no | no |
| fleet tally for the heartbeat (`FleetTally`) | yes | no | no | no |
| Bluetooth transport | CoreBluetooth | none (the host's) | bleak | Web Bluetooth |
| CLI / UI | Mac app | none | `oraclepet` CLI | `index.html` |

## Tests

```bash
make test            # all three suites below; every one must pass
```

| suite | command | what it checks |
|---|---|---|
| C core | `make -C core test wasm wasm-test` | every golden vector, Swift-answered edge cases, the firmware's own path check and block decoder, whole transfers through a simulated pet; then the same vectors through the wasm build (needs zig and node) |
| Python | `cd python && .venv/bin/python -m unittest discover -s tests` | every golden vector, Swift parity cases, a simulated pet built from `ota.c`, adversarial acks, grapheme clipping, speech plumbing. Stdlib only: `python3` works without the venv |
| Web | `cd web && node --test` | every golden vector, the engine against a simulated pet and a scripted one, LISTEN reassembly, a fake `navigator.bluetooth`. Run it from inside `web/` |

### The golden-vector rule

`golden/vectors.json` is written by the Swift reference, never by hand:

```bash
tools/gen_vectors.sh     # or: make vectors   (macOS + swift; builds OraclePetVectors in ../desk-pet-mac)
```

Every suite reproduces every section exactly: crc32, every command's hex, the heartbeat line,
PSND `clip_hex` and the decoded samples, the image fields, status lines, UUIDs. When the protocol
changes, change Swift first, regenerate, and make all three suites pass again. A port that cannot
reproduce a vector is wrong, not the vector.

## Cross-language checks (2026-10-21)

Behaviour the vectors do not pin, compared on the same inputs across Swift, C, Python and Web:

| behaviour | inputs | result |
|---|---|---|
| `tidy()` (trim, 20 ms pad, 6 s cap, 0.9 peak, round half away from zero) | 401 clips, incl. odd peaks and a clip over the cap | all four bit-exact (0 LSB) |
| PSND decode of LISTEN audio, whole and cut short | 12 lengths × 10-15 cut points; C fed in random 1-509 byte pieces | all four identical |
| heartbeat clipping (`msg` 63 bytes, `who` 20 characters) | 6,000 random lines: Thai, Devanagari, Arabic prepend, Hangul, emoji ZWJ, flags, controls | C and Web identical to Swift; Python differed on 429 until fixed (below) |
| Thai voice detection (U+0E00-U+0E7F) | boundaries, mixed text | all four identical |
| command bytes at the edges | names 0/23/24/255 bytes, Thai, PROGRESS -5..1000, LISTEN -1..70000 | C, Python, Web identical |
| send window | read side by side | the same `next`/`ack` rule in all four (`acked = max(acked, min(off, sent))`) |
| firmware check before OTA | read side by side | all refuse a non-image, a non-pet project, and the other board's project, in the Mac app's words |

Fixed here: Python's grapheme breaker approximated UAX #29 and cut Indic conjuncts and Arabic
prepend marks (and joined letters across ZWJ) differently from Swift. It now runs the C core's
breaker over the same Swift-derived table (`python/oraclepet/_gcb.py`, from `core/oraclepet.c`
by `python/tools/gen_gcb.py`); 6,000 of 6,000 lines match Swift.

## Where the ports differ on purpose

- **Write size.** Swift and Python use the negotiated write length (Python caps it at 512 and
  waits for CoreBluetooth's flow control); Web Bluetooth does not expose the MTU, so the web
  library writes 180 bytes. The C core takes `max_len` from the host.
- **Empty files.** The pet sends no ack for a 0-byte transfer. Python, Web and the C window send
  END straight after `ready`; the Mac app waits for an ack and stalls.
- **Voice language.** The Mac app speaks Thai by default (a setting picks English) and rewrites
  English names into Thai script first. Python follows PROTOCOL.md §6: Thai only when the text
  holds Thai, English otherwise (`--lang` overrides).
- **Grapheme tables.** Web uses the JS engine's `Intl.Segmenter`; it matched Swift on all 6,000
  lines under Node 26 (ICU 78, Unicode 17). An older browser may break rare scripts differently.
- **Status JSON.** The C core accepts exactly what Swift's `JSONSerialization` accepts (trailing
  commas, BOM, first duplicate key wins). Python and Web use their standard parsers. The firmware
  never sends those shapes, so it does not matter on the wire.

## Known protocol limits (all implementations)

- `ota.c` sends the final ack (`off == size`) once. If that notification is lost, no host sends
  END and the job fails after the pet's timeout.
- After a cancel, a late `{"ota":"error","why":"aborted"}` can fail a job started right away.
- `ble.c` keeps `who` in 24 bytes, so a 20-character Thai name is cut mid-character on the pet.
