// OraclePet Draw: a 96 x 100 canvas an AI (through the local MCP server, mcp.mjs) or a person draws on.
// One drawing becomes a pack of seven animated states, sent to the pet over Web Bluetooth.
import { Pet, PetError, NAME_RULE } from '../ble/oraclepet.js';
import { encodeGif } from './gif.js';

export const W = 96, H = 100;
const $ = (id) => document.getElementById(id);
const art = document.createElement('canvas');
art.width = W; art.height = H;
const ctx = art.getContext('2d', { willReadFrequently: true });
ctx.imageSmoothingEnabled = false;
ctx.fillStyle = '#000'; ctx.fillRect(0, 0, W, H);
const view = $('art');
const vctx = view.getContext('2d');
vctx.imageSmoothingEnabled = false;
const history = [];
let pet = null;
let packName = 'drawn';

// ---- log ---------------------------------------------------------------------------------------
function log(msg, cls = '') {
  const li = document.createElement('div');
  li.textContent = `${new Date().toLocaleTimeString()}  ${msg}`;
  if (cls) li.className = cls;
  $('log').append(li);
  $('log').scrollTop = $('log').scrollHeight;
}

// ---- drawing -----------------------------------------------------------------------------------
function paint() {
  vctx.clearRect(0, 0, view.width, view.height);
  vctx.drawImage(art, 0, 0, view.width, view.height);
  schedulePreview();
}
function snapshot() {
  history.push(ctx.getImageData(0, 0, W, H));
  if (history.length > 40) history.shift();
}

function points(p) {
  return (p ?? []).map((q) => (Array.isArray(q) ? q : [q.x, q.y]));
}

async function svgImage(svg) {
  const src = /^\s*<svg/i.test(svg) ? svg : `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ${W} ${H}">${svg}</svg>`;
  const withNs = src.includes('xmlns=') ? src : src.replace(/<svg/i, '<svg xmlns="http://www.w3.org/2000/svg"');
  const img = new Image();
  img.src = `data:image/svg+xml;charset=utf-8,${encodeURIComponent(withNs)}`;
  await img.decode();
  return img;
}

/** Run drawing ops (see mcp.mjs for the schema). Returns notes about ops it skipped. */
export async function draw({ clear, ops = [] } = {}) {
  snapshot();
  const notes = [];
  if (clear) { ctx.fillStyle = clear === true ? '#000' : clear; ctx.fillRect(0, 0, W, H); }
  for (const [n, op] of ops.entries()) {
    const ext = extent(op);
    if (ext && (ext[0] < 0 || ext[1] < 0 || ext[2] > W || ext[3] > H)) {
      notes.push(`op ${n} (${op.type}) covers x ${Math.round(ext[0])}..${Math.round(ext[2])}, y ${Math.round(ext[1])}..${Math.round(ext[3])}: past the ${W}x${H} canvas, that part is cut off`);
    }
    ctx.save();
    ctx.lineWidth = op.width ?? 1;
    ctx.lineCap = 'round'; ctx.lineJoin = 'round';
    const fill = op.fill, stroke = op.stroke ?? op.color;
    const finish = () => { if (fill) ctx.fill(); if (stroke) { ctx.strokeStyle = stroke; ctx.stroke(); } };
    if (fill) ctx.fillStyle = fill;
    switch (op.type) {
      case 'rect':
        ctx.beginPath(); ctx.rect(op.x, op.y, op.w, op.h); finish(); break;
      case 'ellipse':
      case 'circle':
        ctx.beginPath(); ctx.ellipse(op.cx, op.cy, op.rx ?? op.r, op.ry ?? op.r, (op.rotate ?? 0) * Math.PI / 180, 0, Math.PI * 2); finish(); break;
      case 'line':
        ctx.beginPath(); ctx.moveTo(op.x1, op.y1); ctx.lineTo(op.x2, op.y2); ctx.strokeStyle = stroke ?? '#fff'; ctx.stroke(); break;
      case 'polyline':
      case 'polygon': {
        const p = points(op.points);
        if (p.length < 2) { notes.push(`op ${n}: needs 2+ points`); break; }
        ctx.beginPath(); ctx.moveTo(...p[0]); for (const q of p.slice(1)) ctx.lineTo(...q);
        if (op.type === 'polygon') ctx.closePath();
        if (op.type === 'polyline' && !stroke) ctx.strokeStyle = '#fff', ctx.stroke(); else finish();
        break;
      }
      case 'text':
        ctx.font = `${op.bold ? 'bold ' : ''}${op.size ?? 10}px ${op.font ?? 'ui-monospace, monospace'}`;
        ctx.textAlign = op.align ?? 'left'; ctx.textBaseline = 'top';
        ctx.fillStyle = fill ?? op.color ?? '#fff'; ctx.fillText(String(op.text ?? ''), op.x, op.y); break;
      case 'pixels': {
        // rows of characters, each mapped to a colour by `palette`; '.' and ' ' are left alone
        const pal = op.palette ?? {};
        const s = op.scale ?? 1;
        (op.rows ?? []).forEach((row, y) => {
          [...row].forEach((ch, x) => {
            const c = pal[ch];
            if (!c || ch === '.' || ch === ' ') return;
            ctx.fillStyle = c; ctx.fillRect(op.x + x * s, op.y + y * s, s, s);
          });
        });
        break;
      }
      case 'svg':
        ctx.drawImage(await svgImage(op.svg), op.x ?? 0, op.y ?? 0, op.w ?? W, op.h ?? H); break;
      case 'fill':
        ctx.fillStyle = op.color ?? '#000'; ctx.fillRect(0, 0, W, H); break;
      default:
        notes.push(`op ${n}: unknown type "${op.type}"`);
    }
    ctx.restore();
  }
  paint();
  return notes;
}

