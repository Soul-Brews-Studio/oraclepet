// test_wasm.mjs: the same golden vectors (../../golden/vectors.json) and Swift edge cases
// (edge_vectors.json), run through build/oraclepet_core.wasm instead of the native build. Proves
// the wasm32 build computes the same bytes, and shows how a JS host drives the module: inputs
// go into linear memory from __heap_base up, structs sit at the offsets oraclepet.h gives.
//   make wasm-test        (node, no packages)
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const wasmPath = process.argv[2] ?? join(here, "..", "build", "oraclepet_core.wasm");
const golden = JSON.parse(readFileSync(join(here, "..", "..", "golden", "vectors.json"), "utf8"));
const edge = JSON.parse(readFileSync(join(here, "edge_vectors.json"), "utf8"));
const { instance } = await WebAssembly.instantiate(readFileSync(wasmPath), {});   // no imports
const x = instance.exports;

// ---- linear memory: a bump allocator from __heap_base, reset before each check ----
const base = x.__heap_base.value;
let top = base;
const utf8 = new TextEncoder();
const fromUtf8 = new TextDecoder();
const reset = () => { top = base; };
function alloc(n, align = 8) {
  top = (top + align - 1) & ~(align - 1);
  const p = top;
  top += Math.max(n, 1);
  const short = top - x.memory.buffer.byteLength;
  if (short > 0) x.memory.grow(Math.ceil(short / 65536));
  return p;
}
const u8 = () => new Uint8Array(x.memory.buffer);          // fresh views: grow() detaches old ones
const dv = () => new DataView(x.memory.buffer);
const putBytes = (b) => { const p = alloc(b.length); u8().set(b, p); return p; };
const putStr = (s) => { const b = utf8.encode(s); const p = alloc(b.length + 1); u8().set(b, p); u8()[p + b.length] = 0; return p; };
const putI16 = (a) => { const p = alloc(a.length * 2, 2); new Int16Array(x.memory.buffer, p, a.length).set(a); return p; };
const getBytes = (p, n) => u8().slice(p, p + n);
const getI16 = (p, n) => Array.from(new Int16Array(x.memory.buffer, p, n));
const getStr = (p) => { const m = u8(); let e = p; while (m[e]) e++; return fromUtf8.decode(m.slice(p, e)); };
const hex = (b) => Buffer.from(b).toString("hex");
const fromHex = (h) => Uint8Array.from(Buffer.from(h, "hex"));
const same = (a, b) => a.length === b.length && a.every((v, i) => v === b[i]);
const OMIT = -2147483648;                                  // OPET_OMIT

// ---- harness ----
let pass = 0, fail = 0, secPass = 0, secFail = 0, secName = "";
function section(name) {
  if (secName) console.log(`${secName.padEnd(18)} ${String(secPass).padStart(6)} passed${secFail ? "   <-- FAILED" : ""}`);
  secName = name;
  secPass = secFail = 0;
}
function check(ok, msg) {
  reset();
  if (ok) { pass++; secPass++; } else { fail++; secFail++; console.log(`  FAIL [${secName}] ${msg}`); }
}

console.log(`oraclepet core (wasm, ${readFileSync(wasmPath).length} bytes): golden vectors (protocol ${golden.protocol}) + Swift edge cases`);

section("crc32");
for (const v of golden.crc32) {
  const b = utf8.encode(v.text_utf8);
  const p = putBytes(b);
  check((x.opet_crc32(p, b.length) >>> 0) === v.crc32, `crc32("${v.text_utf8}")`);
}

section("commands");
function buildCmd(c, out, cap) {
  const a = c.args;
  switch (c.name) {
    case "begin": return x.opet_cmd_begin(out, cap, a.size, a.crc);
    case "begin_file": return x.opet_cmd_begin_file(out, cap, a.size, a.crc, putStr(a.path));
    case "begin_stream": return x.opet_cmd_begin_stream(out, cap, a.size, a.crc);
    case "end": return x.opet_cmd_end(out, cap);
    case "abort": return x.opet_cmd_abort(out, cap);
    case "select_pack": return x.opet_cmd_select_pack(out, cap, putStr(a.name));
    case "run_app": return x.opet_cmd_run_app(out, cap, putStr(a.name));
    case "progress": return x.opet_cmd_progress(out, cap, a.pct);
    case "play": return x.opet_cmd_play(out, cap, putStr(a.name));
    case "selftest": return x.opet_cmd_selftest(out, cap);
    case "listen": return x.opet_cmd_listen(out, cap, a.ms, a.send_audio ? 1 : 0);
    default: throw new Error(`unknown command ${c.name}: teach test_wasm.mjs`);
  }
}
for (const c of [...golden.commands, ...edge.commands]) {
  const out = alloc(512);
  const n = buildCmd(c, out, 512);
  check(hex(getBytes(out, n)) === c.hex, `${c.name} ${JSON.stringify(c.args)}: ${hex(getBytes(out, n))}`);
}
check(x.opet_path_ok(putStr("characters/neo/idle.gif")) === 1 && x.opet_path_ok(putStr("apps/talker.wasm")) === 1
  && x.opet_path_ok(putStr("characters/../x")) === 0, "path rules");

