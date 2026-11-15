// OraclePet peer link: a browser page and a machine (pet-peer.mjs) find each other with a short pairing
// code and talk over one WebRTC data channel. Runs in browsers and in Node (pass RTCPeerConnection).
//
//   code  "7KQ2-M9XD-PL4C"   60 random bits, shown on the page, given to the other side
//   room  sha256("oraclepet-room:" + code), first 32 hex chars, on the relay (tools/signal)
//   key   AES-GCM from sha256("oraclepet-key:" + code): the relay only ever carries ciphertext
//
// The page makes the offer, the machine answers. After that nothing goes through the relay: calls and
// results go peer to peer, DTLS-encrypted by WebRTC.

export const RELAY = 'wss://oraclepet-signal.laris.workers.dev';
export const ICE = [{ urls: 'stun:stun.cloudflare.com:3478' }, { urls: 'stun:stun.l.google.com:19302' }];
const ALPHABET = '0123456789ABCDEFGHJKMNPQRSTVWXYZ'; // Crockford base32: no I, L, O, U
const enc = new TextEncoder(), dec = new TextDecoder();
const subtle = () => globalThis.crypto.subtle;

export function makeCode() {
  const b = globalThis.crypto.getRandomValues(new Uint8Array(12));
  const s = [...b].map((x) => ALPHABET[x & 31]).join('');
  return `${s.slice(0, 4)}-${s.slice(4, 8)}-${s.slice(8, 12)}`;
}
export const normCode = (c) => String(c ?? '').toUpperCase().replace(/[^0-9A-Z]/g, '').replace(/[ILOU]/g, (x) => ({ I: '1', L: '1', O: '0', U: 'V' })[x]);

const hex = (buf) => [...new Uint8Array(buf)].map((x) => x.toString(16).padStart(2, '0')).join('');
const b64 = (u8) => { let s = ''; for (let i = 0; i < u8.length; i += 0x8000) s += String.fromCharCode(...u8.subarray(i, i + 0x8000)); return btoa(s); };
const unb64 = (s) => Uint8Array.from(atob(s), (c) => c.charCodeAt(0));

async function secrets(code) {
  const c = normCode(code);
  const room = hex(await subtle().digest('SHA-256', enc.encode(`oraclepet-room:${c}`))).slice(0, 32);
  const raw = await subtle().digest('SHA-256', enc.encode(`oraclepet-key:${c}`));
  const key = await subtle().importKey('raw', raw, 'AES-GCM', false, ['encrypt', 'decrypt']);
  return { room, key };
}
async function seal(key, obj) {
  const iv = globalThis.crypto.getRandomValues(new Uint8Array(12));
  const ct = new Uint8Array(await subtle().encrypt({ name: 'AES-GCM', iv }, key, enc.encode(JSON.stringify(obj))));
  const out = new Uint8Array(12 + ct.length); out.set(iv); out.set(ct, 12);
  return b64(out);
}
async function open(key, text) {
  try {
    const u = unb64(text);
    return JSON.parse(dec.decode(await subtle().decrypt({ name: 'AES-GCM', iv: u.subarray(0, 12) }, key, u.subarray(12))));
  } catch { return null; } // another code's message, or garbage
}

function gathered(pc, ms = 4000) {
  if (pc.iceGatheringState === 'complete') return Promise.resolve();
  return new Promise((resolve) => {
    const t = setTimeout(resolve, ms);
    const check = () => { if (pc.iceGatheringState === 'complete') { clearTimeout(t); resolve(); } };
    pc.addEventListener?.('icegatheringstatechange', check);
    pc.onicecandidate = (e) => { if (!e.candidate) { clearTimeout(t); resolve(); } };
  });
}

function relaySocket(url, room, WS) {
  return new Promise((resolve, reject) => {
    const ws = new WS(`${url}/room/${room}`);
    ws.onopen = () => resolve(ws);
    ws.onerror = () => reject(new Error(`cannot reach the pairing relay ${url}`));
  });
}

