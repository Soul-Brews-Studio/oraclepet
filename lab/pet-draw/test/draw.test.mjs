// node --test (in lab/pet-draw): the GIF encoder's output, and the MCP server over stdio.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import http from 'node:http';
import { writeFileSync, mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { encodeGif, quantize } from '../web/gif.js';

const W = 96, H = 100;
const solid = (r, g, b) => { const d = new Uint8ClampedArray(W * H * 4); for (let i = 0; i < d.length; i += 4) d.set([r, g, b, 255], i); return d; };

// a minimal GIF reader: frames, delays and the first pixel of each frame
function readGif(bytes) {
  let p = 6;
  const u16 = () => bytes[p++] | (bytes[p++] << 8);
  const w = u16(), h = u16(), packed = bytes[p]; p += 3;
  const gct = [];
  if (packed & 0x80) for (let i = 0; i < 1 << ((packed & 7) + 1); i++) gct.push([bytes[p++], bytes[p++], bytes[p++]]);
  const frames = [];
  let delay = 0;
  for (;;) {
    const b = bytes[p++];
    if (b === 0x3b) break;
    if (b === 0x21) {
      const label = bytes[p++];
      if (label === 0xf9) { p += 2; delay = u16(); p += 2; } // size, flags, delay, transparent index, terminator
      else { while (bytes[p]) p += bytes[p] + 1; p++; }
      continue;
    }
    assert.equal(b, 0x2c, `unexpected block 0x${b.toString(16)} at ${p - 1}`);
    p += 8; p++;
    const min = bytes[p++];
    const data = [];
    while (bytes[p]) { const n = bytes[p++]; data.push(...bytes.slice(p, p + n)); p += n; }
    p++;
    // decode LZW
    const clear = 1 << min, eoi = clear + 1;
    let size = min + 1, dict = [], prev = null, bit = 0;
    const out = [];
    const reset = () => { dict = Array.from({ length: clear + 2 }, (_, i) => (i < clear ? [i] : [])); size = min + 1; prev = null; };
    reset();
    for (;;) {
      let code = 0;
      for (let i = 0; i < size; i++, bit++) code |= ((data[bit >> 3] >> (bit & 7)) & 1) << i;
      if (code === clear) { reset(); continue; }
      if (code === eoi) break;
      let entry;
      if (code < dict.length) entry = dict[code];
      else entry = [...prev, prev[0]];
      out.push(...entry);
      if (prev) dict.push([...prev, entry[0]]);
      prev = entry;
      if (dict.length === 1 << size && size < 12) size++;
    }
    frames.push({ delay, pixels: out, first: gct[out[0]] });
  }
  return { w, h, frames };
}

test('encodes looping frames that decode back to the same pixels', () => {
  const frames = [{ rgba: solid(240, 168, 48), delay: 20 }, { rgba: solid(0, 0, 0), delay: 60 }];
  const g = readGif(encodeGif(frames, W, H));
  assert.equal(g.w, W); assert.equal(g.h, H);
  assert.equal(g.frames.length, 2);
  assert.deepEqual(g.frames.map((f) => f.delay), [20, 60]);
  assert.equal(g.frames[0].pixels.length, W * H);
  assert.deepEqual(g.frames[0].first, [240, 168, 48]);
  assert.deepEqual(g.frames[1].first, [0, 0, 0]);
});

test('a noisy frame (many colours, long LZW runs) round-trips through the palette', () => {
  const d = new Uint8ClampedArray(W * H * 4);
  for (let i = 0, j = 0; i < d.length; i += 4, j++) d.set([(j * 7) & 255, (j * 13) & 255, (j * 29) & 255, 255], i);
  const { palette, indexed } = quantize([{ rgba: d }]);
  assert.ok(palette.length <= 256);
  const g = readGif(encodeGif([{ rgba: d, delay: 10 }], W, H));
  assert.deepEqual(g.frames[0].pixels, [...indexed[0]]);
});

test('MCP server: initialize, tools/list, and a clear error with no page', async () => {
  const srv = spawn(process.execPath, [fileURLToPath(new URL('../mcp.mjs', import.meta.url))], {
    env: { ...process.env, PET_DRAW_OPEN: '0', PET_DRAW_PORT: '7499', PET_DRAW_WAIT: '300' },
    stdio: ['pipe', 'pipe', 'ignore'],
  });
  let buf = '';
  const waits = new Map();
  srv.stdout.on('data', (c) => { buf += c; let n; while ((n = buf.indexOf('\n')) >= 0) { const m = JSON.parse(buf.slice(0, n)); buf = buf.slice(n + 1); waits.get(m.id)?.(m); } });
  let id = 0;
  const rpc = (method, params) => new Promise((r) => { const i = ++id; waits.set(i, r); srv.stdin.write(`${JSON.stringify({ jsonrpc: '2.0', id: i, method, params })}\n`); });
  try {
    const init = await rpc('initialize', { protocolVersion: '2025-06-18', capabilities: {}, clientInfo: { name: 'test', version: '0' } });
    assert.equal(init.result.serverInfo.name, 'oraclepet-draw');
    const tools = (await rpc('tools/list', {})).result.tools.map((t) => t.name);
    assert.deepEqual(tools, ['pet_draw_status', 'pet_draw', 'pet_draw_image', 'pet_draw_view', 'pet_draw_undo', 'pet_draw_send']);
    const r = (await rpc('tools/call', { name: 'pet_draw_status', arguments: {} })).result;
    assert.equal(r.isError, true);
    assert.match(r.content[0].text, /No Draw page is connected/);
    const bad = await rpc('nope', {});
    assert.equal(bad.error.code, -32601);
  } finally {
    srv.kill();
  }
});

test('MCP server: pet_draw_image and image ops reach the page as data: URLs', async () => {
  const PORT = 7498;
  const dir = mkdtempSync(join(tmpdir(), 'pet-draw-'));
  const png = join(dir, 'dot.png');
  writeFileSync(png, Buffer.from('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg==', 'base64'));
  const srv = spawn(process.execPath, [fileURLToPath(new URL('../mcp.mjs', import.meta.url))], {
    env: { ...process.env, PET_DRAW_OPEN: '0', PET_DRAW_PORT: String(PORT), PET_DRAW_WAIT: '5000' },
    stdio: ['pipe', 'pipe', 'ignore'],
  });
  let buf = '';
  const waits = new Map();
  srv.stdout.on('data', (c) => { buf += c; let n; while ((n = buf.indexOf('\n')) >= 0) { const m = JSON.parse(buf.slice(0, n)); buf = buf.slice(n + 1); waits.get(m.id)?.(m); } });
  let id = 0;
  const rpc = (method, params) => new Promise((r) => { const i = ++id; waits.set(i, r); srv.stdin.write(`${JSON.stringify({ jsonrpc: '2.0', id: i, method, params })}\n`); });
  const seen = [];
  let sse;
  try {
    await rpc('initialize', { protocolVersion: '2025-06-18' });
    // a stand-in for the Draw page: hold the event stream, answer every call
    await new Promise((resolve, reject) => {
      const tryConnect = (left) => {
        const req = http.get({ host: '127.0.0.1', port: PORT, path: '/events', headers: { origin: 'http://127.0.0.1:5173' } }, (res) => {
          sse = res; resolve();
          let b = '';
          res.on('data', (c) => {
            b += c; let n;
            while ((n = b.indexOf('\n\n')) >= 0) {
              const ev = b.slice(0, n); b = b.slice(n + 2);
              if (!ev.startsWith('data: ')) continue;
              const msg = JSON.parse(ev.slice(6)); seen.push(msg);
              const body = JSON.stringify({ id: msg.id, ok: true, result: { notes: [], bounds: { empty: true } } });
              const post = http.request({ host: '127.0.0.1', port: PORT, path: '/result', method: 'POST', headers: { origin: 'http://127.0.0.1:5173', 'content-type': 'application/json' } });
              post.end(body);
            }
          });
        });
        req.on('error', (e) => (left > 0 ? setTimeout(() => tryConnect(left - 1), 100) : reject(e)));
      };
      tryConnect(30);
    });
    const r1 = (await rpc('tools/call', { name: 'pet_draw_image', arguments: { path: png } })).result;
    assert.notEqual(r1.isError, true, r1.content?.[0]?.text);
    assert.equal(seen[0].tool, 'draw');
    assert.equal(seen[0].args.clear, true);
    assert.equal(seen[0].args.ops[0].type, 'image');
    assert.match(seen[0].args.ops[0].src, /^data:image\/png;base64,iVBOR/);
    assert.equal(seen[0].args.ops[0].path, undefined);
    const r2 = (await rpc('tools/call', { name: 'pet_draw', arguments: { ops: [{ type: 'image', path: png, fit: 'cover' }, { type: 'rect', x: 1, y: 1, w: 2, h: 2, fill: 'red' }] } })).result;
    assert.notEqual(r2.isError, true);
    assert.match(seen[1].args.ops[0].src, /^data:image\/png;base64,/);
    assert.equal(seen[1].args.ops[0].fit, 'cover');
    assert.equal(seen[1].args.ops[1].type, 'rect');
    const bad = (await rpc('tools/call', { name: 'pet_draw_image', arguments: { path: join(dir, 'notes.txt') } })).result;
    assert.equal(bad.isError, true);
    assert.match(bad.content[0].text, /not a picture/);
  } finally {
    sse?.destroy();
    srv.kill();
  }
});
