# oraclepet core (C99)

The byte-exact half of the OraclePet protocol in one portable C file: the CRC, every control
command, the heartbeat line, PSND sound clips (IMA ADPCM) both ways, firmware image checks,
the OTA send window, and a reader for the pet's status lines. Bluetooth itself is not here:
a host owns the link and asks this code which bytes to write and what the pet's replies mean.

Why C: the same source runs on a Mac or Linux box, in firmware (ESP-IDF, Arduino), and as
wasm in a browser. It needs nothing, not even libc, allocates nothing and keeps no global
state. The spec is [`../PROTOCOL.md`](../PROTOCOL.md); the tests reproduce
[`../golden/vectors.json`](../golden/vectors.json) (made by the Swift app the pets already
accept) byte for byte.

| file | what |
|---|---|
| `oraclepet.h`, `oraclepet.c` | the core: copy both into any project |
| `tests/test_core.c` | every vector, firmware rules, a simulated pet (`make test`) |
| `tests/test_wasm.mjs` | the same vectors through the wasm build (`make wasm-test`, node) |
| `tests/edge_vectors.json` | corner cases answered by the Swift reference (see below) |
| `tools/gen_vectors_h.py` | both JSON files -> `tests/vectors.h` (python3, stdlib only) |
| `tools/gen_edge_vectors.sh` | regenerate `edge_vectors.json` and `hb_cases.h` from the Swift targets OraclePetEdgeVectors and OraclePetHeartbeatCases in `lab/desk-pet-mac` (needs Swift) |
| `tests/hb_cases.h` (`tools/gen_edge_vectors.sh`) | heartbeat lines for empty, Thai and multi-code-point text, from Swift |
| `tools/gcb_table.swift`, `tools/gen_gcb_table.sh` | rewrite `gcb_table` in `oraclepet.c` from Swift's grapheme breaking (needs swiftc) |

## Build and test

```bash
make test        # cc -std=c99 -Wall -Wextra -Werror -pedantic; runs every check (offline)
make wasm        # build/oraclepet_core.wasm: zig cc, wasm32 MVP, no imports, 31 KB
make wasm-test   # the vectors again, through the wasm module (node)
make clean
make CC=gcc test # any C99 compiler
```

`tests/vectors.h` is regenerated whenever either JSON file changes. The generator stops on any
key it does not know, so a new kind of vector gets a test instead of being skipped.

