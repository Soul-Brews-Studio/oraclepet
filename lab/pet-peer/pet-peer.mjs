#!/usr/bin/env node
// pet-peer: a computer paired with the OraclePet Studio page (GitHub Pages) over WebRTC.
//
//   node pet-peer.mjs --pair 7KQ2-M9XD-PL4C        builds for the page (apps, firmware)
//   claude mcp add pet-peer -- node /path/pet-peer.mjs
//                                                   + an AI agent draws in the page (design_* tools)
//
// The page shows the pairing code. Both sides meet on the relay (tools/signal) with messages
// encrypted by that code, then talk peer to peer; the relay never sees the session.
// Needs Node 18+ and, once, node-datachannel (WebRTC for Node): installed on first use into
// ~/.cache/oraclepet-peer unless it resolves already. Builds need this repo's lab/ (PET_LAB or --lab):
// zig for apps, ESP-IDF v6 (IDF_PATH, default ~/esp/esp-idf-v6) for firmware.
import { spawn, execFileSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync, readdirSync, writeFileSync } from 'node:fs';
import { homedir, hostname, tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { answer, normCode, Peer } from './peer.js';

const VERSION = '1.0.0';
const HERE = dirname(fileURLToPath(import.meta.url));
const argv = process.argv.slice(2);
const opt = (k) => { const i = argv.indexOf(k); return i >= 0 ? argv[i + 1] : undefined; };
const LAB = resolve(opt('--lab') ?? process.env.PET_LAB ?? join(HERE, '..'));
const IDF = process.env.IDF_PATH ?? join(homedir(), 'esp/esp-idf-v6');
const NAME = opt('--name') ?? hostname().replace(/\.local$/, '');
const log = (...a) => process.stderr.write(`[pet-peer] ${a.join(' ')}\n`);

// ---- WebRTC for Node -------------------------------------------------------------------------------
const NDC = 'node-datachannel@0.33.6';
async function rtc() {
  try { return (await import('node-datachannel/polyfill')).RTCPeerConnection; } catch {}
  const dir = join(homedir(), '.cache/oraclepet-peer');
  const entry = join(dir, 'node_modules/node-datachannel/dist/esm/polyfill/index.mjs');
  if (!existsSync(entry)) {
    log(`installing ${NDC} (WebRTC for Node) into ${dir}, once`);
    mkdirSync(dir, { recursive: true });
    execFileSync(process.platform === 'win32' ? 'npm.cmd' : 'npm', ['install', '--no-audit', '--no-fund', '--prefix', dir, NDC], { stdio: ['ignore', 'ignore', 'inherit'] });
  }
  return (await import(pathToFileURL(entry).href)).RTCPeerConnection;
}

// ---- what this machine can build ---------------------------------------------------------------------
const has = (cmd) => { try { execFileSync('sh', ['-c', `command -v ${cmd}`], { stdio: 'ignore' }); return true; } catch { return false; } };
const caps = () => ({
  app: existsSync(join(LAB, 'pet-sdk/build.sh')) && has('zig'),
  firmware: existsSync(join(LAB, 'jc3248-pet-idf/CMakeLists.txt')) && existsSync(join(IDF, 'export.sh')),
});

let queue = Promise.resolve();
const serial = (fn) => { const p = queue.then(fn, fn); queue = p.catch(() => {}); return p; };

function run(cmd, args, { cwd, onLine } = {}) {
  return new Promise((resolve, reject) => {
    const p = spawn(cmd, args, { cwd, env: process.env });
    const out = [];
    let tail = '';
    const take = (b) => {
      tail += b;
      const lines = tail.split(/\r?\n/);
      tail = lines.pop();
      for (const l of lines) { out.push(l); if (out.length > 400) out.shift(); onLine?.(l); }
    };
    p.stdout.on('data', take);
    p.stderr.on('data', take);
    p.on('error', reject);
    p.on('close', (code) => { if (tail) out.push(tail); resolve({ code, log: out.join('\n') }); });
  });
}

const NAME_RULE = /^[a-z0-9_-]{1,23}$/;
async function buildApp({ name, lang = 'c', source }, peer) {
  if (!caps().app) throw new Error(`this machine cannot build apps: needs zig and ${join(LAB, 'pet-sdk')}`);
  if (!NAME_RULE.test(String(name))) throw new Error('app name: 1 to 23 of a-z 0-9 - _');
  if (!['c', 'zig'].includes(lang)) throw new Error('lang: c or zig');
  if (typeof source !== 'string' || !source.trim() || source.length > 200000) throw new Error('source: the program text (up to 200 KB)');
  return serial(async () => {
    const dir = join(tmpdir(), 'pet-peer-src');
    mkdirSync(dir, { recursive: true });
    const src = join(dir, `${name}.${lang}`);
    writeFileSync(src, source);
    const out = join(LAB, 'pet-sdk/build', `${name}.wasm`);
    const before = existsSync(out) ? readFileSync(out) : null;
    const t = Date.now();
    log(`build app ${name}.${lang}`);
    const r = await run('bash', [join(LAB, 'pet-sdk/build.sh'), src], { onLine: (line) => peer.note('log', { line }) });
    // build.sh's last step (the WAMR load check) may fail on its own; the .wasm is what counts
    const ok = existsSync(out) && (!before || !readFileSync(out).equals(before) || r.log.includes('✅'));
    if (!ok || !r.log.includes('✅')) throw new Error(`build failed:\n${r.log.split('\n').slice(-25).join('\n')}`);
    const data = readFileSync(out);
    log(`built ${name}.wasm ${data.length} bytes in ${Date.now() - t} ms`);
    return { name, bytes: data.length, ms: Date.now() - t, log: r.log.split('\n').slice(-12).join('\n'), data: Peer.bytes(data) };
  });
}

const BOARDS = { jc3248: { dir: 'build', bin: 'jc3248_pet_idf.bin' }, sc01: { dir: 'build-sc01', bin: 'oraclepet_sc01.bin' }, lcd28: { dir: 'build-lcd28', bin: 'oraclepet_lcd28.bin' } };
async function buildFirmware({ board = 'jc3248', pack = 'siamese' }, peer) {
  if (!caps().firmware) throw new Error(`this machine cannot build firmware: needs ESP-IDF v6 at ${IDF} (IDF_PATH) and ${join(LAB, 'jc3248-pet-idf')}`);
  const b = BOARDS[board];
  if (!b) throw new Error(`board: ${Object.keys(BOARDS).join(', ')}`);
  if (!NAME_RULE.test(String(pack))) throw new Error('pack: 1 to 23 of a-z 0-9 - _');
  return serial(async () => {
    const proj = join(LAB, 'jc3248-pet-idf');
    const extra = board === 'jc3248' ? '' : ` -DSDKCONFIG=${b.dir}/sdkconfig`;
    // export.sh takes the first python3 on PATH and wants the venv ESP-IDF made for that version; when
    // they differ it prints an error and still returns 0. Point it at an installed v6.0 venv instead.
    const envs = join(homedir(), '.espressif/python_env');
    const venv = process.env.IDF_PYTHON_ENV_PATH ?? (existsSync(envs) ? readdirSync(envs).filter((d) => /^idf6\.0_py/.test(d)).sort().map((d) => join(envs, d)).pop() : undefined);
    const cmd = `${venv ? `export IDF_PYTHON_ENV_PATH="${venv}"; export PATH="${venv}/bin:$PATH"; ` : ''}. "${IDF}/export.sh" > /tmp/pet-peer-idf-export.log 2>&1; ` +
      `command -v idf.py >/dev/null || { echo "ESP-IDF did not load:"; tail -5 /tmp/pet-peer-idf-export.log; exit 3; }; ` +
      `idf.py -B ${b.dir} -DPET_BOARD=${board}${extra} -DPET_DEFAULT_PACK=${pack} build`;
    const t = Date.now();
    log(`build firmware ${board} · ${pack}`);
    let last = 0;
    const r = await run('bash', ['-c', cmd], { cwd: proj, onLine: (line) => { if (/^\[\d+\/\d+\]|error|Project build complete/i.test(line) && Date.now() - last > 1500) { last = Date.now(); peer.note('log', { line: line.slice(0, 160) }); } } });
    const out = join(proj, b.dir, b.bin);
    if (r.code !== 0 || !existsSync(out)) throw new Error(`firmware build failed:\n${r.log.split('\n').slice(-25).join('\n')}`);
    const data = readFileSync(out);
    log(`built ${b.bin} ${data.length} bytes in ${Math.round((Date.now() - t) / 1000)} s`);
    return { board, pack, bytes: data.length, ms: Date.now() - t, log: r.log.split('\n').filter((l) => /Project build complete|binary size|bytes/i.test(l)).slice(-4).join('\n'), data: Peer.bytes(data) };
  });
}

// ---- the link ------------------------------------------------------------------------------------------
let page = null;       // the Peer of the paired page
let pairing = null;    // { code, close }
let pageInfo = null;
const waiters = [];

async function pairWith(code) {
  const c = normCode(code);
  if (c.length !== 12) throw new Error('the pairing code has 12 letters and digits, like 7KQ2-M9XD-PL4C (the Studio page shows it)');
  pairing?.close();
  const RTC = await rtc();
  pairing = { code: c, ...(await answer(c, {
    RTC, name: NAME,
    onStatus: (s) => log(s),
    onPeer: (p) => {
      page?.close();
      page = p;
      p.handle('hello', () => ({ name: NAME, version: VERSION, caps: caps() }));
      const logged = (what, fn) => (a) => fn(a, p).catch((e) => { log(`${what} failed: ${e.message.split('\n').slice(0, 6).join(' | ')}`); throw e; });
      p.handle('build_app', logged('build app', buildApp));
      p.handle('build_firmware', logged('build firmware', buildFirmware));
      p.addEventListener('close', () => { if (page === p) { page = null; pageInfo = null; log('the page left (it can pair again with the same code)'); } });
      p.call('hello', {}).then((i) => { pageInfo = i; }).catch(() => {});
      log('paired with the page');
      waiters.splice(0).forEach((w) => w());
    },
  })) };
}

async function toPage(action, args, timeoutMs = 60000) {
  if (!page?.open) {
    if (!pairing) throw new Error('Not paired. In the Studio (https://soul-brews-studio.github.io/oraclepet/studio/) press "Pair a machine", then call studio_pair with the code it shows.');
    await new Promise((resolve, reject) => { const t = setTimeout(() => reject(new Error('the page is not connected: is the Studio open with that code?')), 30000); waiters.push(() => { clearTimeout(t); resolve(); }); });
  }
  return page.call('action', { action, args }, timeoutMs);
}

// ---- MCP (stdio) ------------------------------------------------------------------------------------------
const obj = (properties = {}, required = []) => ({ type: 'object', properties, required });
const n = { type: 'number' }, s = { type: 'string' };
const TOOLS = [
  ['studio_pair', 'Pair with the OraclePet Studio page: the person presses "Pair a machine" there and gives you the code it shows (like 7KQ2-M9XD-PL4C).', obj({ code: s }, ['code'])],
  ['studio_status', 'Paired or not, what the page reports, and what this machine can build.', obj()],
  ['design_view', 'The designer: current state and frame, frames drawn per state, layers, tool, colour; with a picture of the artboard.', obj()],
  ['design_screenshot', 'A picture of the artboard as the person sees it.', obj({ width: n })],
  ['design_shape', 'Add a filled rectangle or ellipse. Artboard 384 x 400 px (the pet frame 96 x 100 at 4x), origin top left; x, y are the top-left corner.', obj({ kind: { type: 'string', enum: ['rect', 'ellipse'] }, x: n, y: n, w: n, h: n, color: s, opacity: n, radius: n })],
  ['design_stroke', 'A brush stroke through points [[x,y],...] (artboard px); erase: true rubs out.', obj({ points: { type: 'array', items: { type: 'array', items: n } }, color: s, size: n, erase: { type: 'boolean' } }, ['points'])],
  ['design_text', 'Add text (bold) at x, y.', obj({ text: s, x: n, y: n, size: n, color: s })],
  ['design_svg', 'Add SVG markup as layers (one, or one per element with split: true). Best for clean shapes and outlines.', obj({ svg: s, x: n, y: n, width: n, split: { type: 'boolean' }, name: s }, ['svg'])],
  ['design_image', 'Place a picture from a URL (https or data:) as a layer.', obj({ url: s, x: n, y: n, width: n, remove_background: { type: 'boolean' } }, ['url'])],
  ['design_layer', 'Change a layer by name: op select, update (props x, y, width, height, color, opacity, angle), up, down, top, bottom, delete, show, hide, lock, unlock.', obj({ name: s, op: s, props: { type: 'object' } }, ['name'])],
  ['design_goto', 'Open a state and frame (1-based): idle, busy, attention, celebrate, dizzy, sleep, heart. States you leave empty move the idle drawing.', obj({ state: s, frame: n }, ['state'])],
  ['design_frame', 'Frames of the current state: op add, duplicate or delete.', obj({ op: s }, ['op'])],
  ['design_clear', 'Clear the current frame.', obj()],
  ['design_undo', 'Undo the last change.', obj()],
  ['design_create_sprite', 'Make the character pack from the frames (idle frame 1 is the base; the rest move it).', obj({ name: s, fps: n })],
  ['design_send_to_pet', 'Send the pack to the pet the page is connected to over Bluetooth.', obj({ name: s })],
  ['studio_call', 'Any designer action by name (view, goto, frame, stroke, shape, text, image, svg, layer, clear, undo, redo, tool, focus, snapshot, query, screenshot, tap, drag, type, key, since, create_sprite, send_to_pet).', obj({ action: s, args: { type: 'object' } }, ['action'])],
].map(([name, description, inputSchema]) => ({ name, description, inputSchema }));

const text = (t) => ({ content: [{ type: 'text', text: typeof t === 'string' ? t : JSON.stringify(t, null, 2) }] });
function withPicture(result, key = 'png') {
  const png = result?.[key];
  const rest = { ...result }; delete rest[key];
  const content = [{ type: 'text', text: JSON.stringify(rest, null, 2) }];
  if (typeof png === 'string' && png.startsWith('data:image/')) content.push({ type: 'image', data: png.split(',')[1], mimeType: png.slice(5, png.indexOf(';')) });
  return { content };
}

async function runTool(name, a) {
  switch (name) {
    case 'studio_pair': {
      await pairWith(a.code);
      const ok = await new Promise((r) => { if (page?.open) return r(true); const t = setTimeout(() => r(false), 30000); waiters.push(() => { clearTimeout(t); r(true); }); });
      return text(ok ? `Paired with the Studio page (${NAME}). Draw with the design_* tools; the person sees every change.` : 'Joined the code, but the page has not connected yet: is the Studio still showing that code?');
    }
    case 'studio_status': return text({ machine: NAME, paired: !!page?.open, code: pairing?.code ?? null, page: pageInfo, caps: caps(), lab: LAB });
    case 'design_view': return withPicture(await toPage('view', {}));
    case 'design_screenshot': return withPicture(await toPage('screenshot', a));
    case 'design_create_sprite': return text(await toPage('create_sprite', a, 120000));
    case 'design_send_to_pet': return text(await toPage('send_to_pet', a, 300000));
    case 'studio_call': return withPicture(await toPage(a.action, a.args ?? {}, 300000));
    default:
      if (name.startsWith('design_')) return text(await toPage(name.slice(7), a));
      throw new Error(`unknown tool ${name}`);
  }
}

function mcp() {
  const send = (m) => process.stdout.write(`${JSON.stringify(m)}\n`);
  const handle = async ({ id, method, params }) => {
    const reply = (result) => id !== undefined && send({ jsonrpc: '2.0', id, result });
    if (method === 'initialize') return reply({ protocolVersion: params?.protocolVersion || '2025-06-18', capabilities: { tools: {} }, serverInfo: { name: 'oraclepet-peer', version: VERSION },
      instructions: 'Pair with the OraclePet Studio page (studio_pair with the code the person reads you), then draw the pet character with design_* tools, look with design_view, make it with design_create_sprite and send it with design_send_to_pet.' });
    if (method === 'ping') return reply({});
    if (method === 'tools/list') return reply({ tools: TOOLS });
    if (method === 'tools/call') {
      try { return reply(await runTool(params?.name, params?.arguments ?? {})); }
      catch (e) { return reply({ content: [{ type: 'text', text: e.message }], isError: true }); }
    }
    if (!method?.startsWith('notifications/') && id !== undefined) send({ jsonrpc: '2.0', id, error: { code: -32601, message: `method not found: ${method}` } });
  };
  let buf = '';
  process.stdin.setEncoding('utf8');
  process.stdin.on('data', (c) => {
    buf += c;
    let i;
    while ((i = buf.indexOf('\n')) >= 0) {
      const line = buf.slice(0, i).trim(); buf = buf.slice(i + 1);
      if (!line) continue;
      try { handle(JSON.parse(line)).catch((e) => log(e.message)); } catch { send({ jsonrpc: '2.0', id: null, error: { code: -32700, message: 'parse error' } }); }
    }
  });
  // the agent closed us: done, unless this is a build box started with --pair
  process.stdin.on('end', () => { if (!opt('--pair')) process.exit(0); });
}

// ---- start ---------------------------------------------------------------------------------------------------
if (argv.includes('--help') || argv.includes('-h')) {
  console.log('node pet-peer.mjs [--pair CODE] [--lab DIR] [--name NAME]\n  as an MCP server: claude mcp add pet-peer -- node pet-peer.mjs');
  process.exit(0);
}
log(`pet-peer ${VERSION} on ${NAME}; lab ${LAB}; can build ${Object.entries(caps()).filter(([, v]) => v).map(([k]) => k).join(', ') || 'nothing'}`);
if (opt('--pair')) pairWith(opt('--pair')).catch((e) => { log(e.message); process.exit(1); });
if (!process.stdin.isTTY) mcp();
