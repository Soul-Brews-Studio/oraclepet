// OraclePet pairing relay (Cloudflare Worker + one in-memory Durable Object per room).
//
//   GET /room/<topic>   WebSocket upgrade. Every text message is passed to the other sockets in
//                       the room; the last few are kept in memory for a peer that joins later.
//
// The room name is a hash of the pairing code and every message is AES-GCM ciphertext keyed by
// that code (lab/pet-draw/web/peer.js), so this relay never sees an offer, an address or a byte of
// the session. Nothing goes to storage: the room is gone when its sockets close.
const TOPIC = /^[a-f0-9]{32}$/;
const KEEP = 8;           // messages replayed to a late joiner
const MAX_MSG = 16384;    // bytes
const MAX_SOCKETS = 4;
const IDLE_MS = 10 * 60 * 1000;

export default {
  async fetch(req, env) {
    const url = new URL(req.url);
    if (url.pathname === '/' ) return new Response('oraclepet-signal: pairing relay for https://soul-brews-studio.github.io/oraclepet/\n', { headers: { 'content-type': 'text/plain' } });
    const m = url.pathname.match(/^\/room\/([^/]+)$/);
    if (!m || !TOPIC.test(m[1])) return new Response('not found', { status: 404 });
    if (req.headers.get('upgrade') !== 'websocket') return new Response('websocket only', { status: 426 });
    return env.ROOM.get(env.ROOM.idFromName(m[1])).fetch(req);
  },
};

export class Room {
  constructor(state) {
    this.state = state;
    this.sockets = new Set();
    this.recent = [];
    this.timer = null;
  }

  async fetch() {
    if (this.sockets.size >= MAX_SOCKETS) return new Response('room full', { status: 429 });
    const [client, server] = Object.values(new WebSocketPair());
    server.accept();
    this.sockets.add(server);
    for (const m of this.recent) server.send(m);
    this.touch();
    server.addEventListener('message', (ev) => {
      const data = typeof ev.data === 'string' ? ev.data : '';
      if (!data || data.length > MAX_MSG) return;
      this.recent.push(data);
      if (this.recent.length > KEEP) this.recent.shift();
      for (const s of this.sockets) if (s !== server) { try { s.send(data); } catch {} }
      this.touch();
    });
    const drop = () => {
      this.sockets.delete(server);
      if (!this.sockets.size) this.recent = [];
    };
    server.addEventListener('close', drop);
    server.addEventListener('error', drop);
    return new Response(null, { status: 101, webSocket: client });
  }

  touch() {
    clearTimeout(this.timer);
    this.timer = setTimeout(() => { for (const s of this.sockets) { try { s.close(1000, 'idle'); } catch {} } this.sockets.clear(); this.recent = []; }, IDLE_MS);
  }
}
