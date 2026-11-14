// Adversarial checks, written apart from the implementation: a PSND reference coded straight
// from PROTOCOL.md 5, heartbeat escaping and clipping the way Heartbeat.swift does it, name and
// path limits taken from ota.c, a scripted link that plays the pet ack by ack (window, re-acks,
// END, error lines, stalls), LISTEN audio cut into odd notification sizes, and the Web Bluetooth
// link driven through a fake navigator.bluetooth to see which characteristic and write type
// every byte uses. Nothing touches real Bluetooth.
import { describe, test } from 'node:test';
import assert from 'node:assert/strict';
import * as op from '../oraclepet.js';
import { SimPet, safePath } from '../simpet.js';

const enc = new TextEncoder();
const dec = new TextDecoder();
const tick = (ms = 10) => new Promise((r) => setTimeout(r, ms));
const waitFor = async (cond, ms = 2000) => {
  const end = Date.now() + ms;
  while (!cond()) {
    if (Date.now() > end) throw new Error('timed out waiting');
    await tick(2);
  }
};

const FAST_SIM = { rate: 4_000_000, tickMs: 1, idleAckMs: 30, idleFailMs: 3000, endWaitMs: 1000, streamEndWaitMs: 300, timeScale: 0.01, playRate: 400_000 };
const FAST_PET = {
  stallMs: 600,
  watchdogMs: 20,
  rebootMs: 3000,
  settleMs: 5,
  reconnectMs: 0,
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

const wasm = (n = 64) => Uint8Array.from({ length: n }, (_, i) => [0x00, 0x61, 0x73, 0x6d, 1, 0, 0, 0][i] ?? (i * 7) & 0xff);

// ---- PSND: a reference from the PROTOCOL.md pseudocode ---------------------------------------

const STEP = [
  7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130,
  143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282,
  1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630,
  9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
];
const INDEX = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8];

function refStep(nib, st) {
  const step = STEP[st.index];
  let diff = step >> 3;
  if (nib & 1) diff += step >> 2;
  if (nib & 2) diff += step >> 1;
  if (nib & 4) diff += step;
  st.pred = nib & 8 ? st.pred - diff : st.pred + diff;
  st.pred = Math.max(-32768, Math.min(32767, st.pred));
  st.index = Math.max(0, Math.min(88, st.index + INDEX[nib]));
}

/** Returns the clip as number[] and the encoder state at the start of every block. */
function refEncode(pcm) {
  const out = [0x50, 0x53, 0x4e, 0x44, 1, 1, 0, 0, 0x80, 0x3e, 0, 0];
  const n = pcm.length;
  out.push(n & 0xff, (n >>> 8) & 0xff, (n >>> 16) & 0xff, (n >>> 24) & 0xff);
  const st = { pred: 0, index: 0 };
  const states = [];
  for (let start = 0; start < n; start += 504) {
    states.push({ ...st });
    const block = new Array(256).fill(0);
    const p = st.pred & 0xffff;
    block[0] = p & 0xff;
    block[1] = p >> 8;
    block[2] = st.index;
    for (let k = 0; k < 504 && start + k < n; k++) {
      let diff = pcm[start + k] - st.pred;
      let nib = 0;
      if (diff < 0) {
        nib = 8;
        diff = -diff;
      }
      let t = STEP[st.index];
      if (diff >= t) {
        nib |= 4;
        diff -= t;
      }
      t >>= 1;
      if (diff >= t) {
        nib |= 2;
        diff -= t;
      }
      t >>= 1;
      if (diff >= t) nib |= 1;
      refStep(nib, st);
      block[4 + (k >> 1)] |= k & 1 ? nib << 4 : nib; // LOW nibble first
    }
    out.push(...block);
  }
  return { clip: out, states };
}

