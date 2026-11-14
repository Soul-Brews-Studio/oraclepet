/*
 * simpet.js: an OraclePet in memory, for the tests and for trying the page without a pet.
 *
 * It has the link shape oraclepet.js drives (connect, disconnect, writeControl, writeData,
 * writeRx, readControl; onControl, onTx, onMic, onDisconnect) and answers the way the firmware
 * does (lab/jc3248-pet-idf/main): ota.c for the commands, "ready", acks every 4 KB and every
 * 0.5 s while idle, END, the outcomes and their error lines, and a 32 KB buffer that overflows
 * when a host ignores the 16 KB window; audio.c for streams played at the speaker's pace,
 * SELFTEST and LISTEN; ble.c for heartbeat lines on NUS RX. Faults can be switched on to walk a
 * host through its error paths. Nothing here touches Bluetooth.
 */
import { crc32, encodePSND, imageInfo, micMetrics } from './oraclepet.js';

const enc = new TextEncoder();
const dec = new TextDecoder();
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
const now = () => globalThis.performance?.now?.() ?? Date.now();

/** Errors shaped like the DOMExceptions Web Bluetooth throws for an ATT error or a dead link. */
const gattError = (name, message) => Object.assign(new Error(message), { name });
const notSupported = () => gattError('NotSupportedError', 'GATT Error: Request not supported.');
const badLength = () => gattError('InvalidModificationError', 'GATT Error: Invalid attribute value length.');

const IDLE = 'idle';
const STARTING = 'starting';
const RECEIVING = 'receiving';
const FINISHING = 'finishing';
const HEADER = 32 + 256; // ota.c reads the image header + esp_app_desc_t before it judges the image

/**
 * Options. Faults (all off by default):
 *   silentAfter: N    after taking N bytes of a transfer the pet hangs: no data, no lines
 *   dropAfter: N      the link drops once N bytes of a transfer have arrived
 *   corrupt: true     one byte of the next transfer is flipped on its way to storage
 *   rollback: true    after an OTA the pet boots its old slot again
 *   noReturn: true    after an OTA the pet never comes back
 *   dropStatus: [k]   status lines of these kinds ("done", "heard", …) are lost on the air
 *   refuse: [op]      command bytes this firmware does not know (ATT "request not supported")
 */
export const SIM_DEFAULTS = Object.freeze({
  name: 'Claude-51A7',
  project: 'oraclepet_lcd28', // 'jc3248_pet_idf' is the board without a mic
  version: 'sim-1.0',
  built: 'Oct 21 2026 12:00:00',
  sha: '51a7e0000',
  slot: 'ota_0',
  mic: null, // null: the lcd28 has a mic, the jc3248 does not
  maxWrite: 244, // the longest write the link takes (ATT MTU 247 - 3)
  window: 16 * 1024, // ota.c WINDOW
  buffer: 32 * 1024, // ota.c SB_SIZE: more unread than this and the pet aborts
  ackEvery: 4 * 1024, // ota.c ACK_EVERY
  rate: 40 * 1024, // bytes/s the pet takes off the link into storage
  playRate: 8127, // bytes/s a stream plays at: 256 bytes per 504 samples at 16 kHz
  audioBuffer: 8 * 1024, // audio.c STREAM_BUF
  tickMs: 20,
  idleAckMs: 500, // re-ack while no data comes, so a lost ack cannot stall the host
  idleFailMs: 15000, // ota.c IDLE_MS
  endWaitMs: 10000, // how long the pet waits for END after the last byte
  streamEndWaitMs: 5000,
  lineMax: 200, // a status line this long or longer is dropped (ota.c notify() char line[200])
  micChunk: null, // LISTEN audio notification size; null: maxWrite (ota.c mic_audio uses MTU - 3)
  timeScale: 1, // scales the physical waits: beep, recording, reboot, boot
  rebootDelayMs: 1000, // from "done" to the restart
  bootMs: 3000, // from the restart until it can be reached again
  storageKB: 1536,
  files: null, // { path: Uint8Array }; null gives a small demo set
  pack: 'atlas',
  app: '',
  heardFirst: false, // LISTEN: notify "heard" before sending the audio
  faults: {},
});

