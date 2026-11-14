// The protocol half beyond the golden vectors: limits, edge cases, and the rules the Swift
// tests (lab/desk-pet-mac Tests/PetProtocolTests) pin for the Mac app, ported.
import { describe, test } from 'node:test';
import assert from 'node:assert/strict';
import * as op from '../oraclepet.js';
import { makeImage, safePath } from '../simpet.js';

const utf8 = (s) => new TextEncoder().encode(s);

describe('crc32', () => {
  test('the standard check value', () => assert.equal(op.crc32(utf8('123456789')), 0xcbf43926));
  test('continues over more bytes, like ota.c crc_update', () => {
    const a = utf8('Oracle');
    const b = utf8('Pet');
    assert.equal(op.crc32(b, op.crc32(a)), op.crc32(utf8('OraclePet')));
  });
  test('takes ArrayBuffers and views', () => {
    const bytes = utf8('123456789');
    assert.equal(op.crc32(bytes.buffer.slice(0)), 0xcbf43926);
    assert.equal(op.crc32(new DataView(bytes.buffer)), 0xcbf43926);
  });
});

describe('commands', () => {
  test('BEGIN is little-endian', () => assert.deepEqual([...op.cmd.begin(0x0a0b0c, 0xdeadbeef)], [0x01, 0x0c, 0x0b, 0x0a, 0x00, 0xef, 0xbe, 0xad, 0xde]));
  test('BEGIN_FILE carries the path after a u8 length', () => {
    const b = op.cmd.beginFile(1, 2, 'characters/neo/idle.gif');
    assert.equal(b[0], 0x04);
    assert.equal(b[9], 23);
    assert.equal(new TextDecoder().decode(b.subarray(10)), 'characters/neo/idle.gif');
  });
  test('BEGIN_STREAM is 9 bytes', () => {
    const b = op.cmd.beginStream(0x0102, 1);
    assert.equal(b[0], 0x0b);
    assert.equal(b.length, 9);
  });
  test('LISTEN clamps to 100..10000 ms', () => {
    assert.deepEqual([...op.cmd.listen(50, false)], [0x0c, 0x64, 0x00, 0]);
    assert.deepEqual([...op.cmd.listen(60000, true)], [0x0c, 0x10, 0x27, 1]);
  });
  test('PROGRESS clamps to 0..100', () => {
    assert.deepEqual([...op.cmd.progress(-5)], [0x07, 0]);
    assert.deepEqual([...op.cmd.progress(250)], [0x07, 100]);
  });
  test('RUN_APP with no name stops the app', () => assert.equal(op.hex(op.cmd.runApp()), '0600'));
  test('sizes and names out of range throw instead of wrapping', () => {
    assert.throws(() => op.cmd.begin(-1, 0), RangeError);
    assert.throws(() => op.cmd.begin(2 ** 32, 0), RangeError);
    assert.throws(() => op.cmd.begin(1.5, 0), RangeError);
    assert.throws(() => op.cmd.selectPack('x'.repeat(256)), RangeError);
    assert.throws(() => op.cmd.beginFile(1, 1, 'x'.repeat(256)), RangeError);
  });
  test('a crc from anywhere in 32 bits is written as its unsigned bits', () => {
    assert.equal(op.hex(op.cmd.begin(1, -1)), op.hex(op.cmd.begin(1, 0xffffffff)));
  });
});

describe('TransferWindow (OTAWindow)', () => {
  test('never runs ahead of the window', () => {
    const w = new op.TransferWindow(10000, 4096);
    let sent = 0;
    for (let r; (r = w.next(512)); ) sent += r[1] - r[0];
    assert.equal(sent, 4096); // window full: waits for an ack
    w.ack(2048);
    assert.deepEqual(w.next(4096), [4096, 6144]); // only what the ack freed
    w.ack(99999); // never past what was sent
    assert.equal(w.acked, 6144);
    while (w.next(512));
    w.ack(10000);
    assert.ok(w.allAcked);
    assert.equal(w.sent, 10000);
  });
  test('an empty transfer is acked from the start', () => {
    const w = new op.TransferWindow(0, 4096);
    assert.equal(w.next(180), null);
    assert.ok(w.allAcked);
  });
});

