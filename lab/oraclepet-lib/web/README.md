# OraclePet in the browser

Drive a desk pet over Web Bluetooth from a web page: firmware updates, animation packs, pet
apps, the speaker test, the microphone, heartbeats. No app to install, no build step, no
dependencies. The bytes follow [`../PROTOCOL.md`](../PROTOCOL.md), and the tests reproduce
[`../golden/vectors.json`](../golden/vectors.json) exactly.

| file | what |
|---|---|
| `oraclepet.js` | the library: one ES module. The protocol half (crc32, commands, PSND clips, image info, heartbeat lines, status parsing) imports anywhere, Node included; `Pet` drives a pet over `navigator.bluetooth`. |
| `index.html` | the page: everything below, wired to `oraclepet.js`. |
| `simpet.js` | a pet in memory that answers like the firmware (`ota.c`, `audio.c`, `ble.c`), for the tests and for the page's **Try a simulated pet** button. |
| `test/` | `node --test`: golden vectors, protocol edge cases, the transfer engine against the simulated pet. |

## Run the page

Web Bluetooth only works on `https://` or on `localhost`/`127.0.0.1`. Serve this folder locally:

```bash
cd lab/oraclepet-lib/web
python3 -m http.server 8000 -b 127.0.0.1
# open http://127.0.0.1:8000/ in Chrome or Edge
```

Press **Connect a pet** and pick a `Claude-XXXX` in the browser's list. A pet takes one host at
a time: quit OraclePet on the Mac (or anything else holding it) first, or the pet will not show up.
**Try a simulated pet** runs the whole page without Bluetooth.

## Browser support

| | Web Bluetooth |
|---|---|
| Chrome, Edge on macOS, Windows 10+, ChromeOS | yes |
| Chrome on Android 6+ | yes |
| Chrome, Edge on Linux | yes with BlueZ 5.41+; some builds need `chrome://flags/#enable-experimental-web-platform-features` |
| Opera, Samsung Internet | yes (Chromium) |
| Brave | off by default (`brave://flags/#brave-web-bluetooth-api`) |
| Firefox, Safari, any browser on iOS | no (the page says so; the simulated pet still works) |

## What works

- **Connect**: the chooser lists only `Claude-*` devices. The page reads what the pet runs (board,
  firmware version and build time, slot, valid/pending, packs, apps, free storage) and reconnects
  by itself for a minute when the link drops.
- **Firmware** (full OTA): pick a `.bin`. The page shows the image's project, version, build time
  and SHA, and refuses the other board's firmware before sending a byte. The job ends when the pet
  is back on the other slot, or reports that it rolled back.
- **Animation pack** (partial OTA): pick a pack folder (manifest.json + GIFs), or a folder of packs
  such as `lab/buddy/characters`, and send one. Names the pet would refuse are flagged first;
  manifest.json goes last, then the pet shows the pack.
- **Pet app**: pick a `.wasm` (built with `lab/pet-sdk`), send it, and run it if you like.
  The packs and apps on the pet are buttons: show a pack, run or stop an app.
- **Sound**: **Test speaker** (a 1 kHz beep; the lcd28 board hears itself through its mic).
  **Listen** records 0.1 to 10 s from the pet's mic, shows the level, peak, strongest frequency and
  noise floor, plays the recording in the browser (WebAudio), saves it as `.wav`, or plays it back
  on the pet. **Play a sound file on the pet** decodes any audio file the browser can play,
  resamples it to 16 kHz and streams it to the speaker.
- **Heartbeat**: set the pet's state (idle, busy, needs you, asleep), status line and name by hand,
  once or every 5 s. It also sets the pet's clock.
- **Progress**: one bar for the whole batch with the overall %, the job and file, KB/s, and
  **Cancel**. The pet draws the same % on its screen for packs and apps.

**Speech is not there.** The pet has no voice of its own: a host turns text into a clip. A browser
can say text with `speechSynthesis` but cannot capture that audio, so there is no portable way to
make the pet talk from a web page. OraclePet on the Mac does it (`AVSpeechSynthesizer`), as can a
host with espeak-ng (Linux) or SAPI (Windows). When an app on the pet calls `pet.say()`, the page
logs the line and can speak it on the computer instead (a checkbox).

## Use the library

```js
import { Pet, encodePSND, tidy } from './oraclepet.js';

const pet = await Pet.request();          // in a click handler: the browser shows its chooser
pet.on('progress', (p) => console.log(Math.floor(p.overall * 100), p.phase));
pet.on('log', console.log);
await pet.connect();                      // reads pet.info

await pet.ota(await file.arrayBuffer());  // resolves once the pet is back on the new image
await pet.pack(folderInput.files);        // the FileList of <input type="file" webkitdirectory>
await pet.app(wasmFile, true);            // send and run
await pet.select('siamese'); await pet.run('hello'); await pet.stop(); await pet.play('say');
await pet.stream(encodePSND(tidy(pcm16k)));            // a clip played while it arrives
const t = await pet.selftest();           // { mic, heard, db, quiet, tone, message }
const r = await pet.listen(3000, true);   // { rms, peak, freq, db, floor, pcm: Int16Array, … }
await pet.heartbeat({ total: 1, running: 1, waiting: 0, msg: '1 working', who: 'neo' });
```

Each job returns a promise of `{ message, … }` and rejects with a `PetError` (`.why` holds the pet's
own words). Jobs run one at a time in the order asked; a request made while one runs waits its
turn and shares the batch's overall %. The transfer engine is the Mac app's (`PetLink.swift`):
BEGIN, wait for `ready`, at most `window` bytes beyond the last ack, END once every byte is
acked, then the outcome. No word from the pet for 20 s aborts the job (ABORT) and drops the queue.

Web Bluetooth does not expose the negotiated MTU, so writes are 180 bytes (fits the ATT MTU of
185 that Apple platforms negotiate). `Pet.request({ chunkSize: 244 })` sends bigger ones where the
link allows. Every GATT operation goes through one queue, because Chrome refuses a second one
while another is in flight.

## Tests

```bash
cd lab/oraclepet-lib/web
node --test        # Node 22+; 219 tests, about 2 s
```

- `test/vectors.test.mjs`: every vector in `golden/vectors.json` (crc32, all 13 commands, the
  heartbeat line, both PSND clips encoded and decoded, the image fields, the status lines, UUIDs,
  IMA tables).
- `test/protocol.test.mjs`: limits and edge cases, and the rules the Swift tests pin.
- `test/transfer.test.mjs`: the engine against `simpet.js`: window and chunk limits, pack/app/OTA
  outcomes, reboot and rollback, streams throttled to playback, the queue, stalls, a dropped link,
  cancel, the speaker test, LISTEN (audio before or after the reply, or a lost reply), heartbeats
  and events. Nothing touches Bluetooth.
- `test/verify.test.mjs`: an independent second reading. A PSND reference written from
  PROTOCOL.md 5 (0, 1, 503, 504, 505, 1008, 1009 samples; block headers carry the encoder
  state; low nibble first; zero padding), heartbeat escaping and clipping (control characters,
  Thai, emoji, grapheme boundaries), name and path limits from `ota.c` (23-character names,
  95-byte paths), a scripted pet that acks by hand (window, idle and duplicate re-acks, END
  only at `ack.off == size` and only once, error lines, stalls), LISTEN audio cut into 1-, 7-,
  20- and 182-byte notifications, and `WebBluetoothLink` driven through a fake
  `navigator.bluetooth` (data without response, commands with response).