/** ota.c safe_path, kept apart from the host's pathProblem() so the tests compare two readings. */
export function safePath(p) {
  if (p.includes('..')) return false;
  const ok = (c) => (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c === '-' || c === '_' || c === '.';
  if (p.startsWith('apps/') || p.startsWith('sounds/')) {
    const ext = p[0] === 'a' ? '.wasm' : '.ima';
    const pre = p[0] === 'a' ? 5 : 7;
    const n = enc.encode(p).length;
    if (n < pre + 1 + ext.length || n - pre - ext.length > 23 || !p.endsWith(ext)) return false;
    const rest = p.slice(pre);
    return !rest.includes('/') && [...rest].every(ok);
  }
  if (!p.startsWith('characters/')) return false;
  let slashes = 0;
  for (const c of p) {
    if (c === '/') slashes++;
    else if (!ok(c)) return false;
  }
  return slashes === 2;
}

function cString(b, off, len) {
  let z = off;
  while (z < off + len && b[z] !== 0) z++;
  return dec.decode(b.subarray(off, z));
}

/**
 * A synthetic ESP32 app image, as `idf.py build` would describe it (esp_app_desc_t at offset 32):
 * for testing the OTA path without a real build.
 */
export function makeImage({ project = 'oraclepet_lcd28', version = 'v2.0.0', date = 'Oct 21 2026', time = '13:00:00', size = 4096, seed = 1 } = {}) {
  const img = new Uint8Array(Math.max(size, 300));
  let s = seed >>> 0;
  for (let i = HEADER; i < img.length; i++) {
    s = (Math.imul(s, 1103515245) + 12345) >>> 0;
    img[i] = s >>> 24;
  }
  img[0] = 0xe9;
  img.set([0x32, 0x54, 0xcd, 0xab], 32); // ESP_APP_DESC_MAGIC_WORD
  const put = (text, off, len) => img.set(enc.encode(text).subarray(0, len - 1), off);
  put(version, 32 + 16, 32);
  put(project, 32 + 48, 32);
  put(time, 32 + 80, 16);
  put(date, 32 + 96, 16);
  for (let i = 0; i < 32; i++) img[32 + 144 + i] = (seed * 37 + i * 11) & 0xff; // app_elf_sha256
  return img;
}

function demoFiles() {
  const gif = (n) => Uint8Array.from([0x47, 0x49, 0x46, 0x38, 0x39, 0x61, n, 0, n, 0, 0, 0, 0, 0x3b]);
  const manifest = (name) => enc.encode(JSON.stringify({ name, states: { idle: 'idle.gif', busy: 'busy.gif' } }));
  const chirp = new Int16Array(4800); // 0.3 s, rising
  for (let i = 0; i < chirp.length; i++) {
    const t = i / 16000;
    chirp[i] = Math.round(8000 * Math.sin(2 * Math.PI * (600 + 1500 * t) * t));
  }
  const wasm = Uint8Array.of(0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00);
  return {
    'characters/atlas/manifest.json': manifest('atlas'),
    'characters/atlas/idle.gif': gif(1),
    'characters/atlas/busy.gif': gif(2),
    'characters/siamese/manifest.json': manifest('siamese'),
    'characters/siamese/idle.gif': gif(3),
    'characters/siamese/busy.gif': gif(4),
    'apps/hello.wasm': wasm,
    'apps/talker.wasm': wasm,
    'sounds/chirp.ima': encodePSND(chirp),
  };
}

export class SimPet {
  constructor(options = {}) {
    const o = { ...SIM_DEFAULTS, ...options, faults: { ...(options.faults ?? {}) } };
    this.opts = o;
    this.name = o.name;
    this.maxWrite = o.maxWrite;
    // what the pet runs and keeps
    this.project = o.project;
    this.version = o.version;
    this.built = o.built;
    this.sha = o.sha;
    this.slot = o.slot;
    this.imageState = 'valid';
    this.pack = o.pack;
    this.app = o.app;
    this.appError = '';
    this.fs = new Map(Object.entries(o.files ?? demoFiles()));
    // the link
    this.connected = false;
    this.onControl = null;
    this.onTx = null;
    this.onMic = null;
    this.onDisconnect = null;
    this._down = false; // restarting (or gone): cannot be reached
    this._sub = { control: false, mic: false, tx: false };
    this._q = [];
    this._qTimer = null;
    // ota.c state
    this.state = IDLE;
    this.kind = '';
    this.path = '';
    this.size = 0;
    this.crcExpect = 0;
    this.received = 0;
    this.written = 0;
    this.abortFlag = false;
    this.endFlag = false;
    this._sb = [];
    this._sbBytes = 0;
    this._lastAck = 0;
    this._silent = false;
    this._corrupted = false;
    this._rx = [];
    // what a test looks at
    this.seen = {
      control: [], // every command written, as bytes
      writes: [], // the length of every data write
      progress: [], // PROGRESS percentages
      aborts: 0,
      maxAhead: 0, // most bytes ever received beyond the last ack sent
      overflow: false, // the 32 KB buffer overflowed (a host that ignored the window)
      lines: [], // status lines notified
      heartbeatLines: [], // NUS RX lines, as text
      heartbeats: [], // ... parsed
      streams: [], // clips that arrived by BEGIN_STREAM
      played: [], // PLAY names
      listens: [], // { ms, send, pcm }
    };
  }

  /** The OTA service and the mic characteristic are there on every board with current firmware. */
  get hasOTA() {
    return true;
  }

  get hasMic() {
    return true;
  }

  /** Whether this board has a microphone (only the lcd28 does). */
  get microphone() {
    return this.opts.mic ?? this.project === 'oraclepet_lcd28';
  }

  // ---- the link: what oraclepet.js calls ----------------------------------------------------

  async connect() {
    await this._air();
    if (this._down) throw gattError('NetworkError', 'Bluetooth Device is no longer in range.');
    this.connected = true;
    this._sub = { control: true, mic: true, tx: true };
  }

  async disconnect() {
    if (this.connected) this._lose();
    await this._air();
  }

  async writeControl(bytes) {
    this._needLink();
    const b = Uint8Array.from(bytes);
    this.seen.control.push(b);
    const err = this._command(b); // ctrl_access may notify before its write response goes out
    await this._air();
    if (err) throw err;
  }

  async writeData(bytes) {
    this._needLink();
    if (bytes.length > this.maxWrite) throw gattError('InvalidModificationError', `A ${bytes.length}-byte write is longer than the link's ${this.maxWrite}.`);
    this.seen.writes.push(bytes.length);
    this._data(Uint8Array.from(bytes));
  }

  /** NUS RX (ble.c blePoll): lines end at "\n" or "\r"; a line past 1 KB is dropped. */
  async writeRx(bytes) {
    this._needLink();
    if (bytes.length > this.maxWrite) throw gattError('InvalidModificationError', `A ${bytes.length}-byte write is longer than the link's ${this.maxWrite}.`);
    for (const c of bytes) {
      if (c === 0x0a || c === 0x0d) {
        if (this._rx.length) {
          const line = dec.decode(Uint8Array.from(this._rx));
          this.seen.heartbeatLines.push(line);
          if (line[0] === '{') {
            try {
              this.seen.heartbeats.push(JSON.parse(line));
            } catch {
              // the firmware's scanner would read what it could; the tests only want whole lines
            }
          }
        }
        this._rx = [];
      } else {
        if (this._rx.length >= 1023) this._rx = [];
        this._rx.push(c);
      }
    }
    await this._air();
  }

  async readControl() {
    this._needLink();
    await this._air();
    return enc.encode(this.infoLine());
  }

  // ---- things a test (or the demo page) can make the pet do -----------------------------------

  /** The screen was touched: {"evt":"tap"}, split over two notifications to test reassembly. */
  tap() {
    this._event('{"evt":"tap"}');
  }

  /** A pet app called pet.say(text). */
  say(text) {
    this._event(JSON.stringify({ evt: 'say', text }));
  }

  /** What a read of the control characteristic returns (ota.c ctrl_access, same layout). */
  infoLine() {
    const keys = [...this.fs.keys()];
    const packs = [...new Set(keys.filter((p) => p.startsWith('characters/')).map((p) => p.split('/')[1]))];
    const apps = keys.filter((p) => /^apps\/.+\.wasm$/.test(p)).map((p) => p.slice(5, -5).slice(0, 23));
    const list = (a) => a.map((x) => `"${x}"`).join(',');
    return (
      `{"ver":"${this.version}","project":"${this.project}","built":"${this.built}","slot":"${this.slot}",` +
      `"sha":"${this.sha}","state":"${this.imageState}","pack":"${this.pack}","packs":[${list(packs)}],` +
      `"free":${Math.floor(this._free() / 1024)},"app":"${this.app}","apps":[${list(apps)}],"apperr":"${this.appError.slice(0, 60)}"}`
    );
  }

  /** What the mic hears: 100 ms of room noise, then the pet's 1 kHz beep (deterministic). */
  recording(ms) {
    const pcm = new Int16Array(ms * 16);
    let seed = 0x51a7;
    for (let i = 0; i < pcm.length; i++) {
      seed = (Math.imul(seed, 1103515245) + 12345) >>> 0;
      const noise = ((seed >>> 16) / 65536 - 0.5) * 120;
      const tone = i >= 1600 ? 3000 * Math.sin((2 * Math.PI * 1000 * i) / 16000) : 0;
      pcm[i] = Math.round(noise + tone);
    }
    return pcm;
  }

  // ---- the air: notifications, write responses and link events, in order, a moment later ----

  _air(fn) {
    return new Promise((resolve) => {
      this._q.push(() => {
        try {
          fn?.();
        } finally {
          resolve();
        }
      });
      this._qTimer ??= setTimeout(() => this._drain(), 0);
    });
  }

  _drain() {
    this._qTimer = null;
    for (const f of this._q.splice(0)) {
      try {
        f();
      } catch (e) {
        setTimeout(() => {
          throw e;
        });
      }
    }
  }

  _needLink() {
    if (!this.connected) throw gattError('NetworkError', 'GATT Server is disconnected. Cannot perform GATT operations.');
  }

  /** ota.c notify(): nothing without a link and a subscriber; a line too long for the buffer is lost. */
  _notify(line) {
    if (this._silent || !this.connected || !this._sub.control) return;
    if (enc.encode(line).length >= this.opts.lineMax) return;
    const kind = /"ota":"(\w+)"/.exec(line)?.[1];
    if (this.opts.faults.dropStatus?.includes(kind)) return;
    this.seen.lines.push(line);
    const bytes = enc.encode(line);
    this._air(() => {
      if (this.connected) this.onControl?.(bytes);
    });
  }

  /** ota.c mic_audio(): one chunk of the PSND stream, cut into notifications of at most MTU - 3. */
  _micNotify(bytes) {
    if (this._silent || !this.connected || !this._sub.mic) return;
    const max = this.opts.micChunk ?? this.maxWrite;
    for (let at = 0; at < bytes.length; at += max) {
      const b = bytes.slice(at, at + max);
      this._air(() => {
        if (this.connected) this.onMic?.(b);
      });
    }
  }

  _event(json) {
    if (!this.connected || !this._sub.tx) return;
    const b = enc.encode(`${json}\n`);
    const cut = Math.ceil(b.length / 2);
    for (const part of [b.slice(0, cut), b.slice(cut)]) {
      this._air(() => {
        if (this.connected) this.onTx?.(part);
      });
    }
  }

  /** The link drops (ota_on_disconnect: a transfer in flight is abandoned). */
  _lose() {
    this.connected = false;
    this._sub = { control: false, mic: false, tx: false };
    if (this.state !== IDLE) this.abortFlag = true;
    this._air(() => this.onDisconnect?.());
  }

  // ---- ota.c ctrl_access: one command; returns the ATT error to answer with, if any ----------

  _command(b) {
    const len = b.length;
    if (len === 0) return badLength();
    const op = b[0];
    if (this.opts.faults.refuse?.includes(op)) return notSupported();
    const text = (from, n) => dec.decode(b.subarray(from, from + n));
    switch (op) {
      case 0x01: // BEGIN
      case 0x04: // BEGIN_FILE
      case 0x0b: {
        // BEGIN_STREAM
        if (len < 9) return badLength();
        if (this.state !== IDLE) {
          this._notify('{"ota":"error","why":"busy"}');
          return null;
        }
        const v = new DataView(b.buffer, b.byteOffset, len);
        let kind = op === 0x0b ? 'stream' : 'app';
        let path = '';
        if (op === 0x04) {
          const pl = len > 9 ? b[9] : 0;
          if (pl === 0 || pl >= 96 || 10 + pl > len) return badLength();
          path = text(10, pl);
          if (!safePath(path)) {
            this._notify('{"ota":"error","why":"bad path"}');
            return null;
          }
          kind = 'file';
        }
        Object.assign(this, { kind, path, size: v.getUint32(1, true), crcExpect: v.getUint32(5, true) });
        Object.assign(this, { received: 0, written: 0, abortFlag: false, endFlag: false, state: STARTING });
        this._sb = [];
        this._sbBytes = 0;
        this._lastAck = 0;
        setTimeout(() => this._transfer(), 0); // ota_task takes it from here
        return null;
      }
      case 0x02: // END
        this.endFlag = true;
        return null;
      case 0x03: // ABORT
        this.abortFlag = true;
        this.seen.aborts += 1;
        return null;
      case 0x07: // PROGRESS
        if (len < 2) return badLength();
        this.seen.progress.push(Math.min(100, b[1]));
        return null;
      case 0x06: {
        // RUN_APP: the main thread starts it
        const nl = len > 1 ? b[1] : 0;
        if (nl >= 24 || 2 + nl > len) return badLength();
        const name = text(2, nl);
        setTimeout(() => this._runApp(name), this._t(30));
        return null;
      }
      case 0x05: {
        // SELECT_PACK: the main thread loads it
        const nl = len > 1 ? b[1] : 0;
        if (nl === 0 || nl >= 24 || 2 + nl > len) return badLength();
        const name = text(2, nl);
        setTimeout(() => this._selectPack(name), this._t(30));
        return null;
      }
      case 0x08: {
        // PLAY: answered inside the write, before its response
        const nl = len > 1 ? b[1] : 0;
        if (nl === 0 || nl >= 24 || 2 + nl > len) return badLength();
        const name = text(2, nl);
        const clip = this.fs.get(`sounds/${name}.ima`);
        if (!clip) {
          this._notify(`{"ota":"error","why":"no sound ${name}"}`);
          return null;
        }
        this.seen.played.push(name);
        this._notify(`{"ota":"play","name":"${name}","bytes":${clip.length}}`);
        return null;
      }
      case 0x0a: // SELFTEST
        this._selftest();
        return null;
      case 0x0c: {
        // LISTEN
        if (len < 4) return badLength();
        const ms = Math.min(10000, Math.max(100, b[1] | (b[2] << 8)));
        this._listen(ms, (b[3] & 1) === 1 && this._sub.mic);
        return null;
      }
      default:
        return notSupported();
    }
  }

  // ---- ota.c data_access: bytes in, only while a transfer receives ----------------------------

  _data(b) {
    if (this.state !== RECEIVING) return; // stray bytes outside a transfer
    this.received += b.length;
    if (this.received > this.size) {
      this.abortFlag = true;
      return;
    }
    if (this._sbBytes + b.length > this.opts.buffer) {
      this.seen.overflow = true; // the host ignored the window
      this.abortFlag = true;
      return;
    }
    this._sb.push(b);
    this._sbBytes += b.length;
    this.seen.maxAhead = Math.max(this.seen.maxAhead, this.received - this._lastAck);
    const f = this.opts.faults;
    if (f.dropAfter != null && this.received >= f.dropAfter) {
      f.dropAfter = null;
      this._lose();
    }
  }

  _take(n) {
    const out = new Uint8Array(n);
    let at = 0;
    while (at < n) {
      const c = this._sb[0];
      const k = Math.min(c.length, n - at);
      out.set(c.subarray(0, k), at);
      at += k;
      if (k === c.length) this._sb.shift();
      else this._sb[0] = c.subarray(k);
    }
    this._sbBytes -= n;
    return out;
  }

  _ack(off) {
    this._lastAck = off;
    this._notify(`{"ota":"ack","off":${off}}`);
  }

  _failed(why) {
    this.state = IDLE;
    this._notify(`{"ota":"error","why":"${why}"}`);
  }

  _free() {
    let used = 0;
    for (const b of this.fs.values()) used += b.length;
    return this.opts.storageKB * 1024 - used;
  }

  // ---- ota.c ota_task / file_transfer / stream_transfer -----------------------------------------

  async _transfer() {
    const o = this.opts;
    const { kind, size } = this;
    if (kind === 'file' && size + 8192 > this._free()) return this._failed('storage full');
    const store = new Uint8Array(size);
    let crc = 0;
    let lastAck = 0;
    let lastData = now();
    let lastIdleAck = now();
    let audio = 0; // stream: bytes waiting in the speaker's buffer, playing out at playRate
    let audioAt = now();
    let why = '';
    this.state = RECEIVING;
    this._notify(`{"ota":"ready","size":${size},"window":${o.window}}`);
    while (this.written < size) {
      if (this.abortFlag) {
        why = kind === 'app' && this.received > size ? 'too many bytes' : 'aborted';
        break;
      }
      await sleep(o.tickMs);
      if (this._silent) return; // hung: the host's watchdog has to notice
      let budget = Math.max(1, Math.round((o.rate * o.tickMs) / 1000));
      let blocked = false;
      if (kind === 'stream') {
        const t = now();
        audio = Math.max(0, audio - (o.playRate * (t - audioAt)) / 1000);
        audioAt = t;
        const room = Math.max(0, Math.floor(o.audioBuffer - audio));
        blocked = room === 0 && this._sbBytes > 0; // audio_stream_write blocks: no acks meanwhile
        budget = Math.min(budget, room);
      }
      const take = Math.min(this._sbBytes, budget, 4096);
      if (take === 0) {
        if (blocked) {
          lastData = now();
          continue;
        }
        if (now() - lastData >= o.idleFailMs) {
          why = 'timed out waiting for data';
          break;
        }
        if (now() - lastIdleAck >= o.idleAckMs) {
          lastIdleAck = now();
          this._ack(this.written); // re-ack: a lost ack must not stall the host
        }
        continue;
      }
      lastData = now();
      lastIdleAck = now();
      const chunk = this._take(take);
      if (o.faults.corrupt && !this._corrupted) {
        chunk[0] ^= 0xff;
        this._corrupted = true;
      }
      store.set(chunk, this.written);
      if (kind === 'app' && this.written < HEADER && this.written + take >= HEADER) {
        why = this._checkImage(store);
        if (why) break;
      }
      crc = crc32(chunk, crc);
      this.written += take;
      if (kind === 'stream') audio += take;
      if (o.faults.silentAfter != null && this.written >= o.faults.silentAfter) {
        this._silent = true;
        return;
      }
      if (this.written - lastAck >= o.ackEvery || this.written === size) {
        lastAck = this.written;
        this._ack(this.written);
      }
    }
    if (why && kind !== 'stream') return this._failed(why);

    // every byte is in: wait for END (a stream plays on and ends without one after a while)
    this.state = FINISHING;
    const t0 = now();
    const wait = kind === 'stream' ? o.streamEndWaitMs : o.endWaitMs;
    while (!this.endFlag && !this.abortFlag && now() - t0 < wait) {
      await sleep(o.tickMs);
      if (this._silent) return;
    }
    if (kind === 'stream') {
      this.state = IDLE;
      if (this.abortFlag || this.written < size) return this._notify('{"ota":"error","why":"stream cut short"}');
      this.seen.streams.push(store);
      return this._notify(`{"ota":"streamed","bytes":${this.written},"crc":${crc === this.crcExpect}}`);
    }
    if (!this.endFlag) return this._failed(kind === 'app' && this.abortFlag ? 'aborted' : 'no END from the host');
    if (crc !== this.crcExpect) {
      const h8 = (x) => x.toString(16).padStart(8, '0');
      return this._failed(kind === 'app' ? `crc mismatch ${h8(crc)} != ${h8(this.crcExpect)}` : 'crc mismatch');
    }
    if (kind === 'file') {
      this.fs.set(this.path, store);
      this.state = IDLE;
      return this._notify(`{"ota":"saved","path":"${this.path}"}`);
    }
    if (size < HEADER) return this._failed('ESP_ERR_OTA_VALIDATE_FAILED'); // esp_ota_end
    this.state = IDLE;
    this._notify('{"ota":"done"}');
    this._reboot(store);
  }

  /** ota.c check_desc: an ESP app image, built as this board's project. */
  _checkImage(h) {
    const magic = (h[32] | (h[33] << 8) | (h[34] << 16) | (h[35] << 24)) >>> 0;
    if (h[0] !== 0xe9 || magic !== 0xabcd5432) return 'not an ESP app image';
    const project = cString(h, 32 + 48, 32);
    return project === this.project ? '' : `wrong firmware (${project.slice(0, 24)})`;
  }

  /** "done": restart ~1 s later on the other slot (rollback: the old one), unreachable while booting. */
  _reboot(image) {
    const info = imageInfo(image);
    const f = this.opts.faults;
    setTimeout(() => {
      if (!f.rollback && info) {
        this.slot = this.slot === 'ota_0' ? 'ota_1' : 'ota_0';
        this.version = info.version;
        this.built = info.built;
        this.sha = info.sha.slice(0, 9);
        this.imageState = 'pending';
      }
      this._down = true;
      if (this.connected) this._lose();
      if (f.noReturn) return;
      setTimeout(() => {
        this._down = false;
        setTimeout(() => {
          this.imageState = 'valid'; // main.c marks the new image good once it runs
        }, this._t(2000));
      }, this._t(this.opts.bootMs));
    }, this._t(this.opts.rebootDelayMs));
  }

  _selectPack(name) {
    if (!this.fs.has(`characters/${name}/manifest.json`)) return this._notify(`{"ota":"error","why":"pack ${name} did not load"}`);
    this.pack = name;
    this._notify(`{"ota":"pack","name":"${name}"}`);
  }

  _runApp(name) {
    if (!name) {
      this.app = '';
      return this._notify('{"ota":"app","name":""}');
    }
    const wasm = this.fs.get(`apps/${name}.wasm`);
    const isWasm = wasm && wasm[0] === 0 && wasm[1] === 0x61 && wasm[2] === 0x73 && wasm[3] === 0x6d;
    const why = !wasm ? 'not on the pet' : !isWasm ? 'not a wasm module' : '';
    if (why) {
      this.appError = why;
      return this._notify(`{"ota":"error","why":"app ${name.slice(0, 20)}: ${why}"}`);
    }
    this.app = name;
    this.appError = '';
    this._notify(`{"ota":"app","name":"${name}"}`);
  }

  // ---- audio.c: the speaker test and the microphone --------------------------------------------

  _selftest() {
    const mic = this.microphone;
    const line = mic
      ? '{"ota":"selftest","mic":true,"heard":true,"db":37.5,"quiet":22,"tone":191}'
      : '{"ota":"selftest","mic":false,"heard":false,"db":0.0,"quiet":0,"tone":0}';
    setTimeout(() => this._notify(line), this._t(mic ? 2000 : 600));
  }

  _listen(ms, send) {
    setTimeout(() => {
      if (!this.microphone) return this._notify('{"ota":"error","why":"no mic"}');
      const pcm = this.recording(ms);
      const m = micMetrics(pcm);
      this.seen.listens.push({ ms, send, pcm });
      const heard =
        `{"ota":"heard","ms":${ms},"samples":${pcm.length},"rms":${m.rms},"peak":${m.peak},` +
        `"freq":${m.freq},"db":${m.db.toFixed(1)},"floor":${m.floor},"sent":${send}}`;
      if (this.opts.heardFirst) this._notify(heard);
      if (send) {
        // a PSND stream: the 16-byte header, then each 256-byte block (the last one padded, as
        // audio.c do_listen sends it), each cut to the notification size
        const clip = encodePSND(pcm);
        this._micNotify(clip.subarray(0, 16));
        for (let at = 16; at < clip.length; at += 256) this._micNotify(clip.subarray(at, at + 256));
      }
      if (!this.opts.heardFirst) this._notify(heard);
    }, this._t(ms));
  }

  _t(ms) {
    return ms * this.opts.timeScale;
  }
}
