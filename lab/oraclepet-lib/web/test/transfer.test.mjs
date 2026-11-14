// The transfer engine (the Mac app's, PetLink.swift) against simpet.js, an in-memory pet that
// answers like the firmware (ota.c, audio.c, ble.c). No Bluetooth: every byte stays in Node.
import { describe, test } from 'node:test';
import assert from 'node:assert/strict';
import * as op from '../oraclepet.js';
import { SimPet, makeImage } from '../simpet.js';

// Fast clocks for both sides; the logic is the same as at full scale.
const FAST_SIM = { rate: 4_000_000, tickMs: 1, idleAckMs: 30, idleFailMs: 3000, endWaitMs: 1000, streamEndWaitMs: 300, timeScale: 0.01, playRate: 400_000 };
const FAST_PET = {
  stallMs: 600,
  watchdogMs: 20,
  rebootMs: 3000,
  settleMs: 5,
  reconnectMs: 3000,
  reconnectEveryMs: 10,
  infoEveryMs: 0,
  selftestMs: 2000,
  listenGraceMs: 2000,
  micQuietMs: 200,
  heardGraceMs: 100,
};

async function pair(t, sim = {}, pet = {}) {
  const s = new SimPet({ ...FAST_SIM, ...sim });
  const p = new op.Pet(s, { ...FAST_PET, ...pet });
  t.after(() => p.disconnect());
  await p.connect();
  return { sim: s, pet: p };
}

const bytes = (n, seed = 1) => Uint8Array.from({ length: n }, (_, i) => (i * 31 + seed * 7 + (i >> 8)) & 0xff);
const wasm = (n = 64) => Uint8Array.from({ length: n }, (_, i) => [0x00, 0x61, 0x73, 0x6d, 1, 0, 0, 0][i] ?? i & 0xff);
const commands = (sim, op0) => sim.seen.control.filter((c) => c[0] === op0);
const pathOf = (c) => new TextDecoder().decode(c.subarray(10));
const waitFor = async (cond, ms = 2000) => {
  const end = Date.now() + ms;
  while (!cond()) {
    if (Date.now() > end) throw new Error('timed out waiting');
    await new Promise((r) => setTimeout(r, 5));
  }
};

describe('connect', () => {
  test('reads what the pet runs', async (t) => {
    const { pet } = await pair(t);
    assert.equal(pet.info.project, 'oraclepet_lcd28');
    assert.equal(pet.info.board, '2.8in ILI9341');
    assert.deepEqual(pet.info.packs, ['atlas', 'siamese']);
    assert.deepEqual(pet.info.apps, ['hello', 'talker']);
    assert.equal(pet.info.slot, 'ota_0');
  });
});