section("heartbeat");
// opet_heartbeat on wasm32: epoch i64 @0, total @8, running @12, waiting @16, tzoff @20,
// pct5h @24, reset5h @28, pct7d @32, reset7d @36, msg @40, who @44 (48 bytes)
for (const h of [{ name: "golden", ...golden.heartbeat }, ...edge.heartbeat]) {
  const a = h.args, hb = alloc(48), out = alloc(512);
  x.opet_heartbeat_init(hb, a.total, a.running, a.waiting, putStr(a.msg), BigInt(a.epoch), a.tzoff);
  const d = dv();
  d.setInt32(hb + 24, a.pct5h ?? OMIT, true);
  d.setInt32(hb + 28, a.reset5h ?? OMIT, true);
  d.setInt32(hb + 32, a.pct7d ?? OMIT, true);
  d.setInt32(hb + 36, a.reset7d ?? OMIT, true);
  d.setUint32(hb + 44, a.who === undefined ? 0 : putStr(a.who), true);
  const n = x.opet_heartbeat_line(out, 512, hb);
  const line = fromUtf8.decode(getBytes(out, n));
  check(line === h.line, `${h.name}: ${JSON.stringify(line)}`);
}

section("psnd");
for (const v of golden.psnd) {
  const pcm = putI16(v.pcm), size = x.opet_psnd_size(v.pcm.length), out = alloc(size);
  const n = x.opet_psnd_encode(out, size, pcm, v.pcm.length, 16000);
  check(n === size && hex(getBytes(out, n)) === v.clip_hex, `${v.name}: encode`);
  for (const chunk of [1, 7, 503, 504, 505, 100000]) {     // streaming encoder, 268-byte state
    const pcm2 = putI16(v.pcm), e = alloc(268), buf = alloc(size + 512);
    let w = x.opet_psnd_header_write(buf, 16, 16000, v.pcm.length), bad = false;
    x.opet_psnd_enc_init(e);
    for (let at = 0; at < v.pcm.length; at += chunk) {
      const k = Math.min(chunk, v.pcm.length - at);
      const r = x.opet_psnd_enc_feed(e, buf + w, (Math.floor(k / 504) + 1) * 256, pcm2 + 2 * at, k);
      if (r < 0) bad = true; else w += r;
    }
    w += Math.max(0, x.opet_psnd_enc_finish(e, buf + w, 256));
    check(!bad && hex(getBytes(buf, w)) === v.clip_hex, `${v.name}: streamed in pieces of ${chunk}`);
  }
}
for (const v of [...golden.psnd, ...edge.psnd_misc]) {
  const clip = fromHex(v.clip_hex), cp = putBytes(clip), out = alloc(2 * (v.decoded.length + 504), 2);
  const n = x.opet_psnd_decode(out, v.decoded.length + 504, cp, clip.length);
  check(n === v.decoded.length && same(getI16(out, n), v.decoded), `${v.name}: decode`);
}
// LISTEN audio: the clip arrives as notifications of any size (opet_mic_rx is 272 bytes)
for (const v of golden.psnd) {
  const clip = fromHex(v.clip_hex), m = alloc(272), got = [];
  x.opet_mic_init(m);
  for (let off = 0, k = 1; off < clip.length; off += k, k = (k * 7 + 3) % 509 + 1) {
    const piece = clip.slice(off, off + k), cap = (Math.floor(piece.length / 256) + 1) * 504, out = alloc(2 * cap, 2);
    const n = x.opet_mic_feed(m, out, cap, putBytes(piece), piece.length);
    if (n > 0) got.push(...getI16(out, n));
  }
  check(x.opet_mic_done(m) === 1 && same(got, v.decoded), `${v.name}: mic stream reassembled`);
}
const tone = golden.psnd.find((p) => p.name === "tone440");
for (const t of edge.psnd_truncated) {
  const clip = fromHex(tone.clip_hex).slice(0, t.len), cp = putBytes(clip), out = alloc(2048, 2);
  const n = x.opet_psnd_decode(out, 1024, cp, clip.length);
  check(n === t.samples && same(getI16(out, n), tone.decoded.slice(0, n)), `tone440 cut at ${t.len}`);
}
for (const b of edge.psnd_invalid) {
  const clip = fromHex(b.hex), cp = putBytes(clip), out = alloc(64, 2);
  check((x.opet_psnd_decode(out, 16, cp, clip.length) !== -1) === b.decodes, `invalid clip ${b.name}`);
}
for (const w of edge.wav) {
  const out = alloc(44);
  x.opet_wav_header(out, 44, 16000, w.pcm.length);
  const pcm = new Uint8Array(new Int16Array(w.pcm).buffer);
  check(hex(getBytes(out, 44)) + hex(pcm) === w.hex, `wav ${w.name}`);
}