describe('PSND', () => {
  test('header and blocks', () => {
    const c = op.encodePSND(new Int16Array(1000));
    assert.deepEqual([...c.subarray(0, 16)], [0x50, 0x53, 0x4e, 0x44, 1, 1, 0, 0, 0x80, 0x3e, 0, 0, 0xe8, 0x03, 0, 0]);
    assert.equal(c.length, 16 + 2 * 256); // 1000 samples = 2 blocks of 504
  });
  test('low nibble first', () => {
    const c = op.encodePSND([1000, 1000]);
    assert.ok((c[16 + 4] & 0x0f) !== 0 && c[16 + 4] >> 4 !== 0);
  });
  test('a tone keeps its shape (SNR > 20 dB)', () => {
    const pcm = Int16Array.from({ length: 16000 }, (_, i) => Math.trunc(16000 * Math.sin((2 * Math.PI * 440 * i) / 16000)));
    const back = op.decodePSND(op.encodePSND(pcm));
    assert.equal(back.length, pcm.length);
    let sig = 0;
    let err = 0;
    for (let i = 0; i < pcm.length; i++) {
      sig += pcm[i] * pcm[i];
      err += (pcm[i] - back[i]) ** 2;
    }
    assert.ok(10 * Math.log10(sig / Math.max(err, 1)) > 20);
  });
  test('every block restarts from its own header', () => {
    const pcm = Int16Array.from({ length: 1500 }, (_, i) => (i * 37) % 20000 - 10000);
    const clip = op.encodePSND(pcm);
    const good = op.decodePSND(clip);
    clip.fill(0xff, 16 + 4, 16 + 256); // spoil block 0's samples
    const spoiled = op.decodePSND(clip);
    assert.deepEqual(Array.from(spoiled.subarray(504)), Array.from(good.subarray(504)));
  });
  test('a cut-off clip decodes as far as its bytes go', () => {
    const clip = op.encodePSND(new Int16Array(1000).fill(5000));
    assert.equal(op.decodePSND(clip.subarray(0, 16 + 4 + 50)).length, 100);
    assert.equal(op.decodePSND(clip.subarray(0, 16 + 256)).length, 504);
    assert.equal(op.decodePSND(clip.subarray(0, 16)).length, 0);
  });
  test('psndLength is the fewest bytes that decode in full', () => {
    for (const n of [0, 1, 503, 504, 505, 1000, 48000]) {
      const clip = op.encodePSND(new Int16Array(n).fill(1234));
      assert.equal(op.decodePSND(clip.subarray(0, op.psndLength(n))).length, n, `n=${n}`);
      if (n) assert.ok(op.decodePSND(clip.subarray(0, op.psndLength(n) - 1)).length < n, `n=${n}`);
    }
  });
  test('not a clip: null', () => {
    assert.equal(op.decodePSND(utf8('RIFF....WAVEfmt ')), null);
    assert.equal(op.decodePSND(new Uint8Array(8)), null);
    const v2 = op.encodePSND([1, 2, 3]);
    v2[4] = 2;
    assert.equal(op.decodePSND(v2), null);
  });
  test('a header that claims 4 billion samples does not allocate them', () => {
    const c = op.encodePSND([1, 2, 3]);
    new DataView(c.buffer).setUint32(12, 0xffffffff, true);
    assert.equal(op.decodePSND(c).length, 504);
  });
  test('samples outside int16 are clamped on the way in', () => {
    const c = op.encodePSND([40000, -40000]);
    assert.deepEqual(Array.from(op.decodePSND(c)).map((x) => Math.sign(x)), [1, -1]);
  });
});