/** Where an op draws: [x0, y0, x1, y1], or null when unknown (svg, fill, text). */
function extent(op) {
  const w = (op.width ?? 1) / 2;
  switch (op.type) {
    case 'rect': return [op.x, op.y, op.x + op.w, op.y + op.h];
    case 'ellipse': case 'circle': { const rx = op.rx ?? op.r, ry = op.ry ?? op.r; return [op.cx - rx - w, op.cy - ry - w, op.cx + rx + w, op.cy + ry + w]; }
    case 'line': return [Math.min(op.x1, op.x2) - w, Math.min(op.y1, op.y2) - w, Math.max(op.x1, op.x2) + w, Math.max(op.y1, op.y2) + w];
    case 'polygon': case 'polyline': { const p = points(op.points); if (!p.length) return null; const xs = p.map((q) => q[0]), ys = p.map((q) => q[1]); return [Math.min(...xs) - w, Math.min(...ys) - w, Math.max(...xs) + w, Math.max(...ys) + w]; }
    case 'pixels': { const s = op.scale ?? 1, rows = op.rows ?? []; const cols = Math.max(0, ...rows.map((r) => [...r].length)); return [op.x, op.y, op.x + cols * s, op.y + rows.length * s]; }
    default: return null;
  }
}

/** The box around everything that is not the black background, and which edges it touches. */
export function bounds() {
  const d = ctx.getImageData(0, 0, W, H).data;
  let x0 = W, y0 = H, x1 = -1, y1 = -1;
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
    const i = (y * W + x) * 4;
    if (d[i] + d[i + 1] + d[i + 2] > 24) { if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; }
  }
  if (x1 < 0) return { empty: true };
  const edges = [x0 === 0 && 'left', y0 === 0 && 'top', x1 === W - 1 && 'right', y1 === H - 1 && 'bottom'].filter(Boolean);
  return { x: x0, y: y0, w: x1 - x0 + 1, h: y1 - y0 + 1, centre: [Math.round((x0 + x1) / 2), Math.round((y0 + y1) / 2)], edges };
}

export function undo() {
  const last = history.pop();
  if (!last) return false;
  ctx.putImageData(last, 0, 0);
  paint();
  return true;
}

