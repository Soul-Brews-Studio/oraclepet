// The Studio with no server (GitHub Pages): the page's /api calls answered in the browser.
// Sprites are made here (makepack.ts), packs go to the pet over Web Bluetooth, the canvas is kept in
// localStorage, and what needs a computer (Codex image generation, auto sprite, app and firmware
// builds) goes to a machine paired over WebRTC (lab/pet-peer), when there is one.
// @ts-expect-error plain ES module without types
import { Pet, PetError } from "../../../../oraclepet-lib/web/oraclepet.js";
// @ts-expect-error plain ES module without types
import { offer, makeCode, Peer } from "../../../../pet-peer/peer.js";
import { makePack, type PackFile } from "./makepack";

export const STATIC = import.meta.env.VITE_STATIC === "1";

// ---- a tiny store the bar and the shim share ------------------------------------------------------
type Listener = () => void;
const listeners = new Set<Listener>();
export const store = {
  pet: null as any, sim: false,
  peer: null as any, peerInfo: null as any, code: "", peerStatus: "",
  log: [] as string[],
};
export function subscribe(f: Listener) { listeners.add(f); return () => { listeners.delete(f); }; }
export function changed() { listeners.forEach((f) => f()); }
export function log(line: string) { store.log = [...store.log.slice(-60), `${new Date().toLocaleTimeString()}  ${line}`]; changed(); }

// ---- jobs: the Designer polls /api/jobs/:id for slow things -------------------------------------
type Job = { id: string; kind: string; title: string; status: string; started: number; ended?: number; log: string[]; result?: any; error?: string; progress?: number };
const jobs = new Map<string, Job>();
let jobSeq = 1;
function job(kind: string, title: string, run: (j: Job) => Promise<any>) {
  const j: Job = { id: `j${jobSeq++}`, kind, title, status: "running", started: Date.now(), log: [] };
  jobs.set(j.id, j);
  run(j).then((r) => { j.result = r; j.status = "ok"; }, (e) => { j.error = String(e?.message ?? e); j.status = "failed"; log(`${title}: ${j.error}`); })
    .finally(() => { j.ended = Date.now(); changed(); });
  return { job: j.id };
}

// ---- packs made in this page -----------------------------------------------------------------------
const packs = new Map<string, { name: string; files: PackFile[]; urls: Record<string, string> }>();
function packInfo(name: string) {
  const p = packs.get(name)!;
  return { name, builtIn: false, bytes: p.files.reduce((n, f) => n + f.data.length, 0),
    files: p.files.map((f) => ({ name: f.name, bytes: f.data.length, url: p.urls[f.name] })) };
}

// ---- the pet over Web Bluetooth -----------------------------------------------------------------
async function attach(pet: any, sim = false) {
  if (store.pet?.connected) await store.pet.disconnect().catch(() => {});
  store.pet = pet; store.sim = sim;
  pet.on("log", (m: string) => log(String(m)));
  pet.on("disconnect", changed);
  pet.on("connect", changed);
  pet.on("info", changed);
  await pet.connect();
  log(`Connected to ${pet.name}${sim ? " (simulated)" : ""}`);
  changed();
}
export async function connectPet() { await attach(await Pet.request()); }
export async function simulatePet() {
  // @ts-expect-error plain ES module without types
  const { SimPet } = await import("../../../../oraclepet-lib/web/simpet.js");
  const s = new SimPet();
  await attach(new Pet(s), true);
}
export async function disconnectPet() { await store.pet?.disconnect().catch(() => {}); changed(); }
function needPet() {
  if (!store.pet?.connected) throw new PetError("No pet connected: press Connect pet (or Simulated pet) at the top");
  return store.pet;
}

// ---- the paired machine over WebRTC ------------------------------------------------------------------
let actionRunner: (action: string, args: any) => Promise<unknown> = async () => { throw new Error("the designer is not ready"); };
export function setActionRunner(f: typeof actionRunner) { actionRunner = f; }
export async function pair() {
  store.peer?.close();
  store.peer = null; store.peerInfo = null;
  store.code = makeCode();
  store.peerStatus = "Making an offer";
  changed();
  const peer = await offer(store.code, { onStatus: (s: string) => { store.peerStatus = s; changed(); } });
  peer.handle("action", ({ action, args }: any) => actionRunner(action, args ?? {}));
  peer.handle("hello", () => ({ page: "studio", pet: store.pet?.connected ? store.pet.name : null }));
  peer.addEventListener("close", () => { store.peer = null; store.peerInfo = null; store.peerStatus = "The machine left"; log("The paired machine left"); changed(); });
  peer.addEventListener("note", (e: any) => { if (e.detail?.type === "log") log(`machine: ${e.detail.detail?.line ?? ""}`); });
  store.peer = peer;
  store.peerInfo = await peer.call("hello", {}).catch(() => null);
  log(`Paired with ${store.peerInfo?.name ?? "a machine"}`);
  changed();
  return peer;
}
export function unpair() { store.peer?.close(); store.peer = null; store.peerInfo = null; store.code = ""; store.peerStatus = ""; changed(); }
function needPeer(what: string) {
  if (!store.peer?.open) throw new Error(`${what} runs on a computer: pair one with the Pair button (node pet-peer.mjs on it)`);
  return store.peer;
}
export async function buildOnPeer(kind: "app" | "firmware", args: any) {
  const peer = needPeer(kind === "app" ? "Building an app" : "Building firmware");
  const r = await peer.call(kind === "app" ? "build_app" : "build_firmware", args, kind === "app" ? 120000 : 30 * 60000);
  return { ...r, data: Peer.unbytes(r.data) as Uint8Array };
}
export async function sendApp(name: string, data: Uint8Array) { await needPet().app({ name: `${name}.wasm`, data }, true, { name }); }
export async function sendFirmware(data: Uint8Array) { await needPet().ota(data); }