describe('pack (partial OTA)', () => {
  test('every file lands intact, manifest.json last, then the pet shows the pack', async (t) => {
    const { sim, pet } = await pair(t);
    const files = [
      { name: 'manifest.json', data: bytes(900, 1) },
      { name: 'idle.gif', data: bytes(40000, 2) },
      { name: 'busy.gif', data: bytes(25000, 3) },
    ];
    const r = await pet.pack({ name: 'neo', files });
    assert.match(r.message, /^Pet now shows neo · 64 KB in \d+ s$/);
    for (const f of files) assert.deepEqual(sim.fs.get(`characters/neo/${f.name}`), f.data, f.name);
    const order = commands(sim, op.CMD.BEGIN_FILE).map(pathOf);
    assert.deepEqual(order, ['characters/neo/idle.gif', 'characters/neo/busy.gif', 'characters/neo/manifest.json']);
    assert.equal(sim.pack, 'neo');
    await waitFor(() => pet.info.pack === 'neo'); // the info is read again after "pack"
    assert.deepEqual(pet.info.packs, ['atlas', 'neo', 'siamese']);
  });

  test('the pet draws the host overall %, rising to 100', async (t) => {
    const { sim, pet } = await pair(t);
    await pet.pack({ name: 'neo', files: [{ name: 'idle.gif', data: bytes(50000) }, { name: 'manifest.json', data: bytes(100) }] });
    const pcts = sim.seen.progress;
    assert.ok(pcts.length >= 5, `only ${pcts.length} PROGRESS`);
    assert.deepEqual(pcts, [...pcts].sort((a, b) => a - b));
    assert.equal(pcts.at(-1), 100);
  });

  test('a folder picker FileList works as is', async (t) => {
    const { sim, pet } = await pair(t);
    // what <input type="file" webkitdirectory> hands over: Files with their path in the folder
    const list = [
      { rel: 'cat/manifest.json', data: bytes(50) },
      { rel: 'cat/idle.gif', data: bytes(3000) },
      { rel: 'cat/NOTES.md', data: bytes(10) },
    ].map(({ rel, data }) => Object.defineProperty(new File([data], rel.split('/').pop()), 'webkitRelativePath', { value: rel }));
    const r = await pet.pack(list);
    assert.match(r.message, /Pet now shows cat/);
    assert.deepEqual(sim.fs.get('characters/cat/idle.gif'), bytes(3000));
    assert.equal(sim.fs.has('characters/cat/NOTES.md'), false);
  });

  test('names the pet would refuse are refused here, before a byte goes out', async (t) => {
    const { sim, pet } = await pair(t);
    await assert.rejects(pet.pack({ name: 'neo', files: [{ name: 'manifest.json', data: bytes(9) }, { name: 'Idle 2.gif', data: bytes(9) }] }), /cannot go to the pet/);
    await assert.rejects(pet.pack({ name: 'neo', files: [{ name: 'idle.gif', data: bytes(9) }] }), /no manifest\.json/);
    assert.equal(sim.seen.control.length, 0);
  });

  test('an empty file: END right after "ready" (no ack ever comes for 0 bytes)', async (t) => {
    const { sim, pet } = await pair(t);
    await pet.pack({ name: 'neo', files: [{ name: 'empty.gif', data: new Uint8Array(0) }, { name: 'manifest.json', data: bytes(10) }] });
    assert.equal(sim.fs.get('characters/neo/empty.gif').length, 0);
  });
});

describe('flow control', () => {
  test('never more than `window` bytes beyond the last ack, and no write past the link limit', async (t) => {
    const { sim, pet } = await pair(t, { window: 4096, ackEvery: 1024, rate: 400_000, maxWrite: 100 });
    const data = bytes(60000, 5);
    await pet.pack({ name: 'neo', files: [{ name: 'idle.gif', data }, { name: 'manifest.json', data: bytes(10) }] });
    assert.ok(sim.seen.maxAhead <= 4096, `ran ${sim.seen.maxAhead} bytes ahead`);
    assert.ok(sim.seen.maxAhead > 3000, 'the window was used');
    assert.equal(sim.seen.overflow, false);
    assert.ok(sim.seen.writes.every((n) => n <= 100));
    assert.deepEqual(sim.fs.get('characters/neo/idle.gif'), data);
  });

  test('a host that ignores the window overflows the pet (the sim notices)', async (t) => {
    const sim = new SimPet({ ...FAST_SIM, rate: 1000 });
    t.after(() => sim.disconnect());
    await sim.connect();
    await sim.writeControl(op.cmd.beginFile(40000, 0, 'characters/x/a.gif'));
    await waitFor(() => sim.state === 'receiving');
    for (let at = 0; at < 40000; at += 200) await sim.writeData(new Uint8Array(200));
    assert.equal(sim.seen.overflow, true);
  });
});

