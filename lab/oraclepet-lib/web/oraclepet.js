/*
 * oraclepet.js: drive an OraclePet desk pet (ESP32-S3) from a web page over Web Bluetooth.
 *
 * One ES module, no build step, no dependencies. ../PROTOCOL.md is the spec and
 * ../golden/vectors.json pins its bytes; test/vectors.test.mjs checks this file against them.
 *
 *   1. Protocol  pure functions over bytes: crc32, the control commands, PSND clips (IMA ADPCM),
 *                firmware image info, heartbeat lines, status and info parsing. Nothing in this
 *                half touches navigator, so Node imports the module for the tests.
 *   2. Pet       one pet over a link. WebBluetoothLink is the real link; simpet.js is an
 *                in-memory pet with the same shape. Transfers run the Mac app's engine
 *                (lab/desk-pet-mac PetLink.swift): a window of unacked bytes, the pet's acks,
 *                END, the outcome, a stall watchdog, and a queue so a second request waits.
 */

// =============================================================================================
// 1. Protocol
// =============================================================================================

export const PROTOCOL_VERSION = 1;

const NUS_BASE = '-b5a3-f393-e0a9-e50e24dcca9e';

/** GATT UUIDs (PROTOCOL.md 2). Every one shares the Nordic UART base. */
export const UUID = Object.freeze({
  nusService: `6e400001${NUS_BASE}`,
  nusRx: `6e400002${NUS_BASE}`, // write: heartbeat lines, host -> pet
  nusTx: `6e400003${NUS_BASE}`, // notify: events, pet -> host
  otaService: `6e400010${NUS_BASE}`,
  otaControl: `6e400011${NUS_BASE}`, // read: info; write: commands; notify: status lines
  otaData: `6e400012${NUS_BASE}`, // write without response: transfer bytes
  otaMic: `6e400013${NUS_BASE}`, // notify: LISTEN audio
});

/** The pet advertises "Claude-<last two MAC bytes>", e.g. "Claude-AD48". */
export const NAME_PREFIX = 'Claude-';

/** Pet firmware projects, one per board. A host never sends one board's firmware to the other. */
export const PROJECTS = Object.freeze({
  jc3248_pet_idf: 'JC3248 3.5in',
  oraclepet_lcd28: '2.8in ILI9341',
  oraclepet_sc01: 'WT32-SC01 Plus',
});
export const isPetProject = (project) => Object.hasOwn(PROJECTS, project);
/** lab/ota-base: takes any ESP32-S3 app image over BLE and boots it. */
export const BASE_PROJECT = 'oraclepet_base';
export const boardName = (project) => (isPetProject(project) ? PROJECTS[project] : String(project ?? ''));

/** Control command bytes (PROTOCOL.md 4.2). */
export const CMD = Object.freeze({
  BEGIN: 0x01,
  END: 0x02,
  ABORT: 0x03,
  BEGIN_FILE: 0x04,
  SELECT_PACK: 0x05,
  RUN_APP: 0x06,
  PROGRESS: 0x07,
  PLAY: 0x08,
  SELFTEST: 0x0a,
  BEGIN_STREAM: 0x0b,
  LISTEN: 0x0c,
});

// ---- bytes ----------------------------------------------------------------------------------

const enc = new TextEncoder();
const dec = new TextDecoder('utf-8', { ignoreBOM: true }); // lossy, like Swift's String(decoding:as:)

/** A Uint8Array over bytes-like input (any typed-array view, ArrayBuffer, number[]), or UTF-8 of a string. */
export function toBytes(x) {
  if (x instanceof Uint8Array) return x;
  if (ArrayBuffer.isView(x)) return new Uint8Array(x.buffer, x.byteOffset, x.byteLength);
  if (x instanceof ArrayBuffer || Object.prototype.toString.call(x) === '[object ArrayBuffer]') return new Uint8Array(x);
  if (typeof x === 'string') return enc.encode(x);
  if (Array.isArray(x)) return Uint8Array.from(x);
  throw new TypeError('expected bytes: a Uint8Array, ArrayBuffer, typed-array view, number[] or string');
}

/** The bytes of a File or Blob (anything with arrayBuffer()), or of bytes-like input. */
export async function readBytes(x) {
  if (x && typeof x.arrayBuffer === 'function' && !ArrayBuffer.isView(x)) return new Uint8Array(await x.arrayBuffer());
  return toBytes(x);
}

export const hex = (bytes) => Array.from(toBytes(bytes), (b) => b.toString(16).padStart(2, '0')).join('');

export function fromHex(h) {
  if (typeof h !== 'string' || h.length % 2 || /[^0-9a-f]/i.test(h)) throw new TypeError('expected an even-length hex string');
  const out = new Uint8Array(h.length / 2);
  for (let i = 0; i < out.length; i++) out[i] = parseInt(h.slice(2 * i, 2 * i + 2), 16);
  return out;
}

const u32le = (b, at) => (b[at] | (b[at + 1] << 8) | (b[at + 2] << 16) | (b[at + 3] << 24)) >>> 0;

function concat(chunks, length = chunks.reduce((n, c) => n + c.length, 0)) {
  const out = new Uint8Array(length);
  let at = 0;
  for (const c of chunks) {
    out.set(c, at);
    at += c.length;
  }
  return out;
}

// ---- CRC-32 ---------------------------------------------------------------------------------

const CRC_TABLE = new Uint32Array(256);
for (let i = 0; i < 256; i++) {
  let c = i;
  for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
  CRC_TABLE[i] = c;
}

/**
 * zlib CRC-32 (poly 0xEDB88320, init and final XOR 0xFFFFFFFF), the one the pet keeps over what
 * it writes. Pass an earlier result as `crc` to continue it over more bytes (ota.c crc_update).
 */
export function crc32(data, crc = 0) {
  const b = toBytes(data);
  let c = ~crc;
  for (let i = 0; i < b.length; i++) c = CRC_TABLE[(c ^ b[i]) & 0xff] ^ (c >>> 8);
  return ~c >>> 0;
}

// ---- control commands -----------------------------------------------------------------------

const clampInt = (v, lo, hi) => Math.min(hi, Math.max(lo, Math.round(Number(v)) || 0));

function sized(op, size, crc) {
  if (!Number.isInteger(size) || size < 0 || size > 0xffffffff) throw new RangeError(`size must be an integer 0..4294967295, got ${size}`);
  if (!Number.isInteger(crc)) throw new RangeError(`crc must be an integer, got ${crc}`);
  const out = new Uint8Array(9);
  const v = new DataView(out.buffer);
  out[0] = op;
  v.setUint32(1, size, true);
  v.setUint32(5, crc >>> 0, true);
  return out;
}

function named(op, name, what) {
  const n = enc.encode(String(name ?? ''));
  if (n.length > 255) throw new RangeError(`${what} is ${n.length} bytes; a u8 length holds 255`);
  const out = new Uint8Array(2 + n.length);
  out[0] = op;
  out[1] = n.length;
  out.set(n, 2);
  return out;
}

/** The control commands, each as the bytes to write to 6e400011 (with response). */
export const cmd = Object.freeze({
  /** 0x01 BEGIN u32 size, u32 crc: a full OTA of an app image. */
  begin: (size, crc) => sized(CMD.BEGIN, size, crc),
  /** 0x04 BEGIN_FILE u32 size, u32 crc, u8 len, path: one file into the pet's LittleFS. */
  beginFile(size, crc, path) {
    const p = enc.encode(String(path));
    if (p.length > 255) throw new RangeError(`path is ${p.length} bytes; a u8 length holds 255`);
    const out = new Uint8Array(10 + p.length);
    out.set(sized(CMD.BEGIN_FILE, size, crc));
    out[9] = p.length;
    out.set(p, 10);
    return out;
  },
  /** 0x0B BEGIN_STREAM u32 size, u32 crc: a PSND clip the pet plays while it arrives. */
  beginStream: (size, crc) => sized(CMD.BEGIN_STREAM, size, crc),
  /** 0x02 END: every byte acked, finish the transfer. */
  end: () => Uint8Array.of(CMD.END),
  /** 0x03 ABORT: stop the transfer; the pet keeps what it had. */
  abort: () => Uint8Array.of(CMD.ABORT),
  /** 0x05 SELECT_PACK u8 len, name: show an animation pack that is on the pet. */
  selectPack: (name) => named(CMD.SELECT_PACK, name, 'pack name'),
  /** 0x06 RUN_APP u8 len, name: run apps/<name>.wasm; "" stops the app. */
  runApp: (name = '') => named(CMD.RUN_APP, name, 'app name'),
  /** 0x07 PROGRESS u8 pct: the host's overall %, drawn by the pet (clamped to 0..100). */
  progress: (pct) => Uint8Array.of(CMD.PROGRESS, clampInt(pct, 0, 100)),
  /** 0x08 PLAY u8 len, name: play sounds/<name>.ima. */
  play: (name) => named(CMD.PLAY, name, 'sound name'),
  /** 0x0A SELFTEST: a 1 kHz beep; a pet with a mic reports whether it heard it. */
  selftest: () => Uint8Array.of(CMD.SELFTEST),
  /** 0x0C LISTEN u16 ms (clamped to 100..10000), u8 flags: bit 0 = send the audio back. */
  listen(ms, sendAudio) {
    const m = clampInt(ms, 100, 10000);
    return Uint8Array.of(CMD.LISTEN, m & 0xff, m >> 8, sendAudio ? 1 : 0);
  },
});

