#!/usr/bin/env node
// OraclePet Draw MCP server: lets an AI agent draw on the OraclePet Draw page and send the drawing to a pet.
//
//   claude mcp add pet-draw -- node /path/to/pet-draw-mcp.mjs
//
// No dependencies (Node 18+). MCP over stdio for the agent; a small HTTP bridge on 127.0.0.1 for the page:
// the page holds one Server-Sent Events stream (GET /events) that carries tool calls, and POSTs each result
// back to /result. Only the page's origin (and localhost) may connect. The page draws on its canvas and talks
// to the pet over Web Bluetooth, so this process never touches Bluetooth.
//
// Env: PET_DRAW_PORT (7414), PET_DRAW_PAGE (the Draw page URL), PET_DRAW_OPEN=0 (do not open a browser),
//      PET_DRAW_ORIGINS (more allowed origins, comma separated), PET_DRAW_WAIT (ms to wait for the page, 45000).
import http from 'node:http';
import { spawn } from 'node:child_process';

const VERSION = '1.0.0';
const PORT = Number(process.env.PET_DRAW_PORT) || 7414;
const PAGE = process.env.PET_DRAW_PAGE || 'https://soul-brews-studio.github.io/oraclepet/draw/';
const ORIGINS = new Set(['https://soul-brews-studio.github.io', ...(process.env.PET_DRAW_ORIGINS || '').split(',').map((s) => s.trim()).filter(Boolean)]);
const WAIT = Number(process.env.PET_DRAW_WAIT) || 45000;
const LOCAL = /^https?:\/\/(localhost|127\.0\.0\.1|\[::1\])(:\d+)?$/;
const log = (...a) => process.stderr.write(`[pet-draw] ${a.join(' ')}\n`);

// ---- the page bridge ---------------------------------------------------------------------------
let page = null; // the live SSE response
let pageWaiters = [];
const pending = new Map();
let nextId = 1;
let opened = false;
let listening = false;
let listenError = '';

function cors(req, res) {
  const origin = req.headers.origin;
  if (origin && !ORIGINS.has(origin) && !LOCAL.test(origin)) return false;
  if (origin) {
    res.setHeader('Access-Control-Allow-Origin', origin);
    res.setHeader('Vary', 'Origin');
  }
  res.setHeader('Access-Control-Allow-Methods', 'GET, POST, OPTIONS');
  res.setHeader('Access-Control-Allow-Headers', 'content-type');
  res.setHeader('Access-Control-Allow-Private-Network', 'true');
  return true;
}

const server = http.createServer((req, res) => {
  if (!cors(req, res)) { res.writeHead(403).end('origin not allowed'); return; }
  const url = new URL(req.url, 'http://127.0.0.1');
  if (req.method === 'OPTIONS') { res.writeHead(204).end(); return; }
  if (req.method === 'GET' && url.pathname === '/hello') {
    res.writeHead(200, { 'content-type': 'application/json' }).end(JSON.stringify({ name: 'oraclepet-draw', version: VERSION, page: !!page }));
    return;
  }
  if (req.method === 'GET' && url.pathname === '/events') {
    if (page) page.end(); // the newest page wins
    res.writeHead(200, { 'content-type': 'text/event-stream', 'cache-control': 'no-store', connection: 'keep-alive' });
    res.write(': hello\n\n');
    page = res;
    log('page connected');
    const ping = setInterval(() => res.write(': ping\n\n'), 15000);
    req.on('close', () => {
      clearInterval(ping);
      if (page === res) { page = null; log('page left'); }
    });
    pageWaiters.splice(0).forEach((w) => w());
    return;
  }
  if (req.method === 'POST' && url.pathname === '/result') {
    let body = '';
    req.on('data', (c) => { body += c; if (body.length > 8e6) req.destroy(); });
    req.on('end', () => {
      try {
        const msg = JSON.parse(body);
        const p = pending.get(msg.id);
        if (p) { pending.delete(msg.id); clearTimeout(p.timer); msg.ok ? p.resolve(msg.result) : p.reject(new Error(msg.error || 'failed on the page')); }
        res.writeHead(204).end();
      } catch { res.writeHead(400).end(); }
    });
    return;
  }
  res.writeHead(404).end();
});
server.on('error', (e) => { listenError = e.code === 'EADDRINUSE' ? `port ${PORT} is taken (another pet-draw server?); set PET_DRAW_PORT` : e.message; log(listenError); });
server.listen(PORT, '127.0.0.1', () => { listening = true; log(`bridge on http://127.0.0.1:${PORT}`); });