describe('full OTA', () => {
  test('refuses the other board, a non-pet image and a non-image before sending a byte', async (t) => {
    const { sim, pet } = await pair(t);
    await assert.rejects(pet.ota(makeImage({ project: 'jc3248_pet_idf' })), { message: 'That firmware is for the JC3248 3.5in board; this pet is 2.8in ILI9341' });
    await assert.rejects(pet.ota(makeImage({ project: 'blink' })), { message: 'Not a pet firmware: blink' });
    await assert.rejects(pet.ota(bytes(5000)), { message: 'Not an ESP32 app image' });
    assert.equal(sim.seen.control.length, 0);
  });

  test('sends, the pet reboots on the other slot, the job ends when it is back', async (t) => {
    const { sim, pet } = await pair(t);
    const phases = new Set();
    pet.on('progress', (p) => phases.add(p.phase));
    const r = await pet.ota(makeImage({ version: 'v2.0.0', size: 60000 }));
    assert.match(r.message, /^Updated in \d+ s: now v2\.0\.0 on ota_1$/);
    assert.equal(pet.info.slot, 'ota_1');
    assert.equal(sim.version, 'v2.0.0');
    for (const ph of ['starting', 'sending', 'finishing', 'rebooting']) assert.ok(phases.has(ph), ph);
    assert.equal(commands(sim, op.CMD.PROGRESS).length, 0); // a lone app image: the pet draws its own bar
  });

  test('a lost "done" still ends well once the pet is back on the new slot', async (t) => {
    const { pet } = await pair(t, { faults: { dropStatus: ['done'] } });
    const r = await pet.ota(makeImage({ version: 'v3', size: 20000 }));
    assert.match(r.message, /now v3 on ota_1/);
  });

  test('a rolled-back image is reported', async (t) => {
    const { pet } = await pair(t, { faults: { rollback: true } });
    await assert.rejects(pet.ota(makeImage({ size: 20000 })), { message: 'Pet came back on ota_0 again: the new image was rolled back' });
  });

  test('a pet that never comes back', async (t) => {
    const { pet } = await pair(t, { faults: { noReturn: true } }, { rebootMs: 200 });
    await assert.rejects(pet.ota(makeImage({ size: 20000 })), { message: 'The pet did not come back after the update' });
  });

  test("the pet's own refusal (it checks the project too)", async (t) => {
    // a pet whose info has no project yet (older firmware): the host cannot tell, the pet can
    const { pet } = await pair(t, { project: 'jc3248_pet_idf' });
    pet.info.project = '';
    await assert.rejects(pet.ota(makeImage({ size: 20000 })), { message: 'Pet refused the update: wrong firmware (oraclepet_lcd28)' });
  });
});

describe('apps and commands', () => {
  test('app: sends the .wasm into apps/ and runs it', async (t) => {
    const { sim, pet } = await pair(t);
    const r = await pet.app({ name: 'Zig-Clock.wasm', data: wasm(3000) });
    assert.match(r.message, /^Pet runs zig-clock · 3000 bytes in \d+ s$/);
    assert.deepEqual(sim.fs.get('apps/zig-clock.wasm'), wasm(3000));
    assert.equal(sim.app, 'zig-clock');
  });

  test('app: send only', async (t) => {
    const { sim, pet } = await pair(t);
    const r = await pet.app({ name: 'later.wasm', data: wasm() }, false);
    assert.match(r.message, /^Sent 0 KB/);
    assert.equal(commands(sim, op.CMD.RUN_APP).length, 0);
  });

  test('app: not WebAssembly, or a name the pet would refuse', async (t) => {
    const { pet } = await pair(t);
    await assert.rejects(pet.app({ name: 'x.wasm', data: bytes(100) }), /not a WebAssembly module/);
    await assert.rejects(pet.app({ name: 'hello world.wasm', data: wasm() }), /must be 1 to 23 characters/);
  });

  test('run, stop, select, play', async (t) => {
    const { sim, pet } = await pair(t);
    assert.equal((await pet.run('talker')).message, 'Pet runs talker');
    assert.equal(sim.app, 'talker');
    assert.equal((await pet.stop()).message, 'App stopped');
    assert.equal(sim.app, '');
    assert.match((await pet.select('siamese')).message, /^Pet now shows siamese/);
    assert.equal((await pet.play('chirp')).message, 'Pet plays chirp'); // answered before its write response
    assert.deepEqual(sim.seen.played, ['chirp']);
  });

  test("the pet's error lines reject, with its words in .why", async (t) => {
    const { pet } = await pair(t);
    await assert.rejects(pet.play('nope'), (e) => e instanceof op.PetError && e.why === 'no sound nope' && /Pet refused the update: no sound nope/.test(e.message));
    await assert.rejects(pet.select('ghost'), { message: 'Pet refused the update: pack ghost did not load' });
    await assert.rejects(pet.run('ghost'), { message: 'Pet refused the update: app ghost: not on the pet' });
  });

  test('a pet already busy says so', async (t) => {
    const { sim, pet } = await pair(t);
    sim.state = 'receiving'; // as if another transfer were running
    await assert.rejects(pet.app({ name: 'x.wasm', data: wasm() }), { message: 'Pet refused the update: busy' });
  });
});