/** Decodes every block from its own header (resetEachBlock) or from the first header only. */
function refDecode(clip, resetEachBlock = true) {
  const n = clip[12] | (clip[13] << 8) | (clip[14] << 16) | (clip[15] << 24);
  const out = [];
  const st = { pred: 0, index: 0 };
  for (let off = 16, blk = 0; out.length < n; off += 256, blk++) {
    if (resetEachBlock || blk === 0) {
      st.pred = ((clip[off] | (clip[off + 1] << 8)) << 16) >> 16;
      st.index = clip[off + 2];
    }
    const m = Math.min(504, n - out.length);
    for (let k = 0; k < m; k++) {
      refStep((clip[off + 4 + (k >> 1)] >> ((k & 1) * 4)) & 15, st);
      out.push(st.pred);
    }
  }
  return out;
}

function signal(n, seed) {
  let s = seed >>> 0;
  return Array.from({ length: n }, (_, i) => {
    s = (Math.imul(s, 1664525) + 1013904223) >>> 0;
    if (seed === 0) return i % 40 < 20 ? 32767 : -32768; // full-scale square: the clamps
    return Math.round(12000 * Math.sin(i / 7) + ((s >>> 16) - 32768) / 4);
  });
}

describe('PSND against a reference from PROTOCOL.md', () => {
  for (const n of [0, 1, 2, 503, 504, 505, 1007, 1008, 1009]) {
    for (const seed of [0, 7]) {
      test(`${n} samples (${seed ? 'tone + noise' : 'full-scale square'})`, () => {
        const pcm = signal(n, seed);
        const ref = refEncode(pcm);
        const clip = op.encodePSND(pcm);
        assert.equal(op.hex(clip), op.hex(ref.clip), 'encoded bytes');
        assert.equal(clip.length, 16 + Math.ceil(n / 504) * 256, 'every block padded to 256');
        assert.deepEqual(op.psndInfo(clip), { version: 1, codec: 1, sampleRate: 16000, samples: n });
        const out = op.decodePSND(clip);
        assert.equal(out.length, n);
        assert.deepEqual(Array.from(out), refDecode(ref.clip), 'decoded samples');
        // the state is carried across blocks: decoding straight through, ignoring every header
        // after the first, gives the same samples as restarting at each header
        assert.deepEqual(refDecode(ref.clip, false), refDecode(ref.clip, true));
        // each block header holds the encoder state before its first sample
        ref.states.forEach((st, b) => {
          const o = 16 + b * 256;
          assert.equal(((clip[o] | (clip[o + 1] << 8)) << 16) >> 16, st.pred, `block ${b} predictor`);
          assert.equal(clip[o + 2], st.index, `block ${b} index`);
          assert.equal(clip[o + 3], 0, `block ${b} reserved byte`);
        });
        // past the last sample the block is zero (a high nibble too, for an odd count)
        const rest = n % 504;
        if (rest) {
          const last = 16 + (Math.ceil(n / 504) - 1) * 256;
          if (rest & 1) assert.equal(clip[last + 4 + (rest >> 1)] >> 4, 0, 'unused high nibble');
          assert.ok(clip.subarray(last + 4 + Math.ceil(rest / 2)).every((b) => b === 0), 'padding');
          // a clip cut at psndLength still decodes in full
          assert.deepEqual(op.decodePSND(clip.subarray(0, op.psndLength(n))), out);
        }
      });
    }
  }

  test('nibble order: the first sample is the low nibble', () => {
    // one sample far above 0 sets bit 2 (4) at index 0; a second negative one gives 8|…
    const clip = op.encodePSND([1000, -1000]);
    const b = clip[16 + 4];
    assert.equal(b & 0x0f, refEncode([1000, -1000]).clip[20] & 0x0f);
    assert.equal(b & 0x08, 0, 'first sample (positive) has no sign bit in the low nibble');
    assert.equal(b & 0x80, 0x80, 'second sample (negative) has the sign bit in the high nibble');
  });
});

// ---- heartbeat ------------------------------------------------------------------------------

