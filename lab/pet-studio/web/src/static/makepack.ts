// tools/make-pack.sh in the browser: one picture -> the seven states the pet firmware reads, 96 x 100
// on black, as looping GIFs. States the person drew get their own frames instead of the puppet moves.
// @ts-expect-error plain ES module without types
import { encodeGif } from "../../../../pet-draw/web/gif.js";

const W = 96, H = 100;
export const STATES = ["idle", "busy", "attention", "celebrate", "dizzy", "sleep", "heart"] as const;
type Frame = { rgba: Uint8ClampedArray; delay: number };
export type PackFile = { name: string; data: Uint8Array };

const canvas = (w: number, h: number) => { const c = document.createElement("canvas"); c.width = w; c.height = h; return c; };
async function load(src: string) { const i = new Image(); i.src = src; await i.decode(); return i; }

/** Key out the background of an opaque picture (flood fill from the top corners, 4% fuzz), trim, fit to 80 px. */
async function sprite(base: string) {
  const img = await load(base);
  const c = canvas(img.naturalWidth, img.naturalHeight);
  const g = c.getContext("2d", { willReadFrequently: true })!;
  g.drawImage(img, 0, 0);
  const id = g.getImageData(0, 0, c.width, c.height), d = id.data, w = c.width, h = c.height;
  let opaque = true;
  for (let i = 3; i < d.length; i += 4) if (d[i] < 250) { opaque = false; break; }
  if (opaque) {
    const fuzz = 0.04 * 255 * Math.sqrt(3);
    for (const [sx, sy] of [[0, 0], [w - 1, 0]]) {
      const s = (sy * w + sx) * 4, r0 = d[s], g0 = d[s + 1], b0 = d[s + 2];
      const seen = new Uint8Array(w * h), stack = [sy * w + sx];
      while (stack.length) {
        const p = stack.pop()!;
        if (seen[p]) continue;
        seen[p] = 1;
        const i = p * 4;
        if (Math.hypot(d[i] - r0, d[i + 1] - g0, d[i + 2] - b0) > fuzz) continue;
        d[i + 3] = 0;
        const x = p % w, y = (p / w) | 0;
        if (x > 0) stack.push(p - 1); if (x < w - 1) stack.push(p + 1);
        if (y > 0) stack.push(p - w); if (y < h - 1) stack.push(p + w);
      }
    }
    g.putImageData(id, 0, 0);
  }
  let x0 = w, y0 = h, x1 = -1, y1 = -1;
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) if (d[(y * w + x) * 4 + 3] > 8) { x0 = Math.min(x0, x); x1 = Math.max(x1, x); y0 = Math.min(y0, y); y1 = Math.max(y1, y); }
  if (x1 < 0) throw new Error("idle frame 1 is empty: draw or place the character first");
  const tw = x1 - x0 + 1, th = y1 - y0 + 1, k = Math.min(80 / tw, 80 / th);
  const out = canvas(Math.max(1, Math.round(tw * k)), Math.max(1, Math.round(th * k)));
  const o = out.getContext("2d")!;
  o.imageSmoothingQuality = "high";
  o.drawImage(c, x0, y0, tw, th, 0, 0, out.width, out.height);
  return out;
}

function frame(s: HTMLCanvasElement, sx: number, sy: number, lift: number, rot: number, dim: boolean, extra?: (g: CanvasRenderingContext2D) => void) {
  const c = canvas(W, H), g = c.getContext("2d", { willReadFrequently: true })!;
  g.fillStyle = "#000"; g.fillRect(0, 0, W, H);
  const w = (s.width * sx) / 100, h = (s.height * sy) / 100, bottom = H - (6 + lift);
  g.save();
  g.translate(W / 2, bottom - h / 2);
  g.rotate((rot * Math.PI) / 180);
  g.drawImage(s, -w / 2, -h / 2, w, h);
  if (dim) { g.globalCompositeOperation = "source-atop"; g.fillStyle = "rgba(0,0,0,.45)"; g.fillRect(-w / 2, -h / 2, w, h); }
  g.restore();
  extra?.(g);
  return g.getImageData(0, 0, W, H).data;
}
const wave = (a: number, i: number, n: number) => Math.round(a * Math.sin((2 * Math.PI * i) / n));
const heart = (g: CanvasRenderingContext2D, cx: number, cy: number) => {
  g.fillStyle = "#ff3b5c";
  g.beginPath(); g.arc(cx - 3, cy, 3, 0, Math.PI * 2); g.arc(cx + 3, cy, 3, 0, Math.PI * 2); g.fill();
  g.beginPath(); g.moveTo(cx - 7, cy + 1); g.lineTo(cx + 7, cy + 1); g.lineTo(cx, cy + 9); g.fill();
};