/** Names the pet stores (packs, apps): lower-case a-z 0-9 - _, 1 to 23 characters. */
export const NAME_RULE = /^[a-z0-9_-]{1,23}$/;

/** The longest BEGIN_FILE path: ota.c keeps it in char s_path[96] and answers a longer one with an ATT error. */
export const PATH_MAX = 95;

/**
 * Why the pet would refuse a BEGIN_FILE path (ota.c safe_path), or null when it is fine:
 * "characters/<pack>/<file>", "apps/<name>.wasm" or "sounds/<name>.ima", lower-case
 * a-z 0-9 - _ . and no "..", at most PATH_MAX bytes.
 */
export function pathProblem(path) {
  const p = String(path ?? '');
  const n = enc.encode(p).length;
  if (n === 0) return 'empty path';
  if (n > PATH_MAX) return `path is ${n} bytes; the pet takes at most ${PATH_MAX}`;
  if (p.includes('..')) return 'path must not contain ".."';
  const m = /^(apps|sounds)\/(.*)$/.exec(p);
  if (m) {
    const ext = m[1] === 'apps' ? '.wasm' : '.ima';
    if (!new RegExp(`^[a-z0-9._-]{1,23}\\${ext}$`).test(m[2])) {
      return `${m[1]}/ takes <name>${ext}, the name 1 to 23 characters of a-z 0-9 - _ .`;
    }
    return null;
  }
  if (!p.startsWith('characters/')) return 'path must start with characters/, apps/ or sounds/';
  if (!/^characters\/[a-z0-9._-]+\/[a-z0-9._-]+$/.test(p)) return 'pack files are characters/<pack>/<file>, lower-case a-z 0-9 - _ .';
  return null;
}

// ---- PSND sound clips (IMA ADPCM) -----------------------------------------------------------

/** PSND layout (PROTOCOL.md 5). */
export const PSND = Object.freeze({ sampleRate: 16000, headerBytes: 16, blockBytes: 256, blockSamples: 504 });

/** The standard IMA ADPCM tables. */
export const IMA_STEP = Object.freeze([
  7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
  130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060,
  1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132,
  7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
]);
export const IMA_INDEX = Object.freeze([-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]);
const STEP = Int32Array.from(IMA_STEP);
const INDEX = Int32Array.from(IMA_INDEX);

/** Int16 samples from an Int16Array (as is) or any array of numbers (rounded, clamped). */
function toInt16(pcm) {
  if (pcm instanceof Int16Array) return pcm;
  const out = new Int16Array(pcm.length);
  for (let i = 0; i < pcm.length; i++) {
    const v = Math.round(Number(pcm[i])) || 0;
    out[i] = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
  }
  return out;
}

/** Float samples (WebAudio, -1..1) as int16 PCM. */
export function pcmFromFloat(samples) {
  const out = new Int16Array(samples.length);
  for (let i = 0; i < samples.length; i++) {
    const x = Math.max(-1, Math.min(1, samples[i] || 0));
    out[i] = Math.round(x * 32767);
  }
  return out;
}

/**
 * 16 kHz mono PCM -> a PSND clip. The encoder tracks its prediction with the decoder's own
 * arithmetic, so the pet plays exactly what decodePSND() returns (bit-exact with PetSound.swift
 * and audio.c play_block). The last block is padded to 256 bytes, as the Mac app writes it.
 */
export function encodePSND(pcm, sampleRate = PSND.sampleRate) {
  const s = toInt16(pcm);
  if (!Number.isInteger(sampleRate) || sampleRate <= 0 || sampleRate > 0xffffffff) throw new RangeError(`bad sample rate ${sampleRate}`);
  const blocks = Math.ceil(s.length / 504);
  const out = new Uint8Array(16 + blocks * 256);
  const v = new DataView(out.buffer);
  out.set([0x50, 0x53, 0x4e, 0x44, 1, 1, 0, 0]); // "PSND", version 1, codec 1 (IMA ADPCM), u16 0
  v.setUint32(8, sampleRate, true);
  v.setUint32(12, s.length, true);
  let pred = 0;
  let index = 0;
  for (let blk = 0; blk < blocks; blk++) {
    const o = 16 + blk * 256;
    out[o] = pred & 0xff; // the state before the block's first sample
    out[o + 1] = (pred >> 8) & 0xff;
    out[o + 2] = index;
    const start = blk * 504;
    const n = Math.min(504, s.length - start);
    for (let k = 0; k < n; k++) {
      let diff = s[start + k] - pred;
      let nib = 0;
      if (diff < 0) {
        nib = 8;
        diff = -diff;
      }
      let t = STEP[index];
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
      // the decoder step, so the prediction stays the pet's
      const st = STEP[index];
      let d = st >> 3;
      if (nib & 1) d += st >> 2;
      if (nib & 2) d += st >> 1;
      if (nib & 4) d += st;
      pred += nib & 8 ? -d : d;
      pred = pred > 32767 ? 32767 : pred < -32768 ? -32768 : pred;
      index += INDEX[nib];
      index = index < 0 ? 0 : index > 88 ? 88 : index;
      out[o + 4 + (k >> 1)] |= nib << ((k & 1) * 4); // low nibble first
    }
  }
  return out;
}

/** The PSND header: { version, codec, sampleRate, samples }, or null when the bytes are not one. */
export function psndInfo(clip) {
  const b = toBytes(clip);
  if (b.length < 16 || b[0] !== 0x50 || b[1] !== 0x53 || b[2] !== 0x4e || b[3] !== 0x44) return null;
  return { version: b[4], codec: b[5], sampleRate: u32le(b, 8), samples: u32le(b, 12) };
}

/** Bytes a PSND clip of `samples` needs to decode in full (a short last block counted short). */
export function psndLength(samples) {
  const rest = samples % 504;
  return 16 + Math.floor(samples / 504) * 256 + (rest ? 4 + Math.ceil(rest / 2) : 0);
}

/**
 * A PSND clip -> Int16Array, or null when it is not a v1 IMA clip. Every block restarts from its
 * own header; a cut-off clip decodes as far as its bytes go (PetSound.decode).
 */
export function decodePSND(clip) {
  const b = toBytes(clip);
  const h = psndInfo(b);
  if (!h || h.version !== 1 || h.codec !== 1) return null;
  const blocksThere = b.length >= 20 ? Math.floor((b.length - 20) / 256) + 1 : 0;
  const out = new Int16Array(Math.min(h.samples, blocksThere * 504));
  let n = 0;
  for (let off = 16; n < out.length && off + 4 <= b.length; off += 256) {
    let pred = ((b[off] | (b[off + 1] << 8)) << 16) >> 16;
    let index = Math.min(88, b[off + 2]);
    const m = Math.min(504, out.length - n);
    for (let k = 0; k < m; k++) {
      const at = off + 4 + (k >> 1);
      if (at >= b.length) return out.slice(0, n);
      const nib = (b[at] >> ((k & 1) * 4)) & 0x0f;
      const st = STEP[index];
      let d = st >> 3;
      if (nib & 1) d += st >> 2;
      if (nib & 2) d += st >> 1;
      if (nib & 4) d += st;
      pred += nib & 8 ? -d : d;
      pred = pred > 32767 ? 32767 : pred < -32768 ? -32768 : pred;
      index += INDEX[nib];
      index = index < 0 ? 0 : index > 88 ? 88 : index;
      out[n++] = pred;
    }
  }
  return n === out.length ? out : out.slice(0, n);
}

const roundHalfAway = (x) => (x < 0 ? -Math.round(-x) : Math.round(x)); // Swift's .rounded()

/**
 * Speech prep (PROTOCOL.md 5, PetSound.tidy): trim quiet ends (|s| <= 300) keeping 20 ms around
 * the sound, cap the length, normalise the peak to `peak` of full scale. Silence -> empty.
 */
export function tidy(pcm, { peak = 0.9, maxSeconds = 6, sampleRate = PSND.sampleRate } = {}) {
  const s = toInt16(pcm);
  const loud = (v) => v > 300 || v < -300;
  let first = 0;
  while (first < s.length && !loud(s[first])) first++;
  if (first === s.length) return new Int16Array(0);
  let last = s.length - 1;
  while (!loud(s[last])) last--;
  const pad = Math.floor(sampleRate / 50);
  const a = Math.max(0, first - pad);
  let z = Math.min(s.length - 1, last + pad) + 1;
  z = Math.min(z, a + Math.max(0, Math.trunc(maxSeconds * sampleRate)));
  let m = 0;
  for (let i = a; i < z; i++) m = Math.max(m, Math.abs(s[i]));
  const g = (32767 * peak) / Math.max(m, 1);
  const out = new Int16Array(z - a);
  for (let i = 0; i < out.length; i++) {
    const v = roundHalfAway(s[a + i] * g);
    out[i] = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
  }
  return out;
}

/** Whether `text` wants the Thai voice (it holds U+0E00..U+0E7F). */
export const isThai = (text) => /[฀-๿]/.test(String(text));