describe('heartbeat escaping and clipping (Heartbeat.swift)', () => {
  const base = { total: 1, running: 0, waiting: 0, epoch: 1, tzoff: 0 };
  const msgOf = (line) => /"msg":("(?:[^"\\]|\\.)*")/.exec(line)[1];

  test('quote, backslash, \\n, \\t, other control characters, DEL, Thai and emoji', () => {
    const msg = 'a"b\\c\nd\te\rf\u0001g\u001fh\u007fi สวัสดี 🐱';
    const line = op.heartbeatLine({ ...base, msg });
    assert.equal(msgOf(line), '"a\\"b\\\\c\\nd\\te\\u000df\\u0001g\\u001fh\u007fi สวัสดี 🐱"');
    assert.equal(JSON.parse(line).msg, msg);
    assert.ok(line.endsWith('}\n'));
  });

  test('empty strings stay in the line', () => {
    assert.equal(op.heartbeatLine({ ...base, msg: '', who: '' }), '{"total":1,"running":0,"waiting":0,"msg":"","epoch":1,"tzoff":0,"who":""}\n');
  });

  test('msg: 63 bytes of Thai are kept whole, the 64th byte starts a character that is left out', () => {
    assert.equal(JSON.parse(op.heartbeatLine({ ...base, msg: 'ก'.repeat(21) })).msg, 'ก'.repeat(21));
    assert.equal(JSON.parse(op.heartbeatLine({ ...base, msg: 'ก'.repeat(22) })).msg, 'ก'.repeat(21));
  });

  test('msg: a Thai consonant and its vowel mark are one character, never split', () => {
    // "กี" is 6 bytes; ten of them are 60, an eleventh would need 66
    assert.equal(JSON.parse(op.heartbeatLine({ ...base, msg: 'กี'.repeat(11) })).msg, 'กี'.repeat(10));
  });

  test('msg: an emoji sequence is one character too', () => {
    const family = '👨‍👩‍👧'; // 18 bytes
    assert.equal(JSON.parse(op.heartbeatLine({ ...base, msg: 'x'.repeat(50) + family })).msg, 'x'.repeat(50));
  });

  test('msg is clipped before it is escaped (the pet unescapes into char[64])', () => {
    const line = op.heartbeatLine({ ...base, msg: '"'.repeat(70) });
    assert.equal(JSON.parse(line).msg, '"'.repeat(63));
  });

  test('who: 20 characters, Thai counted as characters', () => {
    assert.equal(JSON.parse(op.heartbeatLine({ ...base, who: 'ก'.repeat(25) })).who, 'ก'.repeat(20));
    assert.equal(JSON.parse(op.heartbeatLine({ ...base, who: 'นีโอ' })).who, 'นีโอ');
  });

  test('a heartbeat reaches the pet whole when cut into small writes', async (t) => {
    const { sim, pet } = await pair(t, { maxWrite: 20 });
    const msg = 'สวัสดี "neo" \\ ok';
    await pet.heartbeat({ ...base, msg, who: 'นีโอ' });
    await waitFor(() => sim.seen.heartbeats.length === 1);
    assert.equal(sim.seen.heartbeats[0].msg, msg);
    assert.equal(sim.seen.heartbeats[0].who, 'นีโอ');
  });
});

// ---- commands, names and paths --------------------------------------------------------------