describe('tidy', () => {
  test('trims and scales (the Swift test)', () => {
    const pcm = [...new Array(1000).fill(0), 10000, -10000, 5000, ...new Array(1000).fill(0)];
    const t = op.tidy(pcm);
    assert.ok(t.length < pcm.length && t.length >= 3);
    assert.equal(Math.max(...Array.from(t, Math.abs)), Math.round(32767 * 0.9));
  });
  test('keeps 20 ms around the sound', () => {
    const pcm = new Int16Array(5000);
    pcm[2000] = 8000;
    const t = op.tidy(pcm);
    assert.equal(t.length, 320 + 1 + 320);
    assert.equal(t[320], Math.round(32767 * 0.9));
  });
  test('silence (all |s| <= 300) is nothing', () => assert.equal(op.tidy(new Int16Array(1000).fill(300)).length, 0));
  test('caps the length', () => assert.equal(op.tidy(new Int16Array(200000).fill(1000)).length, 96000));
  test('rounds half away from zero, as Swift does', () => {
    assert.deepEqual(Array.from(op.tidy([32767, 301, -301, -3], { peak: 0.5 })), [16384, 151, -151, -2]);
  });
});

test('isThai', () => {
  assert.ok(op.isThai('นีโอรอคุณอยู่'));
  assert.ok(!op.isThai('neo needs you'));
});

test('wav header (PetSound.wav)', () => {
  const w = op.wav([0, 1, -1]);
  assert.equal(w.length, 44 + 6);
  assert.equal(new TextDecoder().decode(w.subarray(0, 4)), 'RIFF');
  assert.equal(new TextDecoder().decode(w.subarray(8, 12)), 'WAVE');
  const v = new DataView(w.buffer);
  assert.equal(v.getUint32(24, true), 16000);
  assert.equal(v.getInt16(44 + 4, true), -1);
});

test('pcmFromFloat', () => assert.deepEqual(Array.from(op.pcmFromFloat([0, 1, -1, 2, 0.5])), [0, 32767, -32767, 32767, 16384]));

describe('micMetrics', () => {
  test('finds a 1 kHz tone above the room', () => {
    const pcm = Int16Array.from({ length: 16000 }, (_, i) => (i < 1600 ? 0 : Math.round(3000 * Math.sin((2 * Math.PI * 1000 * i) / 16000))));
    const m = op.micMetrics(pcm);
    assert.equal(m.freq, 1000);
    assert.ok(m.db > 20);
    assert.equal(m.floor, 0); // the quietest 100 ms
    assert.equal(m.peak, 3000);
    assert.ok(Math.abs(m.rms - 2012) < 5);
  });
  test('nothing in, nothing out', () => assert.deepEqual(op.micMetrics([]), { samples: 0, rms: 0, peak: 0, freq: 0, db: 0, floor: 0 }));
});

describe('firmware images', () => {
  const img = makeImage({ project: 'jc3248_pet_idf', version: 'v9', size: 600 });
  test('project from the app description (the Swift test)', () => {
    const raw = new Uint8Array(300);
    raw[0] = 0xe9;
    raw.set([0x32, 0x54, 0xcd, 0xab], 32);
    raw.set(utf8('jc3248_pet_idf'), 80);
    assert.equal(op.projectName(raw), 'jc3248_pet_idf');
    assert.equal(op.projectName(new Uint8Array(300)), null);
  });
  test('not an image: null', () => {
    assert.equal(op.imageInfo(utf8('hello')), null);
    const wrongMagic = img.slice();
    wrongMagic[32] = 0;
    assert.equal(op.imageInfo(wrongMagic), null);
  });
  test('the board check (PetLink.updateFirmware)', () => {
    assert.equal(op.firmwareProblem(utf8('not firmware'), null), 'Not an ESP32 app image');
    assert.equal(op.firmwareProblem(makeImage({ project: 'hello_world' }), null), 'Not a pet firmware: hello_world');
    assert.equal(
      op.firmwareProblem(img, { project: 'oraclepet_lcd28' }),
      'That firmware is for the JC3248 3.5in board; this pet is 2.8in ILI9341',
    );
    assert.equal(op.firmwareProblem(img, { project: 'jc3248_pet_idf' }), null);
    assert.equal(op.firmwareProblem(img, { project: '' }), null); // older firmware: no project to hold against it
    assert.equal(op.firmwareProblem(img, null), null);
  });
  test('the OTA base takes any app image, and only app images', () => {
    const base = { project: op.BASE_PROJECT };
    assert.equal(op.firmwareProblem(makeImage({ project: 'hello_world' }), base), null);
    assert.equal(op.firmwareProblem(img, base), null);
    assert.equal(op.firmwareProblem(utf8('not firmware'), base), 'Not an ESP32 app image');
  });
  test('board names', () => {
    assert.equal(op.boardName('oraclepet_lcd28'), '2.8in ILI9341');
    assert.equal(op.boardName('jc3248_pet_idf'), 'JC3248 3.5in');
    assert.equal(op.boardName('esp32_other'), 'esp32_other');
    assert.ok(!op.isPetProject('constructor')); // not fooled by Object.prototype
  });
  test('sameBuild compares the SHA prefix, else version and time', () => {
    const pet = { version: 'v', slot: 'ota_1', built: 'a', sha: 'e5893e747' };
    assert.ok(op.sameBuild(pet, { version: 'v', built: 'a', sha: 'e5893e7471c0ffee' }));
    assert.ok(!op.sameBuild(pet, { version: 'v', built: 'a', sha: 'bd09fced0359d3da' }));
    assert.ok(op.sameBuild({ version: 'v', built: 'a', sha: '' }, { version: 'v', built: 'a', sha: 'bd09fced0359d3da' }));
  });
});