// ---- the seven states, made from the one drawing ---------------------------------------------
const HEART = ['.##.##.', '#######', '#######', '.#####.', '..###..', '...#...'];
function stamp(c, rows, x, y, color, s = 1) {
  c.fillStyle = color;
  rows.forEach((r, j) => [...r].forEach((ch, i) => { if (ch === '#') c.fillRect(x + i * s, y + j * s, s, s); }));
}
function frame(fn) {
  const c = document.createElement('canvas');
  c.width = W; c.height = H;
  const g = c.getContext('2d', { willReadFrequently: true });
  g.imageSmoothingEnabled = false;
  g.fillStyle = '#000'; g.fillRect(0, 0, W, H);
  fn(g);
  return g.getImageData(0, 0, W, H).data;
}
const at = (dx, dy, extra) => frame((g) => { g.drawImage(art, dx, dy); extra?.(g); });

export function states() {
  const confetti = [[8, 10, '#f0a830'], [84, 16, '#5ec8f0'], [16, 30, '#f05e8c'], [78, 40, '#8cf05e'], [50, 6, '#fff']];
  return {
    idle: [{ rgba: at(0, 0), delay: 60 }, { rgba: at(0, 1), delay: 60 }],
    busy: [0, 1, 2, 3].map((i) => ({ rgba: at(i % 2 ? 1 : -1, 0, (g) => { for (let d = 0; d <= i % 3; d++) g.fillStyle = '#f0a830', g.fillRect(80 + d * 5, 4, 3, 3); }), delay: 15 })),
    attention: [0, 1].map((i) => ({ rgba: at(0, i ? -2 : 0, (g) => { g.fillStyle = '#ffd400'; g.fillRect(88, 4, 3, 9); g.fillRect(88, 15, 3, 3); }), delay: 25 })),
    sleep: [0, 1].map((i) => ({ rgba: frame((g) => { g.drawImage(art, 0, 1); g.fillStyle = 'rgba(0,0,0,.45)'; g.fillRect(0, 0, W, H); g.fillStyle = '#9ab'; g.font = `${i ? 12 : 9}px monospace`; g.textBaseline = 'top'; g.fillText(i ? 'Z' : 'z', 80, i ? 2 : 8); }), delay: 80 })),
    celebrate: [0, -3, -5, -3].map((dy, i) => ({ rgba: at(0, dy, (g) => confetti.forEach(([x, y, c], k) => { g.fillStyle = c; g.fillRect(x + ((i + k) % 2) * 2, y + i * 3, 2, 2); })), delay: 10 })),
    dizzy: [-2, 0, 2, 0].map((dx, i) => ({ rgba: at(dx, 0, (g) => { g.fillStyle = '#ffd400'; const a = i * Math.PI / 2; g.fillRect(48 + Math.cos(a) * 14, 6 + Math.sin(a) * 4, 2, 2); g.fillRect(48 - Math.cos(a) * 14, 6 - Math.sin(a) * 4, 2, 2); }), delay: 12 })),
    heart: [1, 2].map((s) => ({ rgba: at(0, 0, (g) => stamp(g, HEART, s === 1 ? 80 : 77, s === 1 ? 4 : 2, '#f0506e', s)), delay: 30 })),
  };
}

export function buildPack(name) {
  const files = [];
  const manifest = { name, colors: { body: '#f0a830', bg: '#000000', text: '#FFFFFF', textDim: '#808080', ink: '#000000' }, states: {} };
  for (const [state, frames] of Object.entries(states())) {
    files.push({ name: `${state}.gif`, data: encodeGif(frames, W, H) });
    manifest.states[state] = `${state}.gif`;
  }
  files.push({ name: 'manifest.json', data: new TextEncoder().encode(JSON.stringify(manifest, null, 2)) });
  return { name, files, bytes: files.reduce((n, f) => n + f.data.length, 0) };
}