describe('stream (live sound)', () => {
  test('the pet plays the clip while it arrives; CRC checked', async (t) => {
    const { sim, pet } = await pair(t);
    const pcm = op.tidy(sim.recording(2000));
    const clip = op.encodePSND(pcm);
    const r = await pet.stream(clip);
    assert.match(r.message, /^Streamed \d+ KB, sound after [\d.]+ s$/);
    assert.equal(r.crcOK, true);
    assert.deepEqual(sim.seen.streams[0], clip);
    assert.deepEqual(op.decodePSND(sim.seen.streams[0]), op.decodePSND(clip));
  });

  test('PCM in, encoded on the way; a label for the log', async (t) => {
    const { sim, pet } = await pair(t);
    const pcm = Int16Array.from({ length: 8000 }, (_, i) => Math.round(9000 * Math.sin(i / 5)));
    const r = await pet.stream(pcm, { text: 'hello' });
    assert.match(r.message, /^Pet says “hello” · streamed/);
    assert.deepEqual(op.decodePSND(sim.seen.streams[0]), op.decodePSND(op.encodePSND(pcm)));
  });

  test('throttled to playback: acks wait while the speaker buffer is full', async (t) => {
    const { pet } = await pair(t, { playRate: 100_000, audioBuffer: 4096 });
    const clip = op.encodePSND(new Int16Array(16000 * 2).fill(3000)); // 2 s, ~16 KB
    const t0 = Date.now();
    await pet.stream(clip);
    assert.ok(Date.now() - t0 >= (clip.length - 4096) / 100, 'faster than the speaker plays');
  });

  test('not a 16 kHz PSND clip: refused', async (t) => {
    const { pet } = await pair(t);
    await assert.rejects(pet.stream(bytes(100)), /Not a PSND clip/);
    await assert.rejects(pet.stream(op.encodePSND([1, 2, 3], 8000)), /16 kHz/);
  });
});