describe('commands at their edges', () => {
  test('empty names and paths', () => {
    assert.equal(op.hex(op.cmd.selectPack('')), '0500');
    assert.equal(op.hex(op.cmd.runApp('')), '0600');
    assert.equal(op.hex(op.cmd.play('')), '0800');
    assert.equal(op.hex(op.cmd.beginFile(0, 0, '')), '04000000000000000000');
  });

  test('a u8 length counts UTF-8 bytes', () => {
    assert.equal(op.hex(op.cmd.selectPack('แมว')), '0509e0b981e0b8a1e0b8a7');
    assert.equal(op.cmd.play('x'.repeat(255)).length, 257);
    assert.throws(() => op.cmd.play('x'.repeat(256)), RangeError);
    assert.throws(() => op.cmd.selectPack('ก'.repeat(86)), RangeError); // 258 bytes
  });

  test('LISTEN: u16 little-endian, clamped, flag bit 0', () => {
    assert.equal(op.hex(op.cmd.listen(0x1234, true)), '0c341201');
    assert.equal(op.hex(op.cmd.listen(65535, false)), '0c102700');
    assert.equal(op.hex(op.cmd.listen(99, false)), '0c640000');
  });

  test('crc32 of nothing and of Thai', () => {
    assert.equal(op.crc32(new Uint8Array(0)), 0);
    assert.equal(op.crc32(enc.encode('สวัสดี')), 2813700877);
  });

  test('paths: 95 bytes is the most, and whatever the host takes the firmware takes', () => {
    const at = (n) => `characters/${'p'.repeat(23)}/${'f'.repeat(n - 39)}.gif`;
    assert.equal(op.pathProblem(at(op.PATH_MAX)), null);
    assert.ok(safePath(at(op.PATH_MAX)));
    assert.notEqual(op.pathProblem(at(op.PATH_MAX + 1)), null);
    assert.equal(op.pathProblem(`sounds/${'s'.repeat(23)}.ima`), null);
    assert.notEqual(op.pathProblem(`sounds/${'s'.repeat(24)}.ima`), null);
    assert.notEqual(op.pathProblem('characters/แมว/idle.gif'), null);
  });
});

describe('name limits on the pet', () => {
  const n23 = 'a'.repeat(23);
  const n24 = 'a'.repeat(24);

  test('23-character names go out; 24 are refused before a byte is written', async (t) => {
    const { sim, pet } = await pair(t);
    await pet.app(wasm(300), false, { name: n23 });
    assert.ok(sim.fs.has(`apps/${n23}.wasm`));
    const before = sim.seen.control.length;
    await assert.rejects(pet.app(wasm(), false, { name: n24 }), /1 to 23/);
    await assert.rejects(pet.select(n24), /No such pack name/);
    await assert.rejects(pet.run(n24), /No such app name/);
    await assert.rejects(pet.play(n24), /No such sound name/);
    await assert.rejects(pet.select(''), /No such pack name/);
    assert.equal(sim.seen.control.length, before);
  });

  test('a pack whose longest path is 95 bytes lands; 96 is refused here', async (t) => {
    const { sim, pet } = await pair(t);
    const file = `${'f'.repeat(95 - 35 - 4)}.gif`;
    await pet.pack({ name: n23, files: [{ name: 'manifest.json', data: enc.encode('{}') }, { name: file, data: new Uint8Array(10) }] });
    assert.ok(sim.fs.has(`characters/${n23}/${file}`));
    const long = `${'f'.repeat(96 - 35 - 4)}.gif`;
    await assert.rejects(pet.pack({ name: n23, files: [{ name: 'manifest.json', data: enc.encode('{}') }, { name: long, data: new Uint8Array(10) }] }), /at most 95/);
  });
});

// ---- the transfer engine, one ack at a time -------------------------------------------------

/** A link the test drives by hand: it records every write and notifies only when told to. */
class ScriptLink {
  constructor({ maxWrite = 100 } = {}) {
    this.name = 'Claude-TEST';
    this.maxWrite = maxWrite;
    this.connected = false;
    this.hasOTA = true;
    this.hasMic = true;
    this.log = [];
    this.sent = 0;
    this.lastAck = 0;
    this.window = 0;
    this.maxAhead = 0;
    this.onBegin = null;
  }

  async connect() {
    this.connected = true;
  }

  async disconnect() {
    if (!this.connected) return;
    this.connected = false;
    this.onDisconnect?.();
  }

  async readControl() {
    return enc.encode('{"ver":"t","project":"oraclepet_lcd28","slot":"ota_0","built":"x"}');
  }