/** 16-bit mono WAV, for saving what the pet's mic heard (PetSound.wav). */
export function wav(pcm, sampleRate = PSND.sampleRate) {
  const s = toInt16(pcm);
  const out = new Uint8Array(44 + s.length * 2);
  const v = new DataView(out.buffer);
  out.set(enc.encode('RIFF'), 0);
  v.setUint32(4, 36 + s.length * 2, true);
  out.set(enc.encode('WAVEfmt '), 8);
  v.setUint32(16, 16, true);
  v.setUint16(20, 1, true); // PCM
  v.setUint16(22, 1, true); // mono
  v.setUint32(24, sampleRate, true);
  v.setUint32(28, sampleRate * 2, true);
  v.setUint16(32, 2, true);
  v.setUint16(34, 16, true);
  out.set(enc.encode('data'), 36);
  v.setUint32(40, s.length * 2, true);
  for (let i = 0; i < s.length; i++) v.setInt16(44 + 2 * i, s[i], true);
  return out;
}

/**
 * What LISTEN measures (PROTOCOL.md 4.6), computed on the host: rms and peak, the strongest
 * 50 Hz Goertzel bin from 150 to 4000 Hz, how far it stands above the median bin (dB), and the
 * RMS of the quietest 100 ms. The pet reports its own numbers; this serves the simulated pet and
 * a recording whose "heard" line never arrived.
 */
export function micMetrics(pcm, sampleRate = PSND.sampleRate) {
  const s = toInt16(pcm);
  const n = s.length;
  if (n === 0) return { samples: 0, rms: 0, peak: 0, freq: 0, db: 0, floor: 0 };
  let sum = 0;
  let peak = 0;
  for (let i = 0; i < n; i++) {
    sum += s[i] * s[i];
    peak = Math.max(peak, Math.abs(s[i]));
  }
  const rms = Math.sqrt(sum / n);
  const win = Math.max(1, Math.round(sampleRate / 10));
  let floor = rms;
  if (n >= win) {
    floor = Infinity;
    for (let a = 0; a + win <= n; a += win) {
      let e = 0;
      for (let i = a; i < a + win; i++) e += s[i] * s[i];
      floor = Math.min(floor, Math.sqrt(e / win));
    }
  }
  const powers = [];
  let best = -1;
  let freq = 0;
  for (let f = 150; f <= 4000; f += 50) {
    const c = 2 * Math.cos((2 * Math.PI * f) / sampleRate);
    let s1 = 0;
    let s2 = 0;
    for (let i = 0; i < n; i++) {
      const s0 = s[i] + c * s1 - s2;
      s2 = s1;
      s1 = s0;
    }
    const p = (s1 * s1 + s2 * s2 - c * s1 * s2) / n; // per sample, as audio.c do_listen (the +1 below assumes it)
    powers.push(p);
    if (p > best) {
      best = p;
      freq = f;
    }
  }
  const median = powers.slice().sort((x, y) => x - y)[powers.length >> 1];
  const db = Math.round(100 * Math.log10((best + 1) / (median + 1))) / 10;
  return { samples: n, rms: Math.trunc(rms), peak, freq, db, floor: Math.trunc(floor) };
}

// ---- firmware images ------------------------------------------------------------------------

const DESC = 24 + 8; // esp_image_header_t + the first segment header: esp_app_desc_t starts here

function cString(b, off, len) {
  const end = Math.min(off + len, b.length);
  let z = off;
  while (z < end && b[z] !== 0) z++;
  return off < end ? dec.decode(b.subarray(off, z)) : '';
}

/** The project an ESP32 app image was built as, or null when it is not an app image (0xE9 + magic). */
export function projectName(image) {
  const b = toBytes(image);
  if (b.length <= DESC + 80 || b[0] !== 0xe9 || u32le(b, DESC) !== 0xabcd5432) return null;
  return cString(b, DESC + 48, 32);
}

/**
 * What an app image says about itself (esp_app_desc_t), in the shape the pet reports:
 * { project, board, version, built: "<date> <time>", sha: 16 hex of the ELF SHA-256 }, or null.
 */
export function imageInfo(image) {
  const b = toBytes(image);
  const project = projectName(b);
  if (project === null) return null;
  const sha = b.length >= DESC + 152 ? hex(b.subarray(DESC + 144, DESC + 152)) : '';
  return {
    project,
    board: boardName(project),
    version: cString(b, DESC + 16, 32),
    built: `${cString(b, DESC + 96, 16)} ${cString(b, DESC + 80, 16)}`,
    sha,
  };
}

/**
 * Why this image must not go to this pet (PetLink.updateFirmware), or null when it may:
 * not an app image, not a pet project, or the other board's firmware. `pet` is parseInfo()'s
 * result; an unknown project (older firmware) is not held against the image.
 */
export function firmwareProblem(image, pet) {
  const name = projectName(image);
  if (name === null) return 'Not an ESP32 app image';
  if (pet?.project === BASE_PROJECT) return null; // the OTA base installs any ESP32-S3 image
  if (!isPetProject(name)) return `Not a pet firmware: ${name}`;
  const mine = pet?.project;
  if (mine && mine !== name) return `That firmware is for the ${boardName(name)} board; this pet is ${boardName(mine)}`;
  return null;
}

/** The same build, by the common prefix of the SHAs when both have 8+ digits, else version + time. */
export function sameBuild(a, b) {
  const n = Math.min(a?.sha?.length ?? 0, b?.sha?.length ?? 0);
  return n >= 8 ? a.sha.slice(0, n) === b.sha.slice(0, n) : a?.version === b?.version && a?.built === b?.built;
}

// ---- heartbeat (NUS RX) ---------------------------------------------------------------------

/** The firmware copies msg into char[64] and draws who at 16 px a glyph across 320 px. */
export const MSG_LIMIT = 63;
export const WHO_LIMIT = 20;

/** The smallest counts that make the pet show each state (PetState.counts). */
export const STATE_COUNTS = Object.freeze({
  idle: Object.freeze({ total: 1, running: 0, waiting: 0 }),
  busy: Object.freeze({ total: 1, running: 1, waiting: 0 }),
  attention: Object.freeze({ total: 1, running: 0, waiting: 1 }),
  sleep: Object.freeze({ total: 0, running: 0, waiting: 0 }),
});

/** The pet's state rule: waiting > 0 attention, running > 0 busy, total == 0 sleep, else idle. */
export function petState({ total = 0, running = 0, waiting = 0 } = {}) {
  if (waiting > 0) return 'attention';
  if (running > 0) return 'busy';
  return total === 0 ? 'sleep' : 'idle';
}

let segmenter;
/** User-perceived characters (Swift's Character), so a clip never splits one. */
function graphemes(s) {
  if (segmenter === undefined) {
    segmenter = typeof Intl !== 'undefined' && Intl.Segmenter ? new Intl.Segmenter(undefined, { granularity: 'grapheme' }) : null;
  }
  return segmenter ? Array.from(segmenter.segment(s), (g) => g.segment) : Array.from(s);
}

/** Clip to `limit` UTF-8 bytes without splitting a character (Heartbeat.clip). */
export function clipUtf8(s, limit = MSG_LIMIT) {
  let out = '';
  let n = 0;
  for (const ch of graphemes(String(s ?? ''))) {
    const len = enc.encode(ch).length;
    if (n + len > limit) break;
    out += ch;
    n += len;
  }
  return out;
}

/** A JSON string the way Heartbeat.quote writes it (the golden line depends on it). */
function quote(s) {
  let out = '"';
  for (const ch of s) {
    const c = ch.codePointAt(0);
    if (ch === '"') out += '\\"';
    else if (ch === '\\') out += '\\\\';
    else if (ch === '\n') out += '\\n';
    else if (ch === '\t') out += '\\t';
    else if (c < 0x20) out += `\\u${c.toString(16).padStart(4, '0')}`;
    else out += ch;
  }
  return `${out}"`;
}

/** Local UTC offset in seconds (GMT+7 -> 25200), as the pet's clock expects. */
export const localTzoff = (date = new Date()) => -date.getTimezoneOffset() * 60 || 0;

/**
 * One heartbeat line for NUS RX (PROTOCOL.md 3.1), "\n" included, keys in the order the Mac
 * app writes them. epoch and tzoff default to this computer's clock; pct5h, reset5h, pct7d,
 * reset7d and who are left out unless given; msg is clipped to 63 bytes, who to 20 characters.
 */
export function heartbeatLine(hb = {}) {
  const int = (v) => (Number.isFinite(Number(v)) ? Math.trunc(Number(v)) || 0 : 0);
  const parts = [
    `"total":${int(hb.total)}`,
    `"running":${int(hb.running)}`,
    `"waiting":${int(hb.waiting)}`,
    `"msg":${quote(clipUtf8(hb.msg ?? '', MSG_LIMIT))}`,
    `"epoch":${int(hb.epoch ?? Date.now() / 1000)}`,
    `"tzoff":${int(hb.tzoff ?? localTzoff())}`,
  ];
  for (const k of ['pct5h', 'reset5h', 'pct7d', 'reset7d']) if (hb[k] != null) parts.push(`"${k}":${int(hb[k])}`);
  if (hb.who != null) parts.push(`"who":${quote(graphemes(String(hb.who)).slice(0, WHO_LIMIT).join(''))}`);
  return `{${parts.join(',')}}\n`;
}

export const heartbeatBytes = (hb) => enc.encode(heartbeatLine(hb));