describe('queue', () => {
  test('a request while busy waits its turn; one overall % for the batch', async (t) => {
    const { sim, pet } = await pair(t);
    const starts = [];
    const overall = [];
    pet.on('job', (j) => j.state === 'start' && starts.push(`${j.label} ${j.job}/${j.jobs}`));
    pet.on('progress', (p) => p.busy && overall.push(p.overall));
    const a = pet.pack({ name: 'neo', files: [{ name: 'idle.gif', data: bytes(60000) }, { name: 'manifest.json', data: bytes(50) }] });
    const b = pet.app({ name: 'tiny.wasm', data: wasm(30000) }); // asked second, so it runs second
    const [ra, rb] = await Promise.all([a, b]);
    assert.match(ra.message, /Pet now shows neo/);
    assert.match(rb.message, /Pet runs tiny/);
    assert.deepEqual(starts, ['pack neo 1/2', 'app tiny 2/2']);
    assert.deepEqual(overall, [...overall].sort((x, y) => x - y), 'the batch % never goes back');
    assert.equal(overall.at(-1), 1);
    const pcts = sim.seen.progress;
    assert.deepEqual(pcts, [...pcts].sort((x, y) => x - y));
    assert.ok(pcts.includes(100));
    const begins = sim.seen.control.filter((c) => c[0] === op.CMD.BEGIN_FILE).map(pathOf);
    assert.equal(begins.at(-1), 'apps/tiny.wasm');
  });

  test('a failed job does not stop the next one', async (t) => {
    const { pet } = await pair(t, { faults: { corrupt: true } });
    const a = pet.pack({ name: 'neo', files: [{ name: 'idle.gif', data: bytes(9000) }, { name: 'manifest.json', data: bytes(50) }] });
    const b = pet.run('hello');
    await assert.rejects(a, { message: 'Pet refused the update: crc mismatch' });
    assert.equal((await b).message, 'Pet runs hello');
  });

  test('a bad request fails at once and leaves the queue alone', async (t) => {
    const { pet } = await pair(t);
    const a = pet.app({ name: 'one.wasm', data: wasm(20000) });
    const b = pet.ota(bytes(10)); // not firmware: fails while a still runs
    await assert.rejects(b, /Not an ESP32 app image/);
    assert.ok(pet.busy);
    assert.match((await a).message, /Pet runs one/);
  });
});

describe('failures', () => {
  test('stall: a pet that goes quiet is aborted after stallMs, the queue dropped', async (t) => {
    const { sim, pet } = await pair(t, { faults: { silentAfter: 8192 } }, { stallMs: 300 });
    const a = pet.pack({ name: 'neo', files: [{ name: 'idle.gif', data: bytes(50000) }, { name: 'manifest.json', data: bytes(50) }] });
    const b = pet.run('hello');
    await assert.rejects(a, { message: 'Update stalled: no progress from the pet for 0 s' });
    await assert.rejects(b, { message: 'Cancelled' });
    assert.equal(sim.seen.aborts, 1);
  });

  test('the link drops mid-transfer: the job fails, the pet keeps what it had, the link comes back', async (t) => {
    const { sim, pet } = await pair(t, { faults: { dropAfter: 20000 } });
    const back = new Promise((resolve) => pet.on('connect', (c) => c.reconnected && resolve()));
    await assert.rejects(pet.pack({ name: 'neo', files: [{ name: 'idle.gif', data: bytes(50000) }, { name: 'manifest.json', data: bytes(9) }] }), {
      message: 'Link lost during the update; the pet keeps what it had',
    });
    assert.equal(sim.fs.has('characters/neo/idle.gif'), false);
    await back;
    assert.match((await pet.run('hello')).message, /Pet runs hello/); // and works again
  });

  test('cancel: ABORT goes out, the job and the queue end', async (t) => {
    const { sim, pet } = await pair(t, { rate: 50_000 });
    const a = pet.pack({ name: 'neo', files: [{ name: 'idle.gif', data: bytes(80000) }, { name: 'manifest.json', data: bytes(9) }] });
    const b = pet.selftest();
    await waitFor(() => pet.phase === 'sending');
    pet.cancel();
    await assert.rejects(a, { message: 'Update cancelled' });
    await assert.rejects(b, { message: 'Cancelled' });
    await waitFor(() => sim.seen.aborts === 1);
    await waitFor(() => sim.state === 'idle');
  });

  test('not connected: refused at once', async () => {
    const pet = new op.Pet(new SimPet(FAST_SIM), FAST_PET);
    await assert.rejects(pet.run('hello'), { message: 'Not connected to a pet' });
    await assert.rejects(pet.heartbeat({}), { message: 'Not connected to a pet' });
  });

  test('older firmware without a command says so', async (t) => {
    const { pet } = await pair(t, { faults: { refuse: [op.CMD.LISTEN] } });
    await assert.rejects(pet.listen(500), /did not take command 0x0c/);
  });
});

