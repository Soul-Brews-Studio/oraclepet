// The strip on top of the Studio when it runs with no server: the pet (Web Bluetooth), the paired
// machine (WebRTC) and what that machine can build.
import { useEffect, useState } from "react";
import { Bluetooth, Cpu, FlaskConical, Hammer, Link2, Unlink, X } from "lucide-react";
import { buildOnPeer, connectPet, disconnectPet, log, pair, sendApp, sendFirmware, simulatePet, store, subscribe, unpair } from "./backend";

const HELLO_C = `#include "pet.h"

// Built on the paired machine, sent to the pet over Bluetooth.
static int taps;

PET_EXPORT("on_start") void on_start(void)
{
    pet_caption("hello over p2p");
    pet_status("app: p2p-hello");
    pet_react();
}

PET_EXPORT("on_tap") void on_tap(int x, int y)
{
    taps++;
    pet_caption(taps % 2 ? "built far away" : "runs right here");
    pet_react();
}
`;

function useStore() {
  const [, set] = useState(0);
  useEffect(() => subscribe(() => set((n) => n + 1)), []);
  return store;
}
const btn = "inline-flex items-center gap-1.5 rounded-md border border-ink/30 bg-paper px-2.5 py-1 text-[13px] font-semibold hover:border-ink disabled:opacity-40";

export function StaticBar() {
  const s = useStore();
  const [busy, setBusy] = useState("");
  const [open, setOpen] = useState(false);
  const run = (label: string, f: () => Promise<unknown>) => async () => {
    setBusy(label);
    try { await f(); } catch (e: any) { if (e?.name !== "NotFoundError") log(`${label}: ${e?.message ?? e}`); } finally { setBusy(""); }
  };
  const petOn = !!s.pet?.connected;
  const peerOn = !!s.peer?.open;
  const caps = s.peerInfo?.caps ?? {};
  const last = s.log[s.log.length - 1];

  return (
    <div className="sticker mx-3 mt-3 flex flex-wrap items-center gap-x-3 gap-y-2 px-4 py-2 text-[13px]">
      <span className="label">pet</span>
      {petOn
        ? <><span className="font-semibold">{s.pet.name}{s.sim ? " (simulated)" : ""}</span><button className={btn} onClick={run("disconnect", disconnectPet)}><X className="size-3.5" />Disconnect</button></>
        : <><button className={btn} disabled={!!busy} onClick={run("connect", connectPet)}><Bluetooth className="size-3.5" />Connect pet</button>
            <button className={btn} disabled={!!busy} onClick={run("simulate", simulatePet)}><FlaskConical className="size-3.5" />Simulated pet</button></>}
      <span className="mx-1 h-4 w-px bg-ink/20" />
      <span className="label">machine</span>
      {peerOn
        ? <><span className="font-semibold">{s.peerInfo?.name ?? "paired"}</span>
            <button className={btn} onClick={() => setOpen((o) => !o)}><Hammer className="size-3.5" />Build</button>
            <button className={btn} onClick={unpair}><Unlink className="size-3.5" />Unpair</button></>
        : s.code
          ? <><span>code <b className="font-mono text-[15px] tracking-wider select-all">{s.code}</b></span>
              <span className="text-ink/60">{s.peerStatus} · on the machine: <code className="font-mono">node pet-peer.mjs --pair {s.code}</code></span>
              <button className={btn} onClick={unpair}><X className="size-3.5" />Cancel</button></>
          : <button className={btn} disabled={!!busy} onClick={run("pair", pair)} title="Pair a computer over WebRTC: it builds apps and firmware, and an AI agent on it can draw here"><Link2 className="size-3.5" />Pair a machine</button>}
      {last && <span className="ml-auto truncate text-ink/60" title={s.log.join("\n")}>{busy ? `${busy}…` : last}</span>}
      {open && peerOn && <BuildPanel caps={caps} petOn={petOn} onClose={() => setOpen(false)} />}
    </div>
  );
}