// ---- what the pet says ----------------------------------------------------------------------

function jsonObject(data) {
  try {
    const o = JSON.parse(typeof data === 'string' ? data : dec.decode(toBytes(data)));
    return o && typeof o === 'object' && !Array.isArray(o) ? o : null;
  } catch {
    return null;
  }
}

const str = (v, d) => (typeof v === 'string' ? v : d);
const int = (v, d) => (Number.isInteger(v) ? v : d);
const num = (v, d) => (typeof v === 'number' && Number.isFinite(v) ? v : d);
const bool = (v, d) => (typeof v === 'boolean' ? v : d);
const strings = (v) => (Array.isArray(v) && v.every((x) => typeof x === 'string') ? v.slice().sort() : []);

function infoFrom(o) {
  if (!o || typeof o.ver !== 'string') return null;
  const project = str(o.project, '');
  return {
    version: o.ver,
    slot: str(o.slot, '?'),
    built: str(o.built, ''),
    sha: str(o.sha, ''), // a prefix of the ELF SHA-256 (9 hex digits from current firmware)
    project,
    board: project ? boardName(project) : '',
    state: str(o.state, ''), // "pending" until a new image proves itself, then "valid"
    pack: str(o.pack, ''),
    packs: strings(o.packs),
    freeKB: int(o.free, 0),
    app: str(o.app, ''),
    apps: strings(o.apps),
    appError: str(o.apperr, ''),
  };
}

/** What a read of the control characteristic says (PROTOCOL.md 4.1), or null. */
export const parseInfo = (data) => infoFrom(jsonObject(data));

function statusFrom(o) {
  if (!o || typeof o.ota !== 'string') return null;
  switch (o.ota) {
    case 'ready':
      return { kind: 'ready', size: int(o.size, 0), window: int(o.window, 4096) };
    case 'ack':
      return Number.isInteger(o.off) ? { kind: 'ack', off: o.off } : null;
    case 'done':
      return { kind: 'done' };
    case 'saved':
      return { kind: 'saved', path: str(o.path, '') };
    case 'pack':
    case 'app':
      return { kind: o.ota, name: str(o.name, '') };
    case 'play':
      return { kind: 'play', name: str(o.name, ''), bytes: int(o.bytes, 0) };
    case 'streamed':
      return { kind: 'streamed', bytes: int(o.bytes, 0), crcOK: bool(o.crc, false) };
    case 'heard':
      return {
        kind: 'heard',
        ms: int(o.ms, 0),
        samples: int(o.samples, 0),
        rms: int(o.rms, 0),
        peak: int(o.peak, 0),
        freq: int(o.freq, 0),
        db: num(o.db, 0),
        floor: int(o.floor, 0),
        sent: bool(o.sent, false),
      };
    case 'selftest':
      return {
        kind: 'selftest',
        mic: bool(o.mic, false),
        heard: bool(o.heard, false),
        db: num(o.db, 0),
        quiet: int(o.quiet, 0),
        tone: int(o.tone, 0),
      };
    case 'error':
      return { kind: 'error', why: str(o.why, 'unknown error') };
    default:
      return null;
  }
}

/** One status line the pet notifies on control (PROTOCOL.md 4.3) as { kind, …fields }, or null. */
export const parseStatus = (data) => statusFrom(jsonObject(data));

/** One NUS TX line ({"evt":"tap"}, {"evt":"say","text":…}), or null. */
export function parseEvent(line) {
  const o = jsonObject(line);
  return o && typeof o.evt === 'string' ? o : null;
}

/** Reassembles "\n"-terminated lines across notifications; drops a line that grows past `limit`. */
export class LineBuffer {
  constructor(limit = 4096) {
    this.limit = limit;
    this.buf = new Uint8Array(0);
  }

  /** Feed bytes, get back the lines they completed (without the "\n"). */
  push(bytes) {
    const all = concat([this.buf, toBytes(bytes)]);
    const lines = [];
    let start = 0;
    for (let i = 0; i < all.length; i++) {
      if (all[i] === 0x0a) {
        lines.push(dec.decode(all.subarray(start, i)));
        start = i + 1;
      }
    }
    this.buf = all.slice(start);
    if (this.buf.length > this.limit) this.buf = new Uint8Array(0); // a peer that never ends its line
    return lines;
  }
}

const signed = (db) => `${db < 0 ? '-' : '+'}${Math.abs(db).toFixed(1)}`;

/** The speaker self-test result as the Mac app words it. */
export function describeSelftest({ mic, heard, db = 0, quiet = 0, tone = 0 }) {
  if (!mic) return 'Beeped (this pet has no mic to check itself)';
  return heard
    ? `Heard by its mic: ${signed(db)} dB at 1 kHz (rms ${tone} vs ${quiet} quiet)`
    : `Its mic heard nothing (${signed(db)} dB): speaker or amp off`;
}

/** A LISTEN result in one line (MicLevels.summary). */
export const describeHeard = ({ rms = 0, floor = 0, peak = 0, freq = 0, db = 0 }) =>
  `rms ${rms} (floor ${floor}), peak ${peak}, strongest ${freq} Hz ${signed(db)} dB`;

// ---- picking packs and apps -----------------------------------------------------------------

/**
 * Animation packs in what a folder picker (<input webkitdirectory>) returned: every folder that
 * holds a manifest.json, with its .gif and .json files (PackLibrary.read). Pick one pack folder,
 * or a folder of packs such as lab/buddy/characters. Each pack lists `problems` that would make
 * the pet refuse it.
 */
export function packsFromFiles(files) {
  const dirs = new Map();
  for (const file of Array.from(files ?? [])) {
    const rel = String(file.webkitRelativePath || file.relativePath || file.name || '').replaceAll('\\', '/');
    const cut = rel.lastIndexOf('/');
    const dir = cut >= 0 ? rel.slice(0, cut) : '';
    if (!dirs.has(dir)) dirs.set(dir, []);
    dirs.get(dir).push({ name: rel.slice(cut + 1), file });
  }
  const packs = [];
  for (const [dir, entries] of dirs) {
    if (!entries.some((e) => e.name === 'manifest.json')) continue;
    const name = (dir.split('/').pop() || '').toLowerCase();
    const keep = entries.filter((e) => /\.(gif|json)$/.test(e.name)).sort((x, y) => (x.name < y.name ? -1 : x.name > y.name ? 1 : 0));
    const problems = [];
    if (!NAME_RULE.test(name)) problems.push(`pack name "${name || '(none)'}" must be 1 to 23 characters of a-z 0-9 - _`);
    for (const e of keep) {
      const p = pathProblem(`characters/${name}/${e.name}`);
      if (p && NAME_RULE.test(name)) problems.push(`${e.name}: ${p}`);
    }
    const bytes = keep.reduce((n, e) => n + (e.file.size ?? e.file.data?.length ?? 0), 0);
    packs.push({ name, dir, files: keep, bytes, problems });
  }
  return packs.sort((x, y) => (x.name < y.name ? -1 : x.name > y.name ? 1 : 0));
}

// =============================================================================================
// 2. Pet
// =============================================================================================

/** A transfer path that means BEGIN_STREAM (the Mac app's "@stream"); null means an app image. */
export const STREAM = '@stream';

const LINK_LOST = 'Link lost during the update; the pet keeps what it had';

/** An error the pet or the link caused. `why` holds the pet's own words when it sent some. */
export class PetError extends Error {
  constructor(message, { why = '', cause } = {}) {
    super(message, cause ? { cause } : undefined);
    this.name = 'PetError';
    this.why = why;
  }
}

const now = () => globalThis.performance?.now?.() ?? Date.now();
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

function within(promise, ms, message, onTimeout) {
  let timer;
  const late = new Promise((_, reject) => {
    timer = setTimeout(() => {
      onTimeout?.();
      reject(new PetError(message));
    }, ms);
  });
  return Promise.race([promise, late]).finally(() => clearTimeout(timer));
}

/** Pet options; every wait is a field so tests can run the engine fast. */
export const PET_DEFAULTS = Object.freeze({
  stallMs: 20000, // no word from the pet for this long: abort (PetLink.otaCheckStall)
  rebootMs: 60000, // after "done" the pet must be back within this
  watchdogMs: 2000, // how often the watchdog looks
  settleMs: 500, // pause between queued jobs, as the Mac app
  reconnectMs: 60000, // keep trying this long after an unexpected disconnect (0: never)
  reconnectEveryMs: 2000,
  infoEveryMs: 20000, // re-read the pet's info while idle, to see pending -> valid (0: never)
  selftestMs: 15000, // SELFTEST must answer within this
  listenGraceMs: 15000, // LISTEN must answer within its ms plus this
  micQuietMs: 3000, // LISTEN audio is taken as finished after this long without data
  heardGraceMs: 2000, // complete audio but no "heard" line after this long: measure it here
});

/** Collects a LISTEN's PSND stream from the mic notifications and knows when it is whole. */
class MicStream {
  constructor() {
    this.chunks = [];
    this.length = 0;
    this.need = Infinity;
    this.lastAt = 0;
    this.whole = new Promise((resolve) => {
      this._whole = resolve;
    });
  }

  push(bytes) {
    const b = toBytes(bytes).slice();
    this.chunks.push(b);
    this.length += b.length;
    this.lastAt = now();
    if (this.need === Infinity && this.length >= 16) {
      const h = psndInfo(this.bytes().subarray(0, 16));
      if (h) this.need = psndLength(h.samples);
    }
    if (this.complete) this._whole();
  }