  async writeControl(b) {
    const bytes = Array.from(b);
    this.log.push({ ch: 'control', bytes });
    if ([0x01, 0x04, 0x0b].includes(bytes[0])) this.onBegin?.(bytes);
  }

  async writeData(b) {
    this.log.push({ ch: 'data', n: b.length });
    this.sent += b.length;
    this.maxAhead = Math.max(this.maxAhead, this.sent - this.lastAck);
  }

  async writeRx() {}

  notify(o) {
    if (o.ota === 'ack') this.lastAck = Math.max(this.lastAck, o.off);
    this.onControl?.(enc.encode(JSON.stringify(o)));
  }

  ops(code) {
    return this.log.filter((e) => e.ch === 'control' && e.bytes[0] === code).length;
  }
}

async function scripted(t, opts = {}) {
  const link = new ScriptLink(opts);
  const pet = new op.Pet(link, { ...FAST_PET, stallMs: 5000, ...opts });
  t.after(() => pet.disconnect());
  await pet.connect();
  return { link, pet };
}

describe('transfer engine against a scripted pet', () => {
  test('window, idle re-acks, duplicate acks, END only at ack.off == size, once', async (t) => {
    const { link, pet } = await scripted(t);
    link.onBegin = () => setTimeout(() => link.notify({ ota: 'ready', size: 1000, window: 300 }), 1);
    const job = pet.app(wasm(1000), false, { name: 'w' });
    await waitFor(() => link.sent === 300);
    await tick(20);
    assert.equal(link.sent, 300, 'stops at the window with nothing acked');
    link.notify({ ota: 'ack', off: 0 }); // the idle re-ack
    await tick(20);
    assert.equal(link.sent, 300);
    link.notify({ ota: 'ack', off: 150 });
    await waitFor(() => link.sent === 450);
    link.notify({ ota: 'ack', off: 150 }); // a duplicate
    link.notify({ ota: 'ack', off: 100 }); // an old one, out of order
    await tick(20);
    assert.equal(link.sent, 450, 'old and repeated acks free nothing');
    for (const off of [450, 700, 750]) {
      link.notify({ ota: 'ack', off });
      await tick(10);
    }
    await waitFor(() => link.sent === 1000);
    link.notify({ ota: 'ack', off: 999 });
    await tick(20);
    assert.equal(link.ops(0x02), 0, 'no END before every byte is acked');
    link.notify({ ota: 'ack', off: 1000 });
    await waitFor(() => link.ops(0x02) === 1);
    link.notify({ ota: 'ack', off: 1000 }); // re-acked while the pet waits for END
    await tick(20);
    assert.equal(link.ops(0x02), 1, 'END goes out once');
    link.notify({ ota: 'saved', path: 'apps/w.wasm' });
    await job;
    assert.ok(link.maxAhead <= 300, `ran ${link.maxAhead} ahead of the acks`);
    const writes = link.log.filter((e) => e.ch === 'data');
    assert.ok(writes.every((w) => w.n <= 100));
    // order on the control characteristic: BEGIN_FILE first, END last
    const ctl = link.log.filter((e) => e.ch === 'control').map((e) => e.bytes[0]);
    assert.equal(ctl[0], 0x04);
    assert.equal(ctl.at(-1), 0x02);
    assert.ok(ctl.slice(1, -1).every((c) => c === 0x07), 'only PROGRESS in between');
  });

  test('an error line mid-transfer fails the job with the pet\'s words and no END; the next job still runs', async (t) => {
    const { link, pet } = await scripted(t);
    let begins = 0;
    link.onBegin = () => {
      begins += 1;
      setTimeout(() => link.notify({ ota: 'ready', size: 500, window: 200 }), 1);
    };
    const first = pet.app(wasm(500), false, { name: 'a' });
    const second = pet.app(wasm(500), false, { name: 'b' });
    await waitFor(() => link.sent >= 200);
    link.notify({ ota: 'error', why: 'storage full' });
    await assert.rejects(first, (e) => e.why === 'storage full' && /Pet refused the update: storage full/.test(e.message));
    assert.equal(link.ops(0x02), 0);
    await waitFor(() => begins === 2);
    link.notify({ ota: 'ack', off: 500 }); // a late ack from the failed job is not this job's
    await waitFor(() => link.sent >= 400);
    for (const off of [200, 400, 500]) {
      link.notify({ ota: 'ack', off });
      await tick(10);
    }
    await waitFor(() => link.ops(0x02) === 1);
    link.notify({ ota: 'saved', path: 'apps/b.wasm' });
    await second;
  });

  test('a ready for another size is not this transfer\'s', async (t) => {
    const { link, pet } = await scripted(t, { stallMs: 200 });
    link.onBegin = () => setTimeout(() => link.notify({ ota: 'ready', size: 999, window: 200 }), 1);
    await assert.rejects(pet.app(wasm(500), false, { name: 'a' }), /stalled/);
    assert.equal(link.sent, 0);
  });

  test('stall: no acks after ready -> ABORT, then the job fails', async (t) => {
    const { link, pet } = await scripted(t, { stallMs: 150 });
    link.onBegin = () => setTimeout(() => link.notify({ ota: 'ready', size: 1000, window: 300 }), 1);
    await assert.rejects(pet.app(wasm(1000), false, { name: 'a' }), /stalled/);
    assert.equal(link.ops(0x03), 1);
    assert.equal(link.sent, 300);
  });

  test('an error with no job running is only logged', async (t) => {
    const { link, pet } = await scripted(t);
    const logs = [];
    pet.on('log', (m) => logs.push(m));
    link.notify({ ota: 'error', why: 'stray' });
    assert.ok(logs.some((m) => /stray/.test(m)));
  });
});