describe('heartbeat', () => {
  const base = { total: 1, running: 0, waiting: 0, epoch: 0, tzoff: 0 };
  test('optional fields stay out unless given', () => assert.equal(op.heartbeatLine({ ...base, msg: '' }), '{"total":1,"running":0,"waiting":0,"msg":"","epoch":0,"tzoff":0}\n'));
  test('the 7-day window and who, in the Mac app order', () => {
    const line = op.heartbeatLine({ ...base, msg: 'x', pct5h: 1, reset5h: 2, pct7d: 3, reset7d: 4, who: 'nexus' });
    assert.ok(line.endsWith('"pct5h":1,"reset5h":2,"pct7d":3,"reset7d":4,"who":"nexus"}\n'));
  });
  test('msg is clipped to 63 UTF-8 bytes without splitting a character', () => {
    const thai = 'นีโอรอคุณอยู่'.repeat(3); // combining marks: clusters of 2 and 3 code points
    const o = JSON.parse(op.heartbeatLine({ ...base, msg: thai }));
    assert.ok(utf8(o.msg).length <= 63);
    assert.ok(thai.startsWith(o.msg));
    const seg = new Intl.Segmenter(undefined, { granularity: 'grapheme' });
    const clusters = Array.from(seg.segment(thai), (s) => s.segment);
    let joined = '';
    for (const c of clusters) {
      if (joined === o.msg) break;
      joined += c;
    }
    assert.equal(joined, o.msg); // ends on a cluster boundary
    assert.equal(op.clipUtf8('a'.repeat(100)).length, 63);
  });
  test('who is cut to 20 characters', () => assert.equal(JSON.parse(op.heartbeatLine({ ...base, who: 'abcdefghijklmnopqrstuvwxyz' })).who, 'abcdefghijklmnopqrst'));
  test('control characters are escaped the Swift way', () => {
    const line = op.heartbeatLine({ ...base, msg: 'a\u0001b\tc\nd\\e' });
    assert.ok(line.includes('"msg":"a\\u0001b\\tc\\nd\\\\e"'));
    assert.equal(JSON.parse(line).msg, 'a\u0001b\tc\nd\\e');
  });
  test('epoch and tzoff default to this clock', () => {
    const o = JSON.parse(op.heartbeatLine({ total: 0 }));
    assert.ok(Math.abs(o.epoch - Date.now() / 1000) < 5);
    assert.equal(o.tzoff, -new Date().getTimezoneOffset() * 60);
  });
  test('the state rule and the smallest counts for each state', () => {
    assert.equal(op.petState({ total: 3, running: 1, waiting: 2 }), 'attention');
    assert.equal(op.petState({ total: 3, running: 1, waiting: 0 }), 'busy');
    assert.equal(op.petState({ total: 0, running: 0, waiting: 0 }), 'sleep');
    assert.equal(op.petState({ total: 2, running: 0, waiting: 0 }), 'idle');
    for (const [state, counts] of Object.entries(op.STATE_COUNTS)) assert.equal(op.petState(counts), state);
  });
});