  get complete() {
    return this.length >= this.need;
  }

  bytes() {
    return concat(this.chunks, this.length);
  }

  /** Wait until the stream is whole, or nothing has come for `quietMs`. */
  async settle(quietMs) {
    const start = now();
    while (!this.complete && now() - Math.max(start, this.lastAt) < quietMs) await sleep(Math.min(50, quietMs));
  }
}

/** Flow control for write-without-response: never more than `window` bytes beyond the last ack. */
export class TransferWindow {
  constructor(total, window) {
    this.total = total;
    this.window = window;
    this.sent = 0;
    this.acked = 0;
  }

  /** The next [start, end) to send, at most maxLen long, or null when the window is full or all is sent. */
  next(maxLen) {
    if (this.sent >= this.total || this.sent - this.acked >= this.window || maxLen <= 0) return null;
    const n = Math.min(maxLen, this.total - this.sent, this.window - (this.sent - this.acked));
    const range = [this.sent, this.sent + n];
    this.sent += n;
    return range;
  }

  ack(off) {
    this.acked = Math.max(this.acked, Math.min(off, this.sent));
  }

  get allAcked() {
    return this.acked === this.total;
  }

  get fraction() {
    return this.total === 0 ? 0 : this.acked / this.total;
  }
}

/**
 * One desk pet. Events (each a CustomEvent; pet.on(type, fn) passes fn the detail):
 *   connect {name, reconnected}   disconnect {expected}   info <parseInfo result>
 *   status <parseStatus result>   progress <snapshot>     job {state, label, message}
 *   tap {}   say {text}   event <any NUS TX event>   log <string>
 * Jobs (ota, pack, app, run, stop, select, play, stream, selftest, listen) run one at a time
 * in the order asked and return a promise of { message, … }; a failure rejects with a PetError.
 */
export class Pet extends EventTarget {
  /** link: a WebBluetoothLink, or anything with its shape (see simpet.js). */
  constructor(link, options = {}) {
    super();
    this.link = link;
    this.opts = { ...PET_DEFAULTS, ...options };
    this.info = null;
    this._infoText = '';
    this._wanted = false;
    this._reconnecting = false;
    this._queue = [];
    this._job = null;
    this._jobError = null;
    this._waiter = null;
    this._running = false;
    this._batch = { total: 0, done: 0, index: 0, count: 0 };
    this._phase = 'idle';
    this._win = null;
    this._lastPct = -1;
    this._lastProgress = now();
    this._rebootDropped = false;
    this._lines = new LineBuffer();
    this._mic = null;
    link.onControl = (b) => this._onControl(b);
    link.onTx = (b) => this._onTx(b);
    link.onMic = (b) => this._mic?.push(b);
    link.onDisconnect = () => this._onDisconnect();
  }

  /**
   * Ask the browser for a pet (it shows its Bluetooth chooser; call this from a click).
   * Options: chunkSize (bytes per write, default 180), plus any PET_DEFAULTS field.
   */
  static async request(options = {}) {
    const bluetooth = options.bluetooth ?? globalThis.navigator?.bluetooth;
    if (!bluetooth) throw new PetError('This browser has no Web Bluetooth: use Chrome or Edge, over https or on localhost');
    const device = await bluetooth.requestDevice({
      filters: [{ namePrefix: NAME_PREFIX }],
      optionalServices: [UUID.nusService, UUID.otaService],
    });
    return new Pet(new WebBluetoothLink(device, options), options);
  }

  get name() {
    return this.link.name;
  }

  get connected() {
    return this.link.connected;
  }

  /** A job is running (others may be queued behind it). */
  get busy() {
    return this._running;
  }

  get queued() {
    return this._queue.length;
  }

  /** idle | starting | sending | finishing | rebooting */
  get phase() {
    return this._phase;
  }

  /** Listen for an event with its detail; returns a function that stops listening. */
  on(type, fn) {
    const handler = (e) => fn(e.detail);
    this.addEventListener(type, handler);
    return () => this.removeEventListener(type, handler);
  }

  /** Connect (again), subscribe, and read what the pet runs. Resolves with the info (null without OTA). */
  async connect() {
    this._wanted = true;
    if (!this.link.connected) {
      await this.link.connect();
      this._log(`Connected to ${this.name}`);
      this._emit('connect', { name: this.name, reconnected: false });
    }
    this._startInfoTimer();
    if (this.link.hasOTA) await this.readInfo().catch((e) => this._log(`Could not read the pet's info: ${e.message}`));
    return this.info;
  }

  /** Let go of the pet so another host can connect. A running job is cancelled. */
  async disconnect() {
    this._wanted = false;
    this._stopInfoTimer();
    this.cancel('Disconnected');
    await this.link.disconnect();
  }

  /** Read the control characteristic: version, board, slot, packs, apps (PROTOCOL.md 4.1). */
  async readInfo() {
    if (!this.link.hasOTA) return null;
    const bytes = await this.link.readControl();
    const text = dec.decode(toBytes(bytes));
    const info = infoFrom(jsonObject(text));
    if (info) this._setInfo(info, text);
    return info;
  }

  // ---- jobs ---------------------------------------------------------------------------------

  // Each call takes its place in the queue at once, so jobs run in the order asked even when
  // one needs longer to read its files; `prepare` reads and checks them while it waits.

  /** Full OTA: send an app image (.bin); the pet checks it, reboots into it, and is back. */
  async ota(image) {
    this._needOTA();
    return this._enqueue('firmware', async () => {
      const data = await readBytes(image);
      const problem = firmwareProblem(data, this.info);
      if (problem) throw new PetError(problem);
      const img = imageInfo(data);
      // To the base, a non-pet image is the end of the story: the board boots it and may
      // never come back as a pet, so "done" is the success, not the reconnect.
      const foreign = this.info?.project === BASE_PROJECT && !isPetProject(img.project);
      return { type: 'transfer', label: `firmware ${img.version} for the ${img.board}`, files: [{ path: null, data }], foreign };
    });
  }

  /**
   * Partial OTA of an animation pack, then show it. `input`: the FileList of a folder picker
   * (one pack folder, or a folder of packs plus {name}), a pack from packsFromFiles(), or
   * { name, files: [{ name, data }] }. manifest.json goes last: the pet skips a pack without one.
   */
  async pack(input, { name } = {}) {
    this._needOTA();
    let pack = input;
    if (!(input && typeof input === 'object' && typeof input.name === 'string' && Array.isArray(input.files))) {
      const found = packsFromFiles(input);
      pack = name ? found.find((p) => p.name === name) : found.length === 1 ? found[0] : null;
      if (!pack) {
        throw new PetError(
          found.length
            ? `That folder holds ${found.length} packs (${found.map((p) => p.name).join(', ')}); pass {name}`
            : 'No pack there: a pack is a folder with manifest.json and GIFs',
        );
      }
    }
    const problems = [...(pack.problems ?? [])];
    if (!NAME_RULE.test(pack.name)) problems.push(`pack name "${pack.name}" must be 1 to 23 characters of a-z 0-9 - _`);
    for (const f of pack.files) {
      const p = pathProblem(`characters/${pack.name}/${f.name}`);
      if (p) problems.push(`${f.name}: ${p}`);
    }
    if (!pack.files.some((f) => f.name === 'manifest.json')) problems.push('no manifest.json');
    if (problems.length) throw new PetError(`Pack ${pack.name} cannot go to the pet: ${[...new Set(problems)].join('; ')}`);
    return this._enqueue(`pack ${pack.name}`, async () => {
      const files = [];
      for (const f of pack.files) files.push({ path: `characters/${pack.name}/${f.name}`, data: await readBytes(f.data ?? f.file ?? f) });
      const last = (f) => f.path.endsWith('/manifest.json');
      return { type: 'transfer', files: [...files.filter((f) => !last(f)), ...files.filter(last)], select: pack.name };
    });
  }

  /** Partial OTA of a pet app (.wasm from lab/pet-sdk) into apps/; run it when `run`. */
  async app(file, run = true, { name } = {}) {
    this._needOTA();
    const appName = name ?? String(file?.name ?? '').replace(/\.wasm$/i, '').toLowerCase();
    if (!NAME_RULE.test(appName)) throw new PetError(`App name "${appName}" must be 1 to 23 characters of a-z 0-9 - _ (rename the file)`);
    return this._enqueue(`app ${appName}`, async () => {
      const data = await readBytes(file?.data ?? file);
      if (!(data[0] === 0x00 && data[1] === 0x61 && data[2] === 0x73 && data[3] === 0x6d)) throw new PetError(`${appName} is not a WebAssembly module`);
      return { type: 'transfer', files: [{ path: `apps/${appName}.wasm`, data }], run: run ? appName : undefined };
    });
  }

  /** Run an app that is on the pet. */
  async run(name) {
    this._needOTA();
    if (!NAME_RULE.test(name)) throw new PetError(`No such app name: "${name}"`);
    return this._enqueue(`run ${name}`, () => ({ type: 'transfer', files: [], run: name }));
  }

  /** Stop the running app. */
  async stop() {
    this._needOTA();
    return this._enqueue('stop the app', () => ({ type: 'transfer', files: [], run: '' }));
  }