What `make test` covers: the golden vectors (CRC, all commands, heartbeat, both PSND clips and
their decoded samples, image fields, status kinds, UUIDs); the Swift edge cases; the
firmware's own code where the firmware decides (`safe_path` from ota.c against 20,000 random
paths, the block decoder from audio.c against 3,000 random blocks); and whole transfers
through a simulated pet that follows ota.c (32 KB stream buffer, 16 KB window, an ack every
4 KB, re-acks while idle, lost acks, a pet without a `window` field, a wrong CRC, a bad path,
another board's image, a host that ignores the window). The `verify:` sections add PSND at
the block edges (0, 1, 503, 504, 505, ... samples) against a second encoder typed in from
PROTOCOL.md, LISTEN streams cut into random notification sizes, heartbeats Swift answered
for empty, Thai, Devanagari, Hangul, ZWJ and prepend text, names at their length limits,
non-UTF-8 status lines, and 2,000 random ack sequences against the window.

## API

All names start with `opet_`. Builders write into `(out, cap)` and return the length, 0 when it
does not fit; where 0 is a real answer the return is `int32_t` and `OPET_ERR` (-1) is failure.

| area | functions |
|---|---|
| CRC-32 (zlib) | `crc32(data, len)`, `crc32_update(crc, data, len)` (start from 0, feed pieces) |
| commands (4.2) | `cmd_begin`, `cmd_begin_file`, `cmd_begin_stream`, `cmd_end`, `cmd_abort`, `cmd_select_pack`, `cmd_run_app`, `cmd_progress`, `cmd_play`, `cmd_selftest`, `cmd_listen`; `path_ok(path)`: would the pet take it |
| PSND (5) | `psnd_encode`, `psnd_decode`, `psnd_decode_block`, `psnd_size`, `psnd_header_write`, `psnd_header_read`; streaming encoder `psnd_enc_init` / `psnd_enc_feed` / `psnd_enc_finish`; `wav_header` for saving the mic |
| LISTEN audio (4.6) | `mic_init`, `mic_feed` (one notification in, decoded samples out), `mic_flush`, `mic_done` |
| speech (5-6) | `pcm_tidy` (trim, 0.9 peak, 6 s cap, in place), `text_is_thai` |
| images (4.5) | `image_inspect` -> project, version, built, sha16; `is_pet_project`, `board_name`, `sha_match` |
| heartbeat (3.1) | `heartbeat_init`, `heartbeat_line`, `pet_state` |
| send window (4.4) | `window_init`, `window_next(max_len, &off)`, `window_ack(off)`, `window_done` |
| status (4.3) | `status_parse` -> kind + off, size, window, bytes, crc, why, name, and the LISTEN / selftest fields; `status_kind_name` |
| other JSON | `json_int`, `json_num`, `json_bool`, `json_str`, `json_list_str`: the control read (`ver`, `project`, `sha`, `packs`), NUS events (`evt`, `text`) |

Constants: the UUIDs (`OPET_UUID_*`), command bytes (`OPET_CMD_*`), `OPET_PSND_*`,
`OPET_OMIT`, `OPET_HEARTBEAT_MAX`, `OPET_IMAGE_MIN`.

## A transfer, from the host's side

```c
/* 1. BEGIN_FILE with the payload's size and CRC (ble_* is your link) */
uint8_t cmd[OPET_CMD_MAX];
ble_write_control(cmd, opet_cmd_begin_file(cmd, sizeof cmd, size, opet_crc32(data, size),
                                           "characters/neo/idle.gif"));

/* 2. every notification on OTA control */
void on_control(const uint8_t *v, size_t len)
{
    opet_status st;
    switch (opet_status_parse(&st, (const char *)v, len)) {
    case OPET_ST_READY: opet_window_init(&w, st.size, st.window); ready = 1; break;
    case OPET_ST_ACK:   opet_window_ack(&w, st.off); break;
    case OPET_ST_SAVED: /* landed; st.name is the path */ return;
    case OPET_ST_ERROR: /* st.why; the pet keeps what it had */ return;
    default: return;
    }
    if (ready && !ended && opet_window_done(&w)) {      /* every byte acked (or 0 bytes) */
        ble_write_control(cmd, opet_cmd_end(cmd, sizeof cmd));
        ended = 1;
    }
    pump();
}

/* 3. data, write-without-response, as far as the window allows (call it again whenever
 *    the link can take more) */
void pump(void)
{
    uint32_t off, n;
    while (ready && (n = opet_window_next(&w, max_write_len, &off)) != 0) ble_write_data(data + off, n);
}
```

Speech: tidy the synthesized 16 kHz samples, `opet_psnd_encode` them, send with
`opet_cmd_begin_stream` the same way; the pet answers `streamed`. LISTEN audio arrives on the mic
characteristic as one byte stream (the 16-byte header, then 256-byte blocks) cut into
notifications of up to MTU - 3 bytes, so a block often spans two notifications and one
notification can end one block and start the next. Feed every notification, in order, to
`opet_mic_feed` (after `opet_mic_init` when you send LISTEN); it returns the samples of each
block it completes. When `heard` arrives, `opet_mic_flush` gives what a cut-short stream still
holds, and `opet_mic_done` says whether every announced sample came. (Or concatenate the
notifications and call `opet_psnd_decode` once, as PetLink.swift does.)

## wasm

`make wasm` builds a plain wasm 1.0 module (`-mcpu=mvp`, so any engine loads it, the pet's
WAMR included) with no imports and every function above exported, plus `memory` and
`__heap_base`. A JS host puts its inputs into linear memory from `__heap_base` up (growing
memory as needed) and passes pointers; `tests/test_wasm.mjs` is a worked example. Structs
have the same layout on every target, except `opet_heartbeat`, which holds two pointers:

| struct | bytes | fields (offset) |
|---|---|---|
| `opet_window` | 16 | total 0, sent 4, acked 8, window 12 |
| `opet_status` | 264 | kind 0, off 4, size 8, window 12, bytes 16, crc 20, ms 24, samples 28, rms 32, peak 36, freq 40, floor 44, sent 48, mic 52, heard 56, quiet 60, tone 64, db 68 (float), why 72, name 168 |
| `opet_image_info` | 117 | project 0, version 33, built 66, sha16 100 |
| `opet_psnd_enc` | 268 | opaque |
| `opet_mic_rx` | 272 | total 0, done 4, fill 8, state 12, the rest opaque |
| `opet_heartbeat` (wasm32) | 48 | epoch 0 (i64), total 8, running 12, waiting 16, tzoff 20, pct5h 24, reset5h 28, pct7d 32, reset7d 36, msg 40, who 44 |

`oraclepet.c` checks these layouts when it compiles.

## Edge cases and what they pin

`golden/vectors.json` is the contract. `tests/edge_vectors.json` adds corners it does not
reach, answered by the Swift reference itself (`tools/gen_edge_vectors.sh` compiles
`desk-pet-mac/Sources/PetProtocol` by its OraclePetEdgeVectors target), so these too are Swift's
behaviour, not a reading of it:

- **Heartbeat clipping**: Swift clips `msg` to 63 bytes and `who` to 20 characters by
  *grapheme cluster*. The core implements every UAX #29 rule (CR LF, controls, Hangul
  syllables, marks, prepend, Indic conjuncts, emoji ZWJ sequences, flags) over `gcb_table`, a
  5 KB table of code point classes read off Swift's own `String` breaking by
  `tools/gen_gcb_table.sh`, so it agrees with the reference for any script. Only a C string's
  NUL cannot be sent: `msg` and `who` end there (Swift would escape it as `\u0000`).