// ---- LISTEN: the mic stream in odd pieces ---------------------------------------------------

describe('LISTEN reassembly', () => {
  for (const micChunk of [1, 7, 20, 182]) {
    for (const heardFirst of [false, true]) {
      test(`notifications of ${micChunk} bytes, "heard" ${heardFirst ? 'before' : 'after'} the audio`, async (t) => {
        const { sim, pet } = await pair(t, { micChunk, heardFirst });
        const r = await pet.listen(1001, true); // 16016 samples: the last block holds 392
        const heard = sim.seen.listens[0].pcm;
        assert.equal(r.samples, 16016);
        assert.equal(r.estimated, false);
        assert.equal(r.clip.length, 16 + Math.ceil(16016 / 504) * 256, 'header + every block, as audio.c sends them');
        assert.deepEqual(Array.from(r.pcm), refDecode(refEncode(Array.from(heard)).clip));
      });
    }
  }

  test('levels measured here agree with the pet\'s way of measuring', () => {
    const pcm = new Int16Array(16000);
    for (let i = 0; i < pcm.length; i++) pcm[i] = Math.round(3000 * Math.sin((2 * Math.PI * 1000 * i) / 16000));
    const m = op.micMetrics(pcm);
    assert.equal(m.freq, 1000);
    assert.equal(m.peak, 3000);
    assert.ok(Math.abs(m.rms - 2121) <= 1);
    assert.ok(m.db > 30);
  });
});

// ---- the Web Bluetooth link, through a fake navigator.bluetooth -----------------------------