  /** Show a pack that is on the pet. */
  async select(name) {
    this._needOTA();
    if (!NAME_RULE.test(name)) throw new PetError(`No such pack name: "${name}"`);
    return this._enqueue(`show ${name}`, () => ({ type: 'transfer', files: [], select: name }));
  }

  /** Play sounds/<name>.ima from the pet's storage. */
  async play(name) {
    this._needOTA();
    if (!/^[a-z0-9._-]{1,23}$/.test(name)) throw new PetError(`No such sound name: "${name}"`);
    return this._enqueue(`play ${name}`, () => ({ type: 'transfer', files: [], play: name }));
  }

  /**
   * Stream a clip to the speaker; the pet plays it while it arrives (BEGIN_STREAM). `clip` is a
   * PSND clip (bytes) or 16 kHz Int16Array PCM, encoded as is (tidy() it first for speech).
   */
  async stream(clip, { text = '' } = {}) {
    this._needOTA();
    return this._enqueue(text ? `say "${text}"` : 'sound', async () => {
      const data = clip instanceof Int16Array ? encodePSND(clip) : await readBytes(clip);
      const h = psndInfo(data);
      if (!h || h.version !== 1 || h.codec !== 1) throw new PetError('Not a PSND clip (encodePSND makes one)');
      if (h.sampleRate !== 16000 || h.samples > 16000 * 30) throw new PetError('The pet plays 16 kHz clips of up to 30 s');
      return { type: 'transfer', files: [{ path: STREAM, data }], saying: text };
    });
  }

  /** Speaker self-test: the pet beeps; a pet with a mic says whether it heard it. */
  async selftest() {
    this._needOTA();
    return this._enqueue('speaker test', () => ({ type: 'control', run: () => this._runSelftest() }));
  }

  /**
   * LISTEN: the pet records its mic for `ms` (100..10000) and measures it; with sendAudio the
   * recording comes back too. Resolves with { ms, samples, rms, peak, freq, db, floor, sent,
   * pcm: Int16Array|null, clip, sampleRate, estimated, message }.
   */
  async listen(ms = 3000, sendAudio = true) {
    this._needOTA();
    return this._enqueue(`listen ${clampInt(ms, 100, 10000)} ms`, () => ({ type: 'control', run: () => this._runListen(ms, sendAudio) }));
  }

  /** Send one heartbeat line (PROTOCOL.md 3.1); see heartbeatLine() for the fields. */
  async heartbeat(hb = {}) {
    if (!this.link.connected) throw new PetError('Not connected to a pet');
    const bytes = heartbeatBytes(hb);
    for (let at = 0; at < bytes.length; at += this.link.maxWrite) await this.link.writeRx(bytes.subarray(at, at + this.link.maxWrite));
    return dec.decode(bytes);
  }

  /** Drop the queued jobs and stop the running one (ABORT when it is a transfer). */
  cancel(reason = 'Update cancelled') {
    for (const job of this._queue.splice(0)) this._drop(job, new PetError('Cancelled'));
    const job = this._job;
    if (!job || this._jobError) return;
    if (job.type === 'transfer' && this.link.connected && this.link.hasOTA) this.link.writeControl(cmd.abort()).catch(() => {});
    this._fail(new PetError(reason));
  }

  // ---- the queue ----------------------------------------------------------------------------

  _needOTA() {
    if (!this.link.connected && !this._reconnecting) throw new PetError('Not connected to a pet');
    if (this.link.connected && !this.link.hasOTA) throw new PetError("The pet's firmware has no OTA service yet: flash it once over USB");
  }

  /** Queue a job now; `prepare` returns its { type, files, select, run, play, … } meanwhile. */
  _enqueue(label, prepare) {
    let job;
    const settled = new Promise((resolve, reject) => {
      job = { label, type: 'transfer', files: [], bytes: 0, counted: false, resolve, reject };
    });
    job.ready = Promise.resolve()
      .then(prepare)
      .then((spec) => {
        Object.assign(job, spec);
        job.bytes = job.files.reduce((n, f) => n + f.data.length, 0);
        if (this._running && this._queue.includes(job)) this._count(job);
      });
    job.ready.catch((e) => {
      const i = this._queue.indexOf(job); // a bad file fails at once, not when its turn comes
      if (i >= 0) {
        this._queue.splice(i, 1);
        this._drop(job, e);
      }
    });
    this._queue.push(job);
    if (this._running) {
      this._batch.count += 1; // a request while busy joins the batch: one overall %
      this._log(`Queued ${label} (${this._queue.length} waiting)`);
    } else {
      this._runQueue();
    }
    return settled;
  }

  _count(job) {
    if (job.counted) return;
    job.counted = true;
    this._batch.total += job.bytes;
  }

  /** A queued job that will not run. */
  _drop(job, err) {
    this._batch.count -= 1;
    if (job.counted) this._batch.total -= job.bytes;
    const e = err instanceof PetError ? err : new PetError(err?.message ?? String(err), { cause: err });
    this._log(`Not sent: ${job.label}: ${e.message}`);
    e.logged = true; // a page that logs what a call rejects with can skip this one
    this._emit('job', { state: 'failed', label: job.label, message: e.message });
    job.reject(e);
  }

  async _runQueue() {
    this._running = true;
    this._batch = { total: 0, done: 0, index: 0, count: this._queue.length };
    this._lastPct = -1;
    while (this._queue.length) {
      const job = this._queue.shift();
      try {
        await job.ready;
      } catch (e) {
        this._drop(job, e);
        continue;
      }
      this._count(job);
      this._batch.index += 1;
      try {
        await this._linkUp();
        job.resolve(await this._runJob(job));
      } catch (e) {
        const err = e instanceof PetError ? e : new PetError(e?.message ?? String(e), { cause: e });
        this._log(`Failed: ${job.label}: ${err.message}`);
        err.logged = true;
        this._emit('job', { state: 'failed', label: job.label, message: err.message });
        job.reject(err);
      } finally {
        this._batch.done += job.bytes; // the job counts as done for the batch bar, as in the Mac app
        this._job = null;
        this._waiter = null;
        this._win = null;
        this._mic = null;
        this._phase = 'idle';
      }
      if (this._queue.length && this.opts.settleMs > 0) await sleep(this.opts.settleMs);
    }
    this._running = false;
    this._emitProgress();
  }

  async _linkUp() {
    if (this.link.connected) return;
    const until = now() + this.opts.reconnectMs;
    while (!this.link.connected && this._reconnecting && now() < until) await sleep(50);
    if (!this.link.connected) throw new PetError('Not connected to a pet');
  }

  async _runJob(job) {
    this._job = job;
    this._jobError = null;
    this._waiter = null;
    job.startedAt = now();
    this._log(`Start: ${job.label}`);
    this._emit('job', { state: 'start', label: job.label, job: this._batch.index, jobs: this._batch.count });
    const result = job.type === 'transfer' ? await this._runTransfer(job) : await job.run(job);
    this._log(result.message);
    this._emit('job', { state: 'done', label: job.label, message: result.message });
    return result;
  }

  // ---- waiting on the pet -------------------------------------------------------------------

  /** A promise of the next status that `match` accepts; rejected when the job fails. */
  _until(match) {
    if (this._jobError) return Promise.reject(this._jobError);
    return new Promise((resolve, reject) => {
      this._waiter = { match, resolve, reject };
    });
  }

  _deliver(st) {
    const w = this._waiter;
    if (w && w.match(st)) {
      this._waiter = null;
      w.resolve(st);
    }
  }

  /** End the running job with `err` (an error line, the watchdog, a lost link, a cancel). */
  _fail(err) {
    if (!this._job || this._jobError) return;
    this._jobError = err;
    const w = this._waiter;
    this._waiter = null;
    w?.reject(err);
  }

  _check() {
    if (this._jobError) throw this._jobError;
  }

  _writeError(e, what) {
    this._check();
    if (!this.link.connected) return new PetError(this._job?.type === 'transfer' ? LINK_LOST : 'Link lost', { cause: e });
    return new PetError(`${what} (${e?.name ?? 'Error'}: ${e?.message ?? e})`, { cause: e });
  }

  /**
   * Write a command and wait for the status it causes. The waiter is set before the write:
   * the pet may notify before its write response arrives (PLAY and the refusals do).
   */
  async _ask(bytes, match) {
    const reply = this._until(match);
    const mine = this._waiter;
    reply.catch(() => {}); // awaited below; this keeps an early rejection from being unhandled
    try {
      await this.link.writeControl(bytes);
    } catch (e) {
      if (this._waiter === mine) this._waiter = null;
      throw this._writeError(e, `The pet did not take command 0x${bytes[0].toString(16).padStart(2, '0')}: its firmware may be older than this command`);
    }
    return reply;
  }

  async _writeData(bytes) {
    try {
      await this.link.writeData(bytes);
    } catch (e) {
      throw this._writeError(e, 'Could not write to the pet');
    }
  }

  _touch() {
    this._lastProgress = now();
  }

  // ---- transfers (PetLink.startJob / otaNext / otaReceived) ---------------------------------