function openPage() {
  if (opened || process.env.PET_DRAW_OPEN === '0') return;
  opened = true;
  const url = `${PAGE}?mcp=${PORT}`;
  const cmd = process.platform === 'darwin' ? ['open', [url]] : process.platform === 'win32' ? ['cmd', ['/c', 'start', '', url]] : ['xdg-open', [url]];
  try { spawn(cmd[0], cmd[1], { stdio: 'ignore', detached: true }).unref(); log(`opened ${url}`); } catch (e) { log(`could not open a browser: ${e.message}`); }
}

async function waitForPage(ms) {
  if (page) return true;
  openPage();
  return new Promise((resolve) => {
    const done = (v) => { clearTimeout(t); pageWaiters = pageWaiters.filter((w) => w !== ok); resolve(v); };
    const ok = () => done(true);
    const t = setTimeout(() => done(false), ms);
    pageWaiters.push(ok);
  });
}

async function call(tool, args, timeoutMs = 30000) {
  if (!listening) throw new Error(listenError || 'the page bridge is not up');
  if (!(await waitForPage(WAIT))) {
    throw new Error(`No Draw page is connected. Open ${PAGE}?mcp=${PORT} in Chrome and press "Connect AI" (allow access to this device if asked).`);
  }
  const id = nextId++;
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => { pending.delete(id); reject(new Error(`the page did not answer ${tool} in ${timeoutMs / 1000} s`)); }, timeoutMs);
    pending.set(id, { resolve, reject, timer });
    page.write(`data: ${JSON.stringify({ id, tool, args })}\n\n`);
  });
}

// ---- MCP ---------------------------------------------------------------------------------------
const color = { type: 'string', description: 'CSS colour, e.g. "#f0a830" or "white"' };
const num = { type: 'number' };
const TOOLS = [
  {
    name: 'pet_draw_status',
    description: 'Is the Draw page open, is a pet connected over Bluetooth, and the canvas size. Opens the page in the browser if none is connected.',
    inputSchema: { type: 'object', properties: {} },
  },
  {
    name: 'pet_draw',
    description:
      'Draw on the pet canvas: 96 x 100 pixels, origin top left, black background; the pet shows it 3x bigger. ' +
      'The drawing is one character pose: keep the figure centred and inside x 8..88, y 8..96 (about 60-80 px tall), bright colours with a light outline (dark colours vanish on black). ' +
      'Mind the size of pixel art: a row of N characters at scale s is N*s px wide (40 characters at scale 2 = 80 px). Every reply says where the drawing sits and warns when it touches an edge. ' +
      'Ops run in order. Types: rect {x,y,w,h,fill?,stroke?,width?}; ellipse/circle {cx,cy,rx,ry | r, rotate?, fill?, stroke?, width?}; ' +
      'line {x1,y1,x2,y2,stroke,width?}; polygon/polyline {points:[[x,y],...], fill?, stroke?, width?}; text {x,y,text,size?,fill?,bold?,align?}; ' +
      'pixels {x,y,rows:["..##..",...], palette:{"#":"#fff","o":"#f0a830"}, scale?} for exact pixel art ("." and " " are transparent); ' +
      'svg {svg: "<svg viewBox=\\"0 0 96 100\\">...</svg>" or its inner elements, x?,y?,w?,h?}; fill {color} paints everything. ' +
      'Set clear (true or a colour) to start from an empty canvas. Look at the result with pet_draw_view.',
    inputSchema: {
      type: 'object',
      properties: {
        clear: { description: 'true (black) or a colour: wipe the canvas first', anyOf: [{ type: 'boolean' }, { type: 'string' }] },
        ops: {
          type: 'array',
          items: {
            type: 'object',
            properties: {
              type: { type: 'string', enum: ['rect', 'ellipse', 'circle', 'line', 'polygon', 'polyline', 'text', 'pixels', 'svg', 'fill'] },
              x: num, y: num, w: num, h: num, cx: num, cy: num, r: num, rx: num, ry: num, rotate: num,
              x1: num, y1: num, x2: num, y2: num, width: num, size: num, scale: num,
              fill: color, stroke: color, color,
              points: { type: 'array', items: { type: 'array', items: num } },
              text: { type: 'string' }, bold: { type: 'boolean' }, align: { type: 'string', enum: ['left', 'center', 'right'] },
              rows: { type: 'array', items: { type: 'string' } },
              palette: { type: 'object', additionalProperties: { type: 'string' } },
              svg: { type: 'string' },
            },
            required: ['type'],
          },
        },
      },
      required: ['ops'],
    },
  },
  {
    name: 'pet_draw_view',
    description: 'See the canvas as a PNG. states=true shows the seven animations made from it (idle, busy, attention, sleep, celebrate, dizzy, heart) side by side.',
    inputSchema: { type: 'object', properties: { states: { type: 'boolean' }, scale: { type: 'number', description: '1-8, default 4' } } },
  },
  {
    name: 'pet_draw_undo',
    description: 'Undo the last pet_draw call (or the last stroke a person painted).',
    inputSchema: { type: 'object', properties: {} },
  },
  {
    name: 'pet_draw_send',
    description: 'Make the drawing into a character pack (seven animated GIFs + manifest.json) and send it to the connected pet over Bluetooth; the pet shows it when done. A person must have pressed "Connect pet" on the page.',
    inputSchema: { type: 'object', properties: { name: { type: 'string', description: 'pack name: 1-23 of a-z 0-9 - _ (default "drawn")' } } },
  },
];