describe('what the pet says', () => {
  test('info: packs and apps sorted, free in KB (the Swift tests)', () => {
    const fw = op.parseInfo('{"ver":"v","slot":"ota_0","pack":"neo","packs":["siamese","neo"],"free":900,"project":"oraclepet_lcd28"}');
    assert.deepEqual(fw.packs, ['neo', 'siamese']);
    assert.equal(fw.freeKB, 900);
    assert.equal(fw.pack, 'neo');
    assert.equal(fw.board, '2.8in ILI9341');
    assert.equal(op.parseInfo('{"ver":"v1-3","built":"Oct 21 2026 11:31:00"}').slot, '?');
    assert.equal(op.parseInfo('{"ota":"ack","off":1}'), null);
  });
  test('status: what is not a status is null', () => {
    assert.equal(op.parseStatus('{"evt":"tap"}'), null);
    assert.equal(op.parseStatus('{"ota":"ack"}'), null); // an ack needs its offset
    assert.equal(op.parseStatus('{"ota":"whatever"}'), null);
    assert.equal(op.parseStatus('not json'), null);
    assert.equal(op.parseStatus('[1,2]'), null);
  });
  test('status: defaults as in OTAStatus.parse', () => {
    assert.deepEqual(op.parseStatus('{"ota":"ready","size":7}'), { kind: 'ready', size: 7, window: 4096 });
    assert.deepEqual(op.parseStatus('{"ota":"error"}'), { kind: 'error', why: 'unknown error' });
    assert.deepEqual(op.parseStatus('{"ota":"selftest","mic":true,"heard":true,"db":37.5,"quiet":22,"tone":191}'), {
      kind: 'selftest',
      mic: true,
      heard: true,
      db: 37.5,
      quiet: 22,
      tone: 191,
    });
    assert.equal(op.parseStatus('{"ota":"heard","ms":1,"sent":true}').sent, true);
    assert.deepEqual(op.parseStatus(utf8('{"ota":"play","name":"say","bytes":4112}')), { kind: 'play', name: 'say', bytes: 4112 });
  });
  test('events', () => {
    assert.deepEqual(op.parseEvent('{"evt":"say","text":"hi"}'), { evt: 'say', text: 'hi' });
    assert.equal(op.parseEvent('{"ota":"ack","off":1}'), null);
  });
  test('LineBuffer joins lines across notifications, UTF-8 included', () => {
    const lb = new op.LineBuffer();
    const bytes = utf8('{"evt":"say","text":"สวัสดี"}\n{"evt":"tap"}\n{"evt"');
    assert.deepEqual(lb.push(bytes.subarray(0, 25)), []); // cuts a Thai character in half
    assert.deepEqual(lb.push(bytes.subarray(25)), ['{"evt":"say","text":"สวัสดี"}', '{"evt":"tap"}']);
    assert.deepEqual(lb.push(utf8(':"tap"}\n')), ['{"evt":"tap"}']);
  });
  test('LineBuffer drops a line that never ends', () => {
    const lb = new op.LineBuffer(16);
    lb.push(utf8('x'.repeat(40)));
    assert.deepEqual(lb.push(utf8('ok\n')), ['ok']);
  });
  test('the speaker test in words (PetLink.otaReceived)', () => {
    assert.equal(op.describeSelftest({ mic: false }), 'Beeped (this pet has no mic to check itself)');
    assert.equal(op.describeSelftest({ mic: true, heard: true, db: 37.5, quiet: 22, tone: 191 }), 'Heard by its mic: +37.5 dB at 1 kHz (rms 191 vs 22 quiet)');
    assert.equal(op.describeSelftest({ mic: true, heard: false, db: -1.25, quiet: 22, tone: 25 }), 'Its mic heard nothing (-1.3 dB): speaker or amp off');
  });
});