function moves(s: HTMLCanvasElement): Record<string, Frame[]> {
  const n8 = [0, 1, 2, 3, 4, 5, 6, 7];
  return {
    idle: n8.map((i) => ({ rgba: frame(s, 100, 98 + wave(2, i, 8), 0, 0, false), delay: 12 })),
    busy: n8.map((i) => { const l = Math.round(8 * Math.abs(Math.sin((Math.PI * i) / 4))); return { rgba: frame(s, 102, l < 2 ? 94 : 100, l, 0, false), delay: 6 }; }),
    celebrate: Array.from({ length: 12 }, (_, i) => ({ rgba: frame(s, 100, 100, Math.round(12 * Math.sin((Math.PI * i) / 11)), Math.round(10 * Math.sin((2 * Math.PI * i) / 11)), false), delay: 7 })),
    attention: n8.map((i) => ({ rgba: frame(s, 100, 100, Math.round(7 * Math.abs(Math.sin((Math.PI * i) / 4))), 0, false, i % 2 ? undefined : (g) => {
      g.fillStyle = "#ffd400"; g.beginPath(); g.arc(82, 13, 9, 0, Math.PI * 2); g.fill();
      g.fillStyle = "#000"; g.fillRect(81, 7, 3, 9); g.fillRect(81, 17, 3, 3);
    }), delay: 10 })),
    dizzy: n8.map((i) => ({ rgba: frame(s, 100, 100, 0, wave(12, i, 8), false), delay: 10 })),
    sleep: n8.map((i) => ({ rgba: frame(s, 104, 70 + wave(2, i, 8), -4, 0, true, (g) => {
      g.fillStyle = "#9fb4ff"; g.font = `bold ${10 + (i >> 1)}px Arial, sans-serif`; g.fillText("z", 70 + i, 40 - i * 3);
    }), delay: 18 })),
    heart: n8.map((i) => ({ rgba: frame(s, 100, 100, 0, 0, false, (g) => heart(g, 72, 30 - i * 3)), delay: 10 })),
  };
}

/** A drawn frame (the artboard PNG) fitted to 96 x 100 on black. */
async function fitted(src: string) {
  const img = await load(src);
  const c = canvas(W, H), g = c.getContext("2d", { willReadFrequently: true })!;
  g.fillStyle = "#000"; g.fillRect(0, 0, W, H);
  g.imageSmoothingQuality = "high";
  const k = Math.min(W / img.naturalWidth, H / img.naturalHeight), w = img.naturalWidth * k, h = img.naturalHeight * k;
  g.drawImage(img, (W - w) / 2, (H - h) / 2, w, h);
  return g.getImageData(0, 0, W, H).data;
}

export async function makePack(name: string, base: string, drawn: Record<string, string[]>, fps: number | Record<string, number> = 8) {
  const s = await sprite(base);
  const states = moves(s);
  const by: string[] = [];
  for (const [state, list] of Object.entries(drawn ?? {})) {
    if (!list?.length) continue;
    const f = typeof fps === "number" ? fps : fps?.[state] ?? 8;
    const delay = Math.max(2, Math.round(100 / Math.max(1, f)));
    states[state] = await Promise.all(list.map(async (p) => ({ rgba: await fitted(p), delay })));
    by.push(`${state} (${list.length} frames)`);
  }
  const files: PackFile[] = STATES.map((st) => ({ name: `${st}.gif`, data: encodeGif(states[st], W, H) as Uint8Array }));
  // the body colour: the sprite's average on black
  const t = canvas(1, 1).getContext("2d", { willReadFrequently: true })!;
  t.fillStyle = "#000"; t.fillRect(0, 0, 1, 1); t.drawImage(s, 0, 0, 1, 1);
  const [r, g, b] = t.getImageData(0, 0, 1, 1).data;
  const body = `#${[r, g, b].map((x) => x.toString(16).padStart(2, "0")).join("").toUpperCase()}`;
  const manifest = { name, colors: { body, bg: "#000000", text: "#FFFFFF", textDim: "#808080", ink: "#000000" },
    states: { sleep: "sleep.gif", idle: ["idle.gif"], busy: "busy.gif", attention: "attention.gif", celebrate: "celebrate.gif", dizzy: "dizzy.gif", heart: "heart.gif" } };
  files.push({ name: "manifest.json", data: new TextEncoder().encode(JSON.stringify(manifest, null, 2)) });
  return { files, drawn: by };
}