function describe(b) {
  if (!b) return '';
  if (b.empty) return 'The canvas is empty (all black).';
  const where = `The drawing covers x ${b.x}..${b.x + b.w - 1}, y ${b.y}..${b.y + b.h - 1} (${b.w}x${b.h} px, centre ${b.centre.join(',')}) of the 96x100 canvas.`;
  return b.edges.length ? `${where} WARNING: it touches the ${b.edges.join(', ')} edge, so it is probably cut off: make it smaller or move it in.` : where;
}

const text = (t) => ({ content: [{ type: 'text', text: typeof t === 'string' ? t : JSON.stringify(t, null, 2) }] });

async function runTool(name, args) {
  switch (name) {
    case 'pet_draw_status': return text(await call('status', {}));
    case 'pet_draw': {
      const r = await call('draw', args);
      return text([`Drawn ${args.ops?.length ?? 0} ops.`, ...(r.notes ?? []), describe(r.bounds), 'Check it with pet_draw_view.'].filter(Boolean).join('\n'));
    }
    case 'pet_draw_view': {
      const r = await call('view', args);
      return { content: [{ type: 'image', data: r.png, mimeType: 'image/png' }] };
    }
    case 'pet_draw_undo': { const r = await call('undo', {}); return text(`${r.undone ? 'Undone.' : 'Nothing to undo.'}\n${describe(r.bounds)}`); }
    case 'pet_draw_send': {
      const r = await call('send', args, 240000);
      return text(`Sent pack "${r.name}" (${(r.bytes / 1024).toFixed(1)} KB: ${r.files.join(', ')}). The pet shows it now.`);
    }
    default: throw new Error(`unknown tool ${name}`);
  }
}

function send(msg) { process.stdout.write(`${JSON.stringify(msg)}\n`); }

async function handle(msg) {
  const { id, method, params } = msg;
  const reply = (result) => id !== undefined && send({ jsonrpc: '2.0', id, result });
  const fail = (code, message) => id !== undefined && send({ jsonrpc: '2.0', id, error: { code, message } });
  switch (method) {
    case 'initialize':
      return reply({
        protocolVersion: params?.protocolVersion || '2025-06-18',
        capabilities: { tools: {} },
        serverInfo: { name: 'oraclepet-draw', version: VERSION },
        instructions: 'Draw a desk-pet character on a 96x100 canvas (pet_draw), look at it (pet_draw_view), fix it, then send it to the pet (pet_draw_send).',
      });
    case 'ping': return reply({});
    case 'tools/list': return reply({ tools: TOOLS });
    case 'tools/call':
      try { return reply(await runTool(params?.name, params?.arguments ?? {})); }
      catch (e) { return reply({ content: [{ type: 'text', text: e.message }], isError: true }); }
    default:
      if (method?.startsWith('notifications/')) return;
      return fail(-32601, `method not found: ${method}`);
  }
}

let buf = '';
process.stdin.setEncoding('utf8');
process.stdin.on('data', (chunk) => {
  buf += chunk;
  let nl;
  while ((nl = buf.indexOf('\n')) >= 0) {
    const line = buf.slice(0, nl).trim();
    buf = buf.slice(nl + 1);
    if (!line) continue;
    let msg;
    try { msg = JSON.parse(line); } catch { send({ jsonrpc: '2.0', id: null, error: { code: -32700, message: 'parse error' } }); continue; }
    handle(msg).catch((e) => log(e.stack || e.message));
  }
});
process.stdin.on('end', () => process.exit(0));