describe('paths and names', () => {
  const good = ['characters/neo/idle.gif', 'characters/neo/manifest.json', 'apps/hello-zig.wasm', 'apps/a.b.wasm', 'sounds/say.ima', `apps/${'a'.repeat(23)}.wasm`];
  const bad = [
    'characters/Neo/idle.gif',
    'characters/neo/../x.gif',
    'characters/neo/sub/x.gif',
    'characters/neo.gif',
    'apps/hello.WASM',
    'apps/.wasm',
    `apps/${'a'.repeat(24)}.wasm`,
    'apps/x/y.wasm',
    'sounds/say.wav',
    'etc/passwd',
    'characters/neo/idle .gif',
    '',
  ];
  for (const p of good) test(`accepts ${p}`, () => assert.equal(op.pathProblem(p), null));
  for (const p of bad) test(`refuses ${JSON.stringify(p)}`, () => assert.notEqual(op.pathProblem(p), null));
  test('at most 95 bytes, what ota.c s_path[96] holds', () => {
    const at = (n) => `characters/${'p'.repeat(23)}/${'f'.repeat(n - 35 - 4)}.gif`;
    assert.equal(at(95).length, 95);
    assert.equal(op.pathProblem(at(95)), null);
    assert.match(op.pathProblem(at(96)), /at most 95/);
  });
  test('whatever the host accepts, the firmware accepts', () => {
    for (const p of [...good, ...bad]) if (op.pathProblem(p) === null) assert.ok(safePath(p), p);
  });
});

describe('packsFromFiles', () => {
  const file = (rel, size = 10) => ({ name: rel.split('/').pop(), webkitRelativePath: rel, size });
  test('one pack folder: its gifs and json, sorted; other files left out', () => {
    const [p, ...more] = op.packsFromFiles([file('neo/manifest.json'), file('neo/idle.gif', 100), file('neo/README.md'), file('neo/busy.gif')]);
    assert.equal(more.length, 0);
    assert.equal(p.name, 'neo');
    assert.deepEqual(p.files.map((f) => f.name), ['busy.gif', 'idle.gif', 'manifest.json']);
    assert.equal(p.bytes, 120);
    assert.deepEqual(p.problems, []);
  });
  test('a folder of packs (lab/buddy/characters)', () => {
    const packs = op.packsFromFiles([file('characters/neo/manifest.json'), file('characters/neo/idle.gif'), file('characters/Atlas/manifest.json'), file('characters/loose.gif')]);
    assert.deepEqual(packs.map((p) => p.name), ['atlas', 'neo']); // folder names are lower-cased, like the Mac app
  });
  test('no manifest, no pack', () => assert.deepEqual(op.packsFromFiles([file('neo/idle.gif')]), []));
  test('names the pet would refuse are listed as problems', () => {
    const [p] = op.packsFromFiles([file('neo/manifest.json'), file('neo/idle 2.gif')]);
    assert.equal(p.problems.length, 1);
    const [q] = op.packsFromFiles([file('my pack/manifest.json')]);
    assert.match(q.problems[0], /pack name/);
  });
});

describe('bytes', () => {
  test('toBytes takes views, buffers, arrays and strings', () => {
    assert.deepEqual([...op.toBytes(new Uint16Array([0x0201]))], [1, 2]);
    assert.deepEqual([...op.toBytes([1, 2, 3])], [1, 2, 3]);
    assert.deepEqual([...op.toBytes('é')], [0xc3, 0xa9]);
    assert.throws(() => op.toBytes(42), TypeError);
  });
  test('fromHex refuses what is not hex', () => {
    assert.throws(() => op.fromHex('abc'), TypeError);
    assert.throws(() => op.fromHex('zz'), TypeError);
    assert.deepEqual([...op.fromHex('00ff')], [0, 255]);
  });
  test('readBytes reads a Blob', async () => {
    assert.deepEqual([...(await op.readBytes(new Blob([Uint8Array.of(7, 8)])))], [7, 8]);
  });
});