// ---- the /api routes the Designer calls -------------------------------------------------------------
const json = (d: unknown, status = 200) => new Response(JSON.stringify(d), { status, headers: { "content-type": "application/json" } });
const CANVAS_KEY = (id: string) => `petstudio:canvas:${id}`;

async function route(path: string, method: string, body: any): Promise<Response> {
  let m: RegExpMatchArray | null;
  if (path === "/api/state") {
    const p = store.pet;
    return json({ oraclePet: { running: false }, pets: p ? [{ name: p.name, link: p.connected ? "connected" : "off", firmware: p.info?.version, pack: p.info?.pack, app: p.info?.app }] : [],
      ports: [], boards: {}, firmware: [], apps: [], packs: [...packs.keys()].map(packInfo), sources: [], jobs: [] });
  }
  if ((m = path.match(/^\/api\/jobs\/([^/]+)$/))) { const j = jobs.get(m[1]); return j ? json(j) : json({ error: "no such job" }, 404); }
  if ((m = path.match(/^\/api\/canvas\/([a-z0-9_-]{1,32})$/))) {
    if (method === "POST") { try { localStorage.setItem(CANVAS_KEY(m[1]), JSON.stringify(body)); } catch (e) { log(`could not save the canvas: ${(e as Error).message}`); } return json({ ok: true }); }
    const d = localStorage.getItem(CANVAS_KEY(m[1]));
    return d ? new Response(d, { headers: { "content-type": "application/json" } }) : json({ error: "no canvas" }, 404);
  }
  if (path === "/api/sprite/create") {
    const name = String(body.name ?? "").toLowerCase();
    if (!/^[a-z0-9_-]{1,23}$/.test(name)) return json({ error: "name: lower-case a-z 0-9 - _, at most 23" }, 400);
    if (!body.base) return json({ error: "base: the first idle frame (PNG data URL)" }, 400);
    return json(job("sprite", `create sprite ${name}`, async () => {
      const { files, drawn } = await makePack(name, body.base, body.states ?? {}, body.fps ?? 8);
      const old = packs.get(name);
      Object.values(old?.urls ?? {}).forEach((u) => URL.revokeObjectURL(u));
      const urls = Object.fromEntries(files.map((f) => [f.name, URL.createObjectURL(new Blob([f.data as BlobPart], { type: f.name.endsWith(".gif") ? "image/gif" : "application/json" }))]));
      packs.set(name, { name, files, urls });
      log(`made sprite ${name}${drawn.length ? `: drawn ${drawn.join(", ")}` : ""}`);
      return { ...packInfo(name), drawn };
    }));
  }
  if (path === "/api/flash/ble") {
    if (body.kind !== "pack") return json({ error: "from this page: kind pack (apps and firmware: the Machine panel)" }, 400);
    const p = packs.get(String(body.name));
    if (!p) return json({ error: `no sprite ${body.name}: make it first` }, 400);
    return json(job("flash", `send ${p.name} to the pet`, async () => {
      const pet = needPet();
      await pet.pack({ name: p.name, files: p.files });
      return { pet: pet.name, pack: p.name };
    }));
  }
  if (path === "/api/images/generate" || path === "/api/sprite/auto") {
    const peer = store.peer;
    if (!peer?.open) return json({ error: "Generating pictures runs on a computer with Codex: pair one with the Pair button" }, 400);
    const call = path.endsWith("generate") ? "image_generate" : "sprite_auto";
    return json(job(call, call.replace("_", " "), async () => {
      const r = await peer.call(call, body, 15 * 60000);
      if (r?.png) r.url = `data:image/png;base64,${r.png}`;
      return r;
    }));
  }
  if (path.startsWith("/api/live/")) return json({ ok: true });
  return json({ error: `${path} needs the local Pet Studio server` }, 404);
}

/** Answer /api/* in the page: installed once, only in the static build. */
export function installShim() {
  const real = window.fetch.bind(window);
  window.fetch = (async (input: RequestInfo | URL, init?: RequestInit) => {
    const url = new URL(typeof input === "string" ? input : input instanceof URL ? input.href : input.url, location.href);
    if (url.origin !== location.origin || !url.pathname.startsWith("/api/") && !url.pathname.includes("/api/")) return real(input, init);
    const path = url.pathname.slice(url.pathname.indexOf("/api/"));
    let body: any = null;
    if (init?.body && typeof init.body === "string") { try { body = JSON.parse(init.body); } catch {} }
    return route(path, (init?.method ?? "GET").toUpperCase(), body ?? {});
  }) as typeof fetch;
}