describe('speaker test and microphone', () => {
  test('selftest: the lcd28 hears itself', async (t) => {
    const { pet } = await pair(t);
    const r = await pet.selftest();
    assert.equal(r.message, 'Heard by its mic: +37.5 dB at 1 kHz (rms 191 vs 22 quiet)');
    assert.equal(r.heard, true);
  });

  test('selftest: the jc3248 just beeps', async (t) => {
    const { pet } = await pair(t, { project: 'jc3248_pet_idf' });
    assert.equal((await pet.selftest()).message, 'Beeped (this pet has no mic to check itself)');
  });

  test('listen: the levels and the recording come back', async (t) => {
    const { sim, pet } = await pair(t);
    const r = await pet.listen(3000, true);
    const heard = sim.seen.listens[0];
    const m = op.micMetrics(heard.pcm);
    assert.equal(r.samples, 48000);
    assert.equal(r.ms, 3000);
    assert.ok(r.pcm instanceof Int16Array);
    assert.equal(r.pcm.length, 48000);
    assert.deepEqual(r.pcm, op.decodePSND(op.encodePSND(heard.pcm))); // what the pet sent, decoded
    assert.deepEqual([r.rms, r.peak, r.freq, r.db, r.floor], [m.rms, m.peak, m.freq, m.db, m.floor]);
    assert.equal(r.freq, 1000);
    assert.equal(r.sent, true);
    assert.equal(r.estimated, false);
    assert.equal(r.sampleRate, 16000);
  });

  test('listen: "heard" before the audio works the same', async (t) => {
    const { pet } = await pair(t, { heardFirst: true });
    const r = await pet.listen(1000, true);
    assert.equal(r.pcm.length, 16000);
    assert.equal(r.estimated, false);
  });

  test('listen: a lost "heard" line is measured here', async (t) => {
    const { pet } = await pair(t, { faults: { dropStatus: ['heard'] } });
    const r = await pet.listen(1000, true);
    assert.equal(r.estimated, true);
    assert.equal(r.pcm.length, 16000);
    assert.equal(r.freq, 1000);
    assert.match(r.message, /measured here/);
  });

  test('listen: the numbers only', async (t) => {
    const { pet } = await pair(t);
    const r = await pet.listen(500, false);
    assert.equal(r.pcm, null);
    assert.equal(r.sent, false);
    assert.equal(r.samples, 8000);
  });

  test('listen: a board without a mic', async (t) => {
    const { pet } = await pair(t, { project: 'jc3248_pet_idf' });
    await assert.rejects(pet.listen(500), (e) => e.why === 'no mic' && /no microphone/.test(e.message));
  });
});

describe('NUS', () => {
  test('heartbeat: the pet reassembles the exact line from small writes', async (t) => {
    const { sim, pet } = await pair(t, { maxWrite: 20 });
    const hb = { total: 3, running: 1, waiting: 2, msg: 'wait: neo, "x" · นีโอ', epoch: 1792547421, tzoff: 25200, pct5h: 8, reset5h: 14100, who: 'neo' };
    const line = await pet.heartbeat(hb);
    assert.equal(line, op.heartbeatLine(hb));
    assert.equal(`${sim.seen.heartbeatLines[0]}\n`, line);
    assert.equal(sim.seen.heartbeats[0].waiting, 2);
  });

  test('events: tap and say arrive whole, even split over notifications', async (t) => {
    const { sim, pet } = await pair(t);
    const said = new Promise((resolve) => pet.on('say', resolve));
    const tapped = new Promise((resolve) => pet.on('tap', resolve));
    sim.tap();
    sim.say('สวัสดี neo');
    await tapped;
    assert.deepEqual(await said, { text: 'สวัสดี neo' });
  });
});