// ---- RPC over the data channel -------------------------------------------------------------------
// {t:'call', id, name, args} -> {t:'res', id, ok, result | error}; {t:'note', ...} one-way.
// Messages over 48 KB go as {t:'part', m, i, n, d} pieces. Bytes inside args/results travel as
// {$bytes: base64}: use Peer.bytes(u8) to wrap and Peer.unbytes(x) to unwrap.
const CHUNK = 48000;
export class Peer extends EventTarget {
  constructor(channel, pc, label) {
    super();
    this.dc = channel; this.pc = pc; this.label = label;
    this.handlers = {};
    this.pending = new Map();
    this.parts = new Map();
    this.nextId = 1;
    channel.onmessage = (e) => this._recv(typeof e.data === 'string' ? e.data : dec.decode(e.data));
    channel.onclose = () => { this._closed('closed'); };
  }
  static bytes(u8) { return { $bytes: b64(u8 instanceof Uint8Array ? u8 : new Uint8Array(u8)) }; }
  static unbytes(x) { return x && typeof x.$bytes === 'string' ? unb64(x.$bytes) : x; }
  get open() { return this.dc.readyState === 'open'; }
  handle(name, fn) { this.handlers[name] = fn; return this; }
  call(name, args = {}, timeoutMs = 60000) {
    if (!this.open) return Promise.reject(new Error('the peer link is closed'));
    const id = this.nextId++;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => { this.pending.delete(id); reject(new Error(`${name}: no answer from the peer in ${timeoutMs / 1000} s`)); }, timeoutMs);
      this.pending.set(id, { resolve, reject, timer });
      this._send({ t: 'call', id, name, args });
    });
  }
  note(type, detail = {}) { if (this.open) this._send({ t: 'note', type, detail }); }
  close() { try { this.dc.close(); } catch {} try { this.pc.close(); } catch {} this._closed('closed'); }
  _closed(why) {
    if (this._gone) return;
    this._gone = true;
    for (const p of this.pending.values()) { clearTimeout(p.timer); p.reject(new Error(`the peer link ${why}`)); }
    this.pending.clear();
    this.dispatchEvent(new CustomEvent('close', { detail: why }));
  }
  _send(msg) {
    const s = JSON.stringify(msg);
    if (s.length <= CHUNK) { this.dc.send(s); return; }
    const m = `${this.label}${this.nextId++}`, n = Math.ceil(s.length / CHUNK);
    for (let i = 0; i < n; i++) this.dc.send(JSON.stringify({ t: 'part', m, i, n, d: s.slice(i * CHUNK, (i + 1) * CHUNK) }));
  }
  async _recv(text) {
    let msg = JSON.parse(text);
    if (msg.t === 'part') {
      const got = this.parts.get(msg.m) ?? [];
      got[msg.i] = msg.d;
      this.parts.set(msg.m, got);
      if (got.filter((x) => x !== undefined).length < msg.n) return;
      this.parts.delete(msg.m);
      msg = JSON.parse(got.join(''));
    }
    if (msg.t === 'res') {
      const p = this.pending.get(msg.id);
      if (!p) return;
      this.pending.delete(msg.id); clearTimeout(p.timer);
      msg.ok ? p.resolve(msg.result) : p.reject(new Error(msg.error || 'failed on the peer'));
    } else if (msg.t === 'call') {
      const fn = this.handlers[msg.name];
      try {
        if (!fn) throw new Error(`the peer has no "${msg.name}" (it has ${Object.keys(this.handlers).join(', ') || 'nothing'})`);
        this._send({ t: 'res', id: msg.id, ok: true, result: (await fn(msg.args ?? {}, this)) ?? null });
      } catch (e) {
        this._send({ t: 'res', id: msg.id, ok: false, error: String(e?.message ?? e) });
      }
    } else if (msg.t === 'note') {
      this.dispatchEvent(new CustomEvent('note', { detail: msg }));
    }
  }
}

/**
 * The page's side: make an offer under `code` and wait for a machine to answer.
 * Resolves with a Peer once the data channel opens. `onStatus(text)` follows the steps.
 */
export async function offer(code, { RTC = globalThis.RTCPeerConnection, WS = globalThis.WebSocket, relay = RELAY, onStatus = () => {}, timeoutMs = 10 * 60 * 1000 } = {}) {
  const { room, key } = await secrets(code);
  const pc = new RTC({ iceServers: ICE });
  const dc = pc.createDataChannel('pet', { ordered: true });
  await pc.setLocalDescription(await pc.createOffer());
  await gathered(pc);
  const nonce = makeCode();
  const ws = await relaySocket(relay, room, WS);
  onStatus('Waiting for the machine to join with this code');
  return new Promise((resolve, reject) => {
    const t = setTimeout(() => { ws.close(); pc.close(); reject(new Error('nobody joined with this code in time')); }, timeoutMs);
    ws.onmessage = async (e) => {
      const m = await open(key, String(e.data));
      if (m?.type !== 'answer' || m.nonce !== nonce) return;
      onStatus(`Connecting to ${m.name ?? 'the machine'}`);
      await pc.setRemoteDescription({ type: 'answer', sdp: m.sdp });
    };
    dc.onopen = () => { clearTimeout(t); ws.close(); onStatus('Connected'); resolve(new Peer(dc, pc, 'p')); };
    pc.onconnectionstatechange = () => { if (pc.connectionState === 'failed') { clearTimeout(t); ws.close(); reject(new Error('WebRTC could not connect (both sides behind strict NAT?)')); } };
    seal(key, { type: 'offer', nonce, sdp: pc.localDescription.sdp, at: Date.now() }).then((s) => ws.send(s));
  });
}

/**
 * The machine's side: join `code` and answer the page's offers. Every new offer (a reload, a
 * reconnect) gets a fresh connection; `onPeer(peer)` is called for each. Returns { close }.
 */
export async function answer(code, { RTC = globalThis.RTCPeerConnection, WS = globalThis.WebSocket, relay = RELAY, name = 'pet-peer', onPeer = () => {}, onStatus = () => {} } = {}) {
  const { room, key } = await secrets(code);
  let ws = null, closed = false;
  const seen = new Set();
  const connect = async () => {
    ws = await relaySocket(relay, room, WS);
    onStatus('Joined the code; waiting for the page');
    ws.onmessage = async (e) => {
      const m = await open(key, String(e.data));
      if (m?.type !== 'offer' || seen.has(m.nonce) || Date.now() - (m.at ?? 0) > 15 * 60 * 1000) return;
      seen.add(m.nonce);
      const pc = new RTC({ iceServers: ICE });
      pc.ondatachannel = (ev) => {
        const ch = ev.channel;
        const ready = () => { onStatus('Connected'); onPeer(new Peer(ch, pc, 'm')); };
        if (ch.readyState === 'open') ready(); else ch.onopen = ready;
      };
      await pc.setRemoteDescription({ type: 'offer', sdp: m.sdp });
      await pc.setLocalDescription(await pc.createAnswer());
      await gathered(pc);
      ws.send(await seal(key, { type: 'answer', nonce: m.nonce, sdp: pc.localDescription.sdp, name }));
      onStatus('Answered; connecting');
    };
    ws.onclose = () => { if (!closed) setTimeout(() => connect().catch((err) => onStatus(err.message)), 3000); };
  };
  await connect();
  return { close: () => { closed = true; ws?.close(); } };
}