- **Escaping**: `\"`, `\\`, `\n`, `\t`, `\u00xx` for other controls (`\r` is `\u000d`), DEL and
  everything else as is.
- **Status lines**: a line counts only if Swift's `JSONSerialization` would read it. A cut
  notification (`{"ota":"ack","off":40`) is nothing, not an ack at 40, and so is a line whose
  strings are not well-formed UTF-8 (overlongs, surrogates, stray bytes). Like Swift: a leading
  BOM and trailing commas are fine, the first of two equal keys wins, `4096.0` and `1e3` are
  integers, `0`/`1` read as booleans, `ack` without `off` is nothing, `window` defaults to
  4096 and `why` to `"unknown error"`.
- **Clamping and limits**: `progress` to 0..100, `listen` to 100..10000 ms; names longer
  than 255 bytes are refused (Swift traps); a clip cut short decodes to what it holds.

## Notes for hosts

- A **0-byte transfer** is done right after `ready`: the pet acks nothing and waits for END
  (ota.c's loop never runs). Check `opet_window_done` after `ready`, not only after acks.
- The pet re-acks every 0.5 s while bytes are missing, so a lost ack only costs time. It
  never repeats the **final** ack: if that one is lost, a file or firmware transfer ends with
  `no END from the host` after ~10 s (a stream reports `streamed` after ~5 s anyway).
- The core has no clock: the host gives up when nothing moves for ~20 s (no `ready`, no new
  ack), as PetLink.swift does, and sends ABORT. Commands go to OTA control as writes *with*
  response, data to OTA data *without* response.
- Status fields are `int32_t`: offsets and sizes up to 2 GB (larger values clamp).
- Strings come back NUL-terminated, so an escaped `\u0000` inside one ends it there as a C
  string (`opet_json_str` still returns the full length).
- `opet_heartbeat_line` returns 0 when the line does not fit; `OPET_HEARTBEAT_MAX` (1 KB) is
  the pet's own line buffer (ble.c `RX_CAP`).