  async _runTransfer(job) {
    const t0 = now();
    const secs = () => Math.round((now() - t0) / 1000);
    const kb = () => Math.floor(job.bytes / 1024);
    job.acked = 0;
    job.bytesDone = 0;
    job.part = 0;
    job.fileIndex = 0;
    job.path = undefined;
    this._from = this.info;
    this._phase = 'starting';
    this._touch();
    const dog = setInterval(() => this._checkStall(), this.opts.watchdogMs);
    try {
      for (const f of job.files) {
        job.fileIndex += 1;
        const end = await this._sendFile(job, f);
        job.bytesDone += f.data.length;
        if (f.path === null && job.foreign) {
          return { message: `Sent in ${secs()} s: the board is restarting into ${imageInfo(f.data)?.project ?? 'the new firmware'}`, bytes: job.bytes, seconds: secs() };
        }
        if (f.path === null) {
          // full OTA: the result is the pet coming back on the other slot
          const back = end.kind === 'rebooted' ? end : await this._until((st) => st.kind === 'rebooted');
          const { info } = back;
          if (this._from && this._from.slot === info.slot) throw new PetError(`Pet came back on ${info.slot} again: the new image was rolled back`);
          return { message: `Updated in ${secs()} s: now ${info.version} on ${info.slot}`, info, bytes: job.bytes, seconds: secs() };
        }
        if (f.path === STREAM) {
          const first = job.firstAckAt ? ((job.firstAckAt - t0) / 1000).toFixed(2) : '?';
          const what = job.saying ? `Pet says “${job.saying}” · streamed` : 'Streamed';
          return {
            message: `${what} ${kb()} KB, sound after ${first} s${end.crcOK ? '' : ' (CRC mismatch)'}`,
            crcOK: end.crcOK,
            bytes: job.bytes,
            seconds: secs(),
          };
        }
      }
      if (job.select != null) {
        this._phase = 'finishing';
        this._emitProgress();
        const st = await this._ask(cmd.selectPack(job.select), (s) => s.kind === 'pack');
        this.readInfo().catch(() => {}); // refresh the pack list
        if (job.run == null && job.play == null) {
          return { message: `Pet now shows ${st.name} · ${kb()} KB in ${secs()} s`, bytes: job.bytes, seconds: secs() };
        }
      }
      if (job.run != null) {
        this._phase = 'finishing';
        this._emitProgress();
        const st = await this._ask(cmd.runApp(job.run), (s) => s.kind === 'app');
        this.readInfo().catch(() => {});
        if (job.play == null) {
          const sent = job.bytes > 0 ? ` · ${job.bytes} bytes in ${secs()} s` : '';
          return { message: st.name ? `Pet runs ${st.name}${sent}` : 'App stopped', bytes: job.bytes, seconds: secs() };
        }
      }
      if (job.play != null) {
        this._phase = 'finishing';
        this._emitProgress();
        const st = await this._ask(cmd.play(job.play), (s) => s.kind === 'play');
        const message = job.saying ? `Pet says “${job.saying}” · ${kb()} KB in ${secs()} s` : `Pet plays ${st.name}`;
        return { message, bytes: job.bytes, seconds: secs() };
      }
      return { message: `Sent ${kb()} KB in ${secs()} s`, bytes: job.bytes, seconds: secs() };
    } catch (e) {
      // an error line fails the job, but a later queued job still runs (PetLink .error)
      if (e instanceof PetError && e.why) throw new PetError(`Pet refused the update: ${e.why}`, { why: e.why, cause: e });
      throw e;
    } finally {
      clearInterval(dog);
    }
  }

  /** One file: BEGIN, wait for ready, send inside the window, END, wait for the outcome. */
  async _sendFile(job, { path, data }) {
    const crc = crc32(data);
    const begin =
      path === null ? cmd.begin(data.length, crc) : path === STREAM ? cmd.beginStream(data.length, crc) : cmd.beginFile(data.length, crc, path);
    job.path = path;
    job.part = 0;
    job.endSent = false;
    this._win = null;
    this._phase = 'starting';
    this._touch();
    this._emitProgress();
    this._log(`→ ${path === null ? 'app image' : path === STREAM ? 'stream' : path}, ${data.length} bytes`);
    const ready = await this._ask(begin, (st) => st.kind === 'ready' && st.size === data.length);
    const win = new TransferWindow(data.length, ready.window);
    this._win = win;
    this._phase = 'sending';
    this._emitProgress();
    for (;;) {
      this._check();
      if (win.allAcked) break;
      const r = win.next(this.link.maxWrite);
      if (r) await this._writeData(data.subarray(r[0], r[1]));
      else await this._until((st) => st.kind === 'ack'); // the window is full: wait for the pet
    }
    this._phase = 'finishing';
    this._emitProgress();
    const outcome = path === null ? 'done' : path === STREAM ? 'streamed' : 'saved';
    // an app image's "done" can be lost to the reboot; the pet coming back answers then
    const end = this._until((st) => st.kind === outcome || (path === null && st.kind === 'rebooted'));
    end.catch(() => {});
    try {
      await this.link.writeControl(cmd.end());
    } catch (e) {
      throw this._writeError(e, 'Could not send END');
    }
    job.endSent = true;
    return end;
  }

  _onAck(off) {
    const job = this._job;
    if (!job || job.type !== 'transfer' || !this._win || this._phase !== 'sending') return;
    this._win.ack(off);
    if (!job.firstAckAt && off > 0) job.firstAckAt = now();
    job.acked = job.bytesDone + this._win.acked;
    job.part = this._win.fraction;
    const pct = Math.floor(this._overall() * 100);
    // packs, apps and batches: the pet draws the host's % (an app image draws its own)
    if ((job.path !== null || this._batch.count > 1) && pct !== this._lastPct) {
      this._lastPct = pct;
      this.link.writeControl(cmd.progress(pct)).catch(() => {});
    }
    this._emitProgress();
  }

  _checkStall() {
    if (!this._job || this._jobError) return;
    const quiet = now() - this._lastProgress;
    if (this._phase === 'rebooting') {
      if (quiet > this.opts.rebootMs) this._fail(new PetError('The pet did not come back after the update'));
    } else if (quiet > this.opts.stallMs) {
      this.cancel(`Update stalled: no progress from the pet for ${Math.round(this.opts.stallMs / 1000)} s`);
    }
  }

  _overall() {
    const b = this._batch;
    return b.total === 0 ? 0 : Math.min(1, (b.done + (this._job?.acked ?? 0)) / b.total);
  }

  _emitProgress() {
    const job = this._job;
    const secs = job ? Math.max(0.001, (now() - job.startedAt) / 1000) : 0;
    this._emit('progress', {
      phase: this._phase,
      label: job?.label ?? '',
      overall: this._overall(),
      part: job?.part ?? 0,
      acked: job?.acked ?? 0,
      total: job?.bytes ?? 0,
      bytesPerSecond: job?.acked ? Math.round(job.acked / secs) : 0,
      job: this._batch.index,
      jobs: this._batch.count,
      file: job?.fileIndex ?? 0,
      files: job?.files?.length ?? 0,
      path: job?.path ?? null,
      queued: this._queue.length,
      busy: this._running,
    });
  }

  // ---- speaker test and microphone ----------------------------------------------------------

  /** Fail the running control job with `message` if it has not finished within `ms`. */
  _deadline(ms, message) {
    const timer = setTimeout(() => this._fail(new PetError(message)), ms);
    return () => clearTimeout(timer);
  }

  _refusal(why) {
    if (why === 'no mic') return new PetError('This pet has no microphone (the 2.8in lcd28 board has one)', { why });
    return new PetError(`Pet refused: ${why}`, { why });
  }

  async _runSelftest() {
    const done = this._deadline(this.opts.selftestMs, 'No answer to the speaker test');
    try {
      const st = await this._ask(cmd.selftest(), (s) => s.kind === 'selftest');
      return { ...st, message: describeSelftest(st) };
    } finally {
      done();
    }
  }

  async _runListen(ms, sendAudio) {
    const wait = clampInt(ms, 100, 10000);
    const wantAudio = !!sendAudio && !!this.link.hasMic; // no mic characteristic: the numbers only
    const mic = new MicStream();
    this._mic = mic;
    const done = this._deadline(wait + this.opts.listenGraceMs, `No answer to LISTEN within ${Math.round((wait + this.opts.listenGraceMs) / 1000)} s`);
    try {
      const reply = this._ask(cmd.listen(wait, wantAudio), (s) => s.kind === 'heard');
      reply.catch(() => {});
      // the "heard" line, or (if it never comes) the whole recording and a short grace
      const orphan = wantAudio ? mic.whole.then(() => sleep(this.opts.heardGraceMs)).then(() => null) : new Promise(() => {});
      let heard;
      try {
        heard = await Promise.race([reply, orphan]);
      } catch (e) {
        if (!mic.complete) throw e;
        heard = null; // timed out, but the audio is all here: measure it ourselves
      }
      if (wantAudio && !mic.complete) {
        // "heard" may come before the audio, and a firmware may leave out "sent": give the
        // first bytes a moment, then wait for the rest while they keep coming
        if (heard && !heard.sent && mic.length === 0) await mic.settle(Math.min(500, this.opts.micQuietMs));
        if (heard === null || heard.sent || mic.length > 0) await mic.settle(this.opts.micQuietMs);
      }
      const clip = mic.length ? mic.bytes() : null;
      const pcm = clip ? decodePSND(clip) : null;
      const levels = heard ?? { ...micMetrics(pcm ?? new Int16Array(0)), ms: wait, sent: true };
      const result = {
        ms: levels.ms,
        samples: levels.samples,
        rms: levels.rms,
        peak: levels.peak,
        freq: levels.freq,
        db: levels.db,
        floor: levels.floor,
        sent: levels.sent,
        estimated: heard === null, // the pet's numbers were lost; these come from the recording
        pcm: pcm && pcm.length ? pcm : null,
        clip,
        sampleRate: clip ? psndInfo(clip)?.sampleRate ?? PSND.sampleRate : PSND.sampleRate,
      };
      const audio = result.pcm ? ` · ${result.pcm.length} samples` : wantAudio ? ` · no audio came back (${mic.length} bytes)` : '';
      result.message = `Heard: ${describeHeard(result)}${result.estimated ? ' (measured here)' : ''}${audio}`;
      return result;
    } finally {
      done();
    }
  }