// state previews under the canvas
let previewTimer = 0;
const urls = [];
function schedulePreview() {
  clearTimeout(previewTimer);
  previewTimer = setTimeout(() => {
    urls.splice(0).forEach((u) => URL.revokeObjectURL(u));
    for (const [state, frames] of Object.entries(states())) {
      const u = URL.createObjectURL(new Blob([encodeGif(frames, W, H)], { type: 'image/gif' }));
      urls.push(u);
      const img = document.querySelector(`#states img[data-state="${state}"]`);
      if (img) img.src = u;
    }
  }, 250);
}

// ---- the pet over Bluetooth --------------------------------------------------------------------
function petStatus() {
  $('petstate').textContent = pet?.connected ? `Connected: ${pet.name}` : 'Not connected';
  $('petstate').className = pet?.connected ? 'pill on' : 'pill';
  $('send').disabled = !pet?.connected;
  $('connect').textContent = pet?.connected ? 'Disconnect' : 'Connect pet';
}
let sim = null; // the SimPet behind `pet`, when simulated
async function attach(next, simPet = null) {
  if (pet?.connected) await pet.disconnect().catch(() => {});
  pet = next;
  sim = simPet;
  pet.on('log', (m) => log(String(m)));
  pet.on('progress', (p) => { $('bar').value = Math.floor((p.overall ?? 0) * 100); });
  pet.on('disconnect', petStatus);
  await pet.connect();
  log(`Connected to ${pet.name}${sim ? ' (simulated)' : ''}`, 'ok');
}
$('connect').onclick = async () => {
  try {
    if (pet?.connected) { await pet.disconnect(); petStatus(); return; }
    await attach(await Pet.request());
  } catch (e) { if (e?.name !== 'NotFoundError') log(e.message, 'bad'); }
  petStatus();
};
$('sim').onclick = async () => {
  try {
    const { SimPet } = await import('../ble/simpet.js');
    const s = new SimPet();
    await attach(new Pet(s), s);
  } catch (e) { log(e.message, 'bad'); }
  petStatus();
};

export async function sendToPet(name = packName) {
  if (!pet?.connected) throw new PetError('No pet connected: press "Connect pet" on the Draw page first');
  if (!NAME_RULE.test(name)) throw new PetError(`Pack name "${name}" must be 1 to 23 characters of a-z 0-9 - _`);
  const pack = buildPack(name);
  log(`Sending pack ${name} (${(pack.bytes / 1024).toFixed(1)} KB)`);
  await pet.pack(pack);
  log(`Pet shows ${name}${sim ? ` (simulated: ${[...sim.fs.keys()].filter((k) => k.startsWith(`characters/${name}/`)).length} files, showing ${sim.pack})` : ''}`, 'ok');
  return { name, bytes: pack.bytes, files: pack.files.map((f) => f.name) };
}
$('send').onclick = async () => {
  packName = $('name').value.trim().toLowerCase() || 'drawn';
  try { await sendToPet(packName); } catch (e) { log(e.message, 'bad'); }
};
$('download').onclick = () => {
  packName = $('name').value.trim().toLowerCase() || 'drawn';
  const pack = buildPack(packName);
  for (const f of pack.files) {
    const a = document.createElement('a');
    a.href = URL.createObjectURL(new Blob([f.data]));
    a.download = `${packName}-${f.name}`;
    a.click();
  }
};

// ---- a person draws too ------------------------------------------------------------------------
let drawing = false;
function penAt(e) {
  const r = view.getBoundingClientRect();
  const x = Math.floor(((e.clientX - r.left) * W) / r.width), y = Math.floor(((e.clientY - r.top) * H) / r.height);
  const s = Number($('pen').value);
  ctx.fillStyle = $('erase').checked ? '#000' : $('color').value;
  ctx.fillRect(x - Math.floor(s / 2), y - Math.floor(s / 2), s, s);
  paint();
}
view.onpointerdown = (e) => { drawing = true; snapshot(); view.setPointerCapture(e.pointerId); penAt(e); };
view.onpointermove = (e) => { if (drawing) penAt(e); };
view.onpointerup = () => { drawing = false; };
$('undo').onclick = () => undo();
$('clear').onclick = () => draw({ clear: '#000' });