function BuildPanel({ caps, petOn, onClose }: { caps: any; petOn: boolean; onClose: () => void }) {
  const [name, setName] = useState("p2p-hello");
  const [lang, setLang] = useState("c");
  const [source, setSource] = useState(HELLO_C);
  const [board, setBoard] = useState("jc3248");
  const [pack, setPack] = useState("siamese");
  const [out, setOut] = useState<{ kind: string; name: string; data: Uint8Array; ms: number; log?: string } | null>(null);
  const [busy, setBusy] = useState("");
  const go = (label: string, f: () => Promise<void>) => async () => {
    setBusy(label);
    try { await f(); } catch (e: any) { log(`${label}: ${e?.message ?? e}`); } finally { setBusy(""); }
  };
  const save = () => {
    const a = document.createElement("a");
    a.href = URL.createObjectURL(new Blob([out!.data as BlobPart]));
    a.download = out!.kind === "app" ? `${out!.name}.wasm` : `${out!.name}.bin`;
    a.click();
  };
  return (
    <div className="basis-full rounded-md border border-ink/20 bg-paper p-3">
      <div className="mb-2 flex items-center gap-2"><Cpu className="size-4" /><b>Build on the paired machine</b>
        <span className="text-ink/60">compilers: {Object.entries(caps).filter(([, v]) => v).map(([k]) => k).join(", ") || "none found"}</span>
        <button className={`${btn} ml-auto`} onClick={onClose}><X className="size-3.5" /></button></div>
      <div className="grid gap-3 md:grid-cols-2">
        <div className="space-y-2">
          <div className="flex flex-wrap items-center gap-2"><span className="label">app</span>
            <input className="w-32 rounded border border-ink/30 px-2 py-1 font-mono" value={name} onChange={(e) => setName(e.target.value.toLowerCase())} />
            <select className="rounded border border-ink/30 px-2 py-1" value={lang} onChange={(e) => setLang(e.target.value)}><option value="c">C</option><option value="zig">Zig</option></select>
            <button className={btn} disabled={!!busy || !caps.app} onClick={go("build app", async () => {
              const t = performance.now();
              const r = await buildOnPeer("app", { name, lang, source });
              setOut({ kind: "app", name, data: r.data, ms: performance.now() - t, log: r.log });
              log(`built ${name}.wasm on the machine: ${r.data.length} bytes`);
            })}><Hammer className="size-3.5" />{busy === "build app" ? "Building…" : "Build app"}</button></div>
          <textarea className="h-40 w-full rounded border border-ink/30 p-2 font-mono text-[12px]" value={source} onChange={(e) => setSource(e.target.value)} spellCheck={false} />
        </div>
        <div className="space-y-2">
          <div className="flex flex-wrap items-center gap-2"><span className="label">firmware</span>
            <select className="rounded border border-ink/30 px-2 py-1" value={board} onChange={(e) => setBoard(e.target.value)}><option value="jc3248">JC3248W535</option><option value="sc01">WT32-SC01 Plus</option><option value="lcd28">2.8″ ILI9341</option></select>
            <input className="w-28 rounded border border-ink/30 px-2 py-1 font-mono" value={pack} onChange={(e) => setPack(e.target.value.toLowerCase())} title="the character it starts with" />
            <button className={btn} disabled={!!busy || !caps.firmware} onClick={go("build firmware", async () => {
              const t = performance.now();
              const r = await buildOnPeer("firmware", { board, pack });
              setOut({ kind: "firmware", name: `${board}-${pack}`, data: r.data, ms: performance.now() - t, log: r.log });
              log(`built ${board} firmware on the machine: ${r.data.length} bytes`);
            })}><Hammer className="size-3.5" />{busy === "build firmware" ? "Building… (minutes)" : "Build firmware"}</button></div>
          {out && (
            <div className="rounded border border-ink/20 p-2">
              <div><b>{out.kind === "app" ? `${out.name}.wasm` : `${out.name}.bin`}</b> · {out.data.length.toLocaleString()} bytes · {(out.ms / 1000).toFixed(1)} s</div>
              <div className="mt-2 flex flex-wrap gap-2">
                <button className={btn} disabled={!!busy || !petOn} onClick={go("send", async () => {
                  if (out.kind === "app") await sendApp(out.name, out.data); else await sendFirmware(out.data);
                  log(`${out.kind === "app" ? "app" : "firmware"} sent to the pet`);
                })}><Bluetooth className="size-3.5" />{out.kind === "app" ? "Run on the pet" : "Update the pet (OTA)"}</button>
                <button className={btn} onClick={save}>Download</button>
              </div>
              {out.log && <pre className="mt-2 max-h-28 overflow-auto text-[11px] text-ink/70">{out.log}</pre>}
            </div>
          )}
        </div>
      </div>
    </div>
  );
}