/** A BluetoothDevice whose GATT server is a SimPet; every write is logged with its type. */
function fakeBluetooth(sim) {
  const writes = [];
  const device = new EventTarget();
  device.name = sim.name;
  const ch = (uuid) => {
    const c = new EventTarget();
    c.uuid = uuid;
    const deliver = (b) => {
      c.value = new DataView(Uint8Array.from(b).buffer);
      c.dispatchEvent(new Event('characteristicvaluechanged'));
    };
    const send = async (type, v) => {
      const b = new Uint8Array(v.buffer, v.byteOffset, v.byteLength).slice();
      writes.push({ uuid, type, n: b.length });
      if (uuid === op.UUID.otaControl) return sim.writeControl(b);
      if (uuid === op.UUID.otaData) return sim.writeData(b);
      if (uuid === op.UUID.nusRx) return sim.writeRx(b);
      throw new Error(`write to ${uuid}`);
    };
    c.writeValueWithResponse = (v) => send('with', v);
    c.writeValueWithoutResponse = (v) => send('without', v);
    c.readValue = async () => {
      const b = await sim.readControl();
      return new DataView(b.buffer, b.byteOffset, b.byteLength);
    };
    c.startNotifications = async () => {
      if (uuid === op.UUID.otaControl) sim.onControl = deliver;
      if (uuid === op.UUID.otaMic) sim.onMic = deliver;
      if (uuid === op.UUID.nusTx) sim.onTx = deliver;
      return c;
    };
    return c;
  };
  const chars = Object.fromEntries([op.UUID.nusRx, op.UUID.nusTx, op.UUID.otaControl, op.UUID.otaData, op.UUID.otaMic].map((u) => [u, ch(u)]));
  const service = (uuids) => ({ getCharacteristic: async (u) => (uuids.includes(u) ? chars[u] : Promise.reject(new Error('NotFoundError'))) });
  const server = {
    getPrimaryService: async (u) =>
      u === op.UUID.nusService ? service([op.UUID.nusRx, op.UUID.nusTx]) : service([op.UUID.otaControl, op.UUID.otaData, op.UUID.otaMic]),
  };
  sim.onDisconnect = () => {
    device.gatt.connected = false;
    device.dispatchEvent(new Event('gattserverdisconnected'));
  };
  device.gatt = {
    connected: false,
    async connect() {
      await sim.connect();
      this.connected = true;
      return server;
    },
    disconnect() {
      if (sim.connected) sim.disconnect();
      else if (this.connected) sim.onDisconnect();
    },
  };
  const requests = [];
  const bluetooth = { requestDevice: async (o) => (requests.push(o), device) };
  return { bluetooth, writes, requests };
}

describe('WebBluetoothLink', () => {
  test('data on 6e400012 without response, commands on 6e400011 with response, heartbeats on NUS RX', async (t) => {
    const sim = new SimPet({ ...FAST_SIM });
    const fake = fakeBluetooth(sim);
    const pet = await op.Pet.request({ bluetooth: fake.bluetooth, chunkSize: 100, ...FAST_PET });
    t.after(() => pet.disconnect());
    assert.deepEqual(fake.requests[0].filters, [{ namePrefix: 'Claude-' }]);
    assert.deepEqual(fake.requests[0].optionalServices.sort(), [op.UUID.nusService, op.UUID.otaService].sort());
    const info = await pet.connect();
    assert.equal(info.project, 'oraclepet_lcd28');
    await pet.app(wasm(2500), true, { name: 'linked' });
    assert.equal(sim.fs.get('apps/linked.wasm').length, 2500);
    await pet.heartbeat({ total: 1, running: 1, waiting: 0, msg: 'สวัสดี', epoch: 1, tzoff: 0 });
    const r = await pet.listen(300, true);
    assert.equal(r.pcm.length, 4800);
    const by = (uuid) => fake.writes.filter((w) => w.uuid === uuid);
    assert.ok(by(op.UUID.otaData).length > 0);
    assert.ok(by(op.UUID.otaData).every((w) => w.type === 'without' && w.n <= 100), 'data: without response, <= chunkSize');
    assert.ok(by(op.UUID.otaControl).every((w) => w.type === 'with'), 'commands: with response');
    assert.ok(by(op.UUID.nusRx).length > 0 && by(op.UUID.nusRx).every((w) => w.type === 'without' && w.n <= 100));
    await waitFor(() => sim.seen.heartbeats.length === 1);
    assert.equal(sim.seen.heartbeats[0].msg, 'สวัสดี');
    assert.equal(dec.decode(Uint8Array.from(sim.seen.control.at(-1))).length > 0, true);
  });
});