section("speech");
for (const t of edge.tidy) {
  const p = putI16(t.pcm), n = x.opet_pcm_tidy(p, t.pcm.length);
  check(n === t.out.length && same(getI16(p, n), t.out), `tidy ${t.name}`);
}
{
  const long = Array.from({ length: edge.tidy_long.n }, (_, i) => ((i * 7919) % 20001) - 10000);
  const p = putI16(long), n = x.opet_pcm_tidy(p, long.length);
  check(n === edge.tidy_long.out_len && (x.opet_crc32(p, 2 * n) >>> 0) === edge.tidy_long.out_crc32, "tidy of 100000 samples");
}
for (const t of edge.is_thai) check(x.opet_text_is_thai(putStr(t.text)) === (t.thai ? 1 : 0), `is_thai(${t.text})`);

section("images");
// opet_image_info: project @0 (33), version @33 (33), built @66 (34), sha16 @100 (17)
for (const im of [{ name: "golden", valid: true, ...golden.image }, ...edge.image]) {
  const b = fromHex(im.hex), p = putBytes(b), info = alloc(117);
  const ok = x.opet_image_inspect(p, b.length, info) === 1;
  check(ok === im.valid && (!ok || (getStr(info) === im.project && getStr(info + 33) === im.version
    && getStr(info + 66) === im.built && getStr(info + 100) === im.sha16)), `image ${im.name}`);
  if (ok && im.board !== undefined) {
    const proj = putStr(im.project);
    check(getStr(x.opet_board_name(proj)) === im.board && x.opet_is_pet_project(proj) === (im.pet_project ? 1 : 0),
      `image ${im.name}: board`);
  }
}
for (const name of golden.projects) check(x.opet_is_pet_project(putStr(name)) === 1, `${name} is a pet project`);

section("status");
// opet_status (264 bytes): kind @0, then int32 off, size, window, bytes, crc, ms, samples, rms,
// peak, freq, floor, sent, mic, heard, quiet, tone @4..@64, float db @68, why @72, name @168
const FIELDS = ["off", "size", "window", "bytes", "crc", "ms", "samples", "rms", "peak", "freq", "floor",
  "sent", "mic", "heard", "quiet", "tone"];
function readStatus(line) {
  const b = utf8.encode(line), p = putBytes(b), st = alloc(264);
  const kind = x.opet_status_parse(st, p, b.length), d = dv(), o = { kind: getStr(x.opet_status_kind_name(kind)) };
  FIELDS.forEach((f, i) => { o[f] = d.getInt32(st + 4 + 4 * i, true); });
  o.db = d.getFloat32(st + 68, true);
  o.why = getStr(st + 72);
  o.name = getStr(st + 168);
  return o;
}
for (const s of golden.status) check(readStatus(s.line).kind === s.kind, `golden ${s.line}`);
{
  const h = readStatus(golden.status.find((s) => s.kind === "heard").line);
  check(h.ms === 3000 && h.samples === 48000 && h.rms === 412 && h.peak === 9120 && h.freq === 1000 && h.floor === 31
    && h.db === Math.fround(23.4), "heard fields");
}
for (const s of edge.status) {
  const o = readStatus(s.line);
  // C strings end at a NUL, so a "\u0000" inside why/name cuts them there (oraclepet.h)
  const want = (k) => (typeof s[k] === "string" ? s[k].split("\0")[0] : s[k]);
  const bad = Object.keys(s).filter((k) => k !== "line" && (
    k === "db" ? o.db !== Math.fround(s.db) : typeof s[k] === "boolean" ? o[k] !== (s[k] ? 1 : 0) : o[k] !== want(k)));
  check(bad.length === 0, `edge ${JSON.stringify(s.line)}: ${bad.map((k) => `${k} ${o[k]} vs ${s[k]}`).join(", ")}`);
}
{
  const info = '{"ver":"6ac87a5-dirty","project":"oraclepet_lcd28","packs":["atlas","siamese"],"free":1244}';
  const b = utf8.encode(info), p = putBytes(b), out = alloc(64), v = alloc(8);
  const n = x.opet_json_list_str(p, b.length, putStr("packs"), 1, out, 64);
  const okInt = x.opet_json_int(p, b.length, putStr("free"), v) === 1 && dv().getBigInt64(v, true) === 1244n;
  check(n === 7 && getStr(out) === "siamese" && okInt, "json getters on the control read");
}

section("window");
// opet_window: total @0, sent @4, acked @8, window @12
{
  const w = alloc(16), off = alloc(4), steps = edge.window.steps;
  x.opet_window_init(w, edge.window.total, edge.window.window);
  let ok = true;
  for (const s of steps) {
    if (s.op === "ack") x.opet_window_ack(w, s.arg);
    else {
      const len = x.opet_window_next(w, s.arg, off);
      if (len !== s.len || (len && dv().getUint32(off, true) !== s.off)) ok = false;
    }
    const d = dv();
    if (d.getUint32(w + 4, true) !== s.sent || d.getUint32(w + 8, true) !== s.acked
      || x.opet_window_done(w) !== (s.done ? 1 : 0)) ok = false;
  }
  check(ok, "the script Swift's OTAWindow ran");
}

section("");
console.log(`${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
