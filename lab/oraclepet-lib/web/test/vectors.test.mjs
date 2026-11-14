// Every golden vector in ../golden/vectors.json (generated from the Swift PetProtocol the pets
// already accept, lab/desk-pet-mac Sources/OraclePetVectors), reproduced by oraclepet.js byte for byte.
// Run: node --test   (from lab/oraclepet-lib/web)
import { describe, test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import * as op from '../oraclepet.js';

const v = JSON.parse(readFileSync(new URL('../../golden/vectors.json', import.meta.url), 'utf8'));
const utf8 = (s) => new TextEncoder().encode(s);

test('protocol version', () => assert.equal(op.PROTOCOL_VERSION, v.protocol));

describe('uuids', () => {
  const key = {
    nus_service: 'nusService',
    nus_rx: 'nusRx',
    nus_tx: 'nusTx',
    ota_service: 'otaService',
    ota_control: 'otaControl',
    ota_data: 'otaData',
    ota_mic: 'otaMic',
  };
  for (const [name, uuid] of Object.entries(v.uuids)) test(name, () => assert.equal(op.UUID[key[name]], uuid));
  test('nothing more, nothing less', () => assert.deepEqual(Object.keys(op.UUID).sort(), Object.values(key).sort()));
});

test('pet projects', () => assert.deepEqual(Object.keys(op.PROJECTS), v.projects));
test('IMA step table', () => assert.deepEqual([...op.IMA_STEP], v.ima_step));
test('IMA index table', () => assert.deepEqual([...op.IMA_INDEX], v.ima_index));

describe('crc32', () => {
  for (const c of v.crc32) {
    test(`crc32(${JSON.stringify(c.text_utf8)})`, () => {
      assert.equal(op.crc32(utf8(c.text_utf8)), c.crc32);
      assert.equal(op.crc32(c.text_utf8), c.crc32); // a string is taken as its UTF-8
    });
  }
});

describe('commands', () => {
  const build = {
    begin: (a) => op.cmd.begin(a.size, a.crc),
    begin_file: (a) => op.cmd.beginFile(a.size, a.crc, a.path),
    begin_stream: (a) => op.cmd.beginStream(a.size, a.crc),
    end: () => op.cmd.end(),
    abort: () => op.cmd.abort(),
    select_pack: (a) => op.cmd.selectPack(a.name),
    run_app: (a) => op.cmd.runApp(a.name),
    progress: (a) => op.cmd.progress(a.pct),
    play: (a) => op.cmd.play(a.name),
    selftest: () => op.cmd.selftest(),
    listen: (a) => op.cmd.listen(a.ms, a.send_audio),
  };
  for (const c of v.commands) {
    test(`${c.name} ${JSON.stringify(c.args)} -> ${c.hex}`, () => {
      assert.ok(build[c.name], `no builder for ${c.name}`);
      assert.equal(op.hex(build[c.name](c.args)), c.hex);
    });
  }
  test('the first byte of each matches CMD', () => {
    const byName = { begin: 'BEGIN', begin_file: 'BEGIN_FILE', begin_stream: 'BEGIN_STREAM', end: 'END', abort: 'ABORT', select_pack: 'SELECT_PACK', run_app: 'RUN_APP', progress: 'PROGRESS', play: 'PLAY', selftest: 'SELFTEST', listen: 'LISTEN' };
    for (const c of v.commands) assert.equal(parseInt(c.hex.slice(0, 2), 16), op.CMD[byName[c.name]], c.name);
  });
});

describe('heartbeat', () => {
  test('the exact line, "\\n" included', () => assert.equal(op.heartbeatLine(v.heartbeat.args), v.heartbeat.line));
  test('heartbeatBytes is its UTF-8', () => assert.deepEqual(op.heartbeatBytes(v.heartbeat.args), utf8(v.heartbeat.line)));
});

describe('psnd', () => {
  for (const clip of v.psnd) {
    test(`${clip.name}: encode pcm -> clip_hex`, () => assert.equal(op.hex(op.encodePSND(clip.pcm)), clip.clip_hex));
    test(`${clip.name}: encode from an Int16Array`, () => assert.equal(op.hex(op.encodePSND(Int16Array.from(clip.pcm))), clip.clip_hex));
    test(`${clip.name}: decode clip_hex -> decoded samples`, () => {
      const pcm = op.decodePSND(op.fromHex(clip.clip_hex));
      assert.ok(pcm instanceof Int16Array);
      assert.deepEqual(Array.from(pcm), clip.decoded);
    });
    test(`${clip.name}: the header`, () => {
      assert.deepEqual(op.psndInfo(op.fromHex(clip.clip_hex)), { version: 1, codec: 1, sampleRate: 16000, samples: clip.pcm.length });
    });
  }
});

describe('firmware image', () => {
  const img = op.fromHex(v.image.hex);
  const info = op.imageInfo(img);
  test('project', () => assert.equal(op.projectName(img), v.image.project));
  test('version', () => assert.equal(info.version, v.image.version));
  test('built', () => assert.equal(info.built, v.image.built));
  test('sha16', () => assert.equal(info.sha, v.image.sha16));
  test('the board check passes for its own board', () => assert.equal(op.firmwareProblem(img, { project: v.image.project }), null));
});

describe('status lines', () => {
  for (const s of v.status) test(`${s.kind}: ${s.line}`, () => assert.equal(op.parseStatus(s.line)?.kind, s.kind));
  test('and their fields', () => {
    const [ready, ack, streamed, heard, error] = v.status.map((s) => op.parseStatus(s.line));
    assert.deepEqual(ready, { kind: 'ready', size: 100, window: 16384 });
    assert.deepEqual(ack, { kind: 'ack', off: 4096 });
    assert.deepEqual(streamed, { kind: 'streamed', bytes: 14000, crcOK: true });
    assert.deepEqual(heard, { kind: 'heard', ms: 3000, samples: 48000, rms: 412, peak: 9120, freq: 1000, db: 23.4, floor: 31, sent: false });
    assert.deepEqual(error, { kind: 'error', why: 'no mic' });
  });
});