  // ---- what the pet sends -------------------------------------------------------------------

  _onControl(bytes) {
    const text = dec.decode(toBytes(bytes));
    const o = jsonObject(text);
    const info = infoFrom(o);
    if (info) return this._setInfo(info, text);
    const st = statusFrom(o);
    if (!st) return;
    this._emit('status', st);
    if (st.kind !== 'selftest' && st.kind !== 'heard') this._touch();
    if (st.kind === 'ack') this._onAck(st.off);
    if (st.kind === 'done' && this._job?.type === 'transfer') {
      this._phase = 'rebooting'; // the pet accepted the image and restarts in ~1 s
      this._rebootDropped = false;
      this._emitProgress();
    }
    if (st.kind === 'error') {
      if (this._job) this._fail(this._job.type === 'transfer' ? new PetError(st.why, { why: st.why }) : this._refusal(st.why));
      else this._log(`Pet: ${st.why}`);
      return;
    }
    this._deliver(st);
  }

  _setInfo(info, text) {
    const changed = text !== this._infoText;
    this.info = info;
    this._infoText = text;
    // back from an OTA: only info read after the link dropped counts (a read before the
    // restart would still show the old slot)
    if (this._phase === 'rebooting' && this._rebootDropped) this._deliver({ kind: 'rebooted', info });
    if (changed) this._emit('info', info);
  }

  _onTx(bytes) {
    for (const line of this._lines.push(bytes)) {
      const ev = parseEvent(line);
      if (!ev) continue;
      if (ev.evt === 'tap') this._emit('tap', {});
      if (ev.evt === 'say') this._emit('say', { text: String(ev.text ?? '') });
      this._emit('event', ev);
    }
  }

  _onDisconnect() {
    this._stopInfoTimer();
    const job = this._job;
    if (job && !this._jobError) {
      if (this._phase === 'rebooting') {
        this._rebootDropped = true; // expected: the pet restarts into the new image
      } else if (this._phase === 'finishing' && job.type === 'transfer' && job.path === null && job.endSent) {
        this._phase = 'rebooting'; // app image: the "done" notify may not have made it out
        this._rebootDropped = true;
        this._touch();
      } else {
        this._fail(new PetError(job.type === 'transfer' ? LINK_LOST : 'Link lost'));
      }
    }
    this._log(!this._wanted ? 'Disconnected' : this._phase === 'rebooting' ? 'The pet restarts into the new firmware…' : 'Link lost');
    this._emit('disconnect', { expected: !this._wanted });
    if (this._wanted && this.opts.reconnectMs > 0) this._reconnect();
  }

  async _reconnect() {
    if (this._reconnecting) return;
    this._reconnecting = true;
    const until = now() + this.opts.reconnectMs;
    try {
      while (this._wanted && !this.link.connected && now() < until) {
        await sleep(this.opts.reconnectEveryMs);
        if (!this._wanted || this.link.connected) break;
        try {
          await this.link.connect();
        } catch {
          continue; // not back yet
        }
        this._log(`Reconnected to ${this.name}`);
        this._emit('connect', { name: this.name, reconnected: true });
        this._startInfoTimer();
        if (this.link.hasOTA) await this.readInfo().catch(() => {});
      }
      if (this._wanted && !this.link.connected) this._log('Gave up reconnecting; press Connect to try again');
    } finally {
      this._reconnecting = false;
    }
  }

  _startInfoTimer() {
    this._stopInfoTimer();
    if (!this.opts.infoEveryMs) return;
    this._infoTimer = setInterval(() => {
      if (this.link.connected && this.link.hasOTA && !this._job) this.readInfo().catch(() => {});
    }, this.opts.infoEveryMs);
    this._infoTimer.unref?.(); // Node: a forgotten pet does not keep the process alive
  }

  _stopInfoTimer() {
    clearInterval(this._infoTimer);
    this._infoTimer = null;
  }

  _emit(type, detail) {
    this.dispatchEvent(new CustomEvent(type, { detail }));
  }

  _log(message) {
    this._emit('log', message);
  }
}

// =============================================================================================
// Web Bluetooth link
// =============================================================================================

const copyOf = (view) => new Uint8Array(view.buffer.slice(view.byteOffset, view.byteOffset + view.byteLength));

/**
 * The GATT plumbing for one BluetoothDevice. Every GATT operation goes through one queue:
 * Chrome refuses a second operation while one is in flight ("GATT operation already in
 * progress"). Web Bluetooth does not expose the negotiated MTU, so data and heartbeat writes
 * use `chunkSize` bytes (180 fits the ATT MTU of 185 that Apple platforms negotiate; the pet
 * asks for 517).
 */
export class WebBluetoothLink {
  constructor(device, { chunkSize = 180, connectTimeoutMs = 15000 } = {}) {
    this.device = device;
    this.maxWrite = chunkSize;
    this.connectTimeoutMs = connectTimeoutMs;
    this.onControl = null;
    this.onTx = null;
    this.onMic = null;
    this.onDisconnect = null;
    this._c = null; // the characteristics of the live connection
    this._ops = Promise.resolve();
    this._onTxEvent = (e) => this.onTx?.(copyOf(e.target.value));
    this._onControlEvent = (e) => this.onControl?.(copyOf(e.target.value));
    this._onMicEvent = (e) => this.onMic?.(copyOf(e.target.value));
    device.addEventListener('gattserverdisconnected', () => {
      const was = this._c !== null;
      this._c = null;
      if (was) this.onDisconnect?.();
    });
  }

  get name() {
    return this.device.name || 'pet';
  }

  get connected() {
    return this._c !== null && !!this.device.gatt?.connected;
  }

  get hasOTA() {
    return !!(this._c?.control && this._c?.data);
  }

  get hasMic() {
    return !!this._c?.mic;
  }

  async connect() {
    if (this.connected) return;
    const gatt = this.device.gatt;
    const server = await within(gatt.connect(), this.connectTimeoutMs, 'The pet did not answer the connection', () => gatt.disconnect());
    try {
      const nus = await server.getPrimaryService(UUID.nusService);
      const c = { rx: await nus.getCharacteristic(UUID.nusRx), tx: await nus.getCharacteristic(UUID.nusTx), control: null, data: null, mic: null };
      try {
        const ota = await server.getPrimaryService(UUID.otaService);
        c.control = await ota.getCharacteristic(UUID.otaControl);
        c.data = await ota.getCharacteristic(UUID.otaData);
        c.mic = await ota.getCharacteristic(UUID.otaMic).catch(() => null); // firmware before LISTEN has none
      } catch {
        // firmware without the OTA service: heartbeats only
      }
      c.tx.addEventListener('characteristicvaluechanged', this._onTxEvent);
      await c.tx.startNotifications();
      if (c.control) {
        c.control.addEventListener('characteristicvaluechanged', this._onControlEvent);
        await c.control.startNotifications();
      }
      if (c.mic) {
        c.mic.addEventListener('characteristicvaluechanged', this._onMicEvent);
        await c.mic.startNotifications().catch(() => {
          c.mic = null;
        });
      }
      this._ops = Promise.resolve();
      this._c = c;
    } catch (e) {
      if (gatt.connected) gatt.disconnect();
      throw new PetError(`Not an OraclePet, or it went away: ${e.message}`, { cause: e });
    }
  }

  async disconnect() {
    if (this.device.gatt?.connected) this.device.gatt.disconnect();
  }

  _op(fn) {
    const run = this._ops.then(() => {
      if (!this._c) throw new PetError('Not connected to a pet');
      return fn(this._c);
    });
    this._ops = run.catch(() => {});
    return run;
  }

  writeControl(bytes) {
    return this._op((c) => write(c.control, bytes, true));
  }

  writeData(bytes) {
    return this._op((c) => write(c.data, bytes, false));
  }

  writeRx(bytes) {
    return this._op((c) => write(c.rx, bytes, false));
  }

  readControl() {
    return this._op(async (c) => {
      if (!c.control) throw new PetError('This pet has no OTA service');
      return copyOf(await c.control.readValue());
    });
  }
}

function write(ch, bytes, withResponse) {
  if (!ch) throw new PetError('This pet has no OTA service');
  if (withResponse) return ch.writeValueWithResponse ? ch.writeValueWithResponse(bytes) : ch.writeValue(bytes);
  return ch.writeValueWithoutResponse ? ch.writeValueWithoutResponse(bytes) : ch.writeValue(bytes);
}