// ---- the AI: the local MCP server (mcp.mjs) over Server-Sent Events + POST --------------------
const params = new URLSearchParams(location.search);
let port = Number(params.get('mcp')) || 7414;
let es = null;
function aiStatus(on, text) {
  $('aistate').textContent = text;
  $('aistate').className = on ? 'pill on' : 'pill';
  $('aiconnect').textContent = es ? 'Stop' : 'Connect AI';
}
async function reply(id, body) {
  await fetch(`http://127.0.0.1:${port}/result`, { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ id, ...body }) });
}
const handlers = {
  status: () => ({ size: [W, H], pet: pet?.connected ? { name: pet.name, info: pet.info ?? null } : null, packName, page: location.href }),
  draw: async (a) => ({ notes: await draw(a), bounds: bounds() }),
  undo: () => ({ undone: undo(), bounds: bounds() }),
  view: (a) => {
    const s = Math.max(1, Math.min(8, a?.scale ?? 4));
    const c = document.createElement('canvas');
    if (a?.states) {
      const all = Object.entries(states());
      c.width = all.length * W * 2; c.height = H * 2 + 14;
      const g = c.getContext('2d'); g.imageSmoothingEnabled = false; g.fillStyle = '#111'; g.fillRect(0, 0, c.width, c.height);
      all.forEach(([name, frames], i) => {
        const t = document.createElement('canvas'); t.width = W; t.height = H;
        t.getContext('2d').putImageData(new ImageData(new Uint8ClampedArray(frames[frames.length - 1].rgba), W, H), 0, 0);
        g.drawImage(t, i * W * 2, 14, W * 2, H * 2); g.fillStyle = '#ddd'; g.font = '11px monospace'; g.fillText(name, i * W * 2 + 4, 11);
      });
    } else {
      c.width = W * s; c.height = H * s;
      const g = c.getContext('2d'); g.imageSmoothingEnabled = false; g.drawImage(art, 0, 0, c.width, c.height);
    }
    return { png: c.toDataURL('image/png').split(',')[1] };
  },
  send: (a) => sendToPet(a?.name ?? packName),
};
function connectAI() {
  if (es) { es.close(); es = null; aiStatus(false, 'Not connected'); return; }
  aiStatus(false, `Looking for the MCP server on 127.0.0.1:${port}…`);
  es = new EventSource(`http://127.0.0.1:${port}/events`);
  es.onopen = () => { aiStatus(true, `AI connected (127.0.0.1:${port})`); log('AI connected', 'ok'); };
  es.onerror = () => {
    aiStatus(false, `Waiting for the MCP server on 127.0.0.1:${port}…`);
    // a blocked request (local network permission, CORS) closes an EventSource for good: open a new one
    if (es?.readyState === EventSource.CLOSED) { es = null; setTimeout(() => { if (!es) connectAI(); }, 3000); }
  };
  es.onmessage = async (ev) => {
    const { id, tool, args } = JSON.parse(ev.data);
    $('aistate').textContent = `AI: ${tool}`;
    try {
      const h = handlers[tool];
      if (!h) throw new Error(`unknown tool ${tool}`);
      const result = await h(args ?? {});
      if (tool !== 'status' && tool !== 'view') log(`AI ${tool}${args?.ops ? ` · ${args.ops.length} ops` : ''}`);
      await reply(id, { ok: true, result });
    } catch (e) {
      log(`AI ${tool}: ${e.message}`, 'bad');
      await reply(id, { ok: false, error: e.message }).catch(() => {});
    }
    aiStatus(true, `AI connected (127.0.0.1:${port})`);
  };
  aiStatus(false, `Looking for the MCP server on 127.0.0.1:${port}…`);
}
$('aiconnect').onclick = connectAI;
$('mcpcmd').textContent = `curl -fsSLO ${new URL('pet-draw-mcp.mjs', location.href).href.split('?')[0]}\nclaude mcp add pet-draw -- node "$PWD/pet-draw-mcp.mjs"`;
if (params.has('mcp')) connectAI();

paint();
petStatus();
