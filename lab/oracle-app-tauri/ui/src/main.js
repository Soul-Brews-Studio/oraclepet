// oracle-app-tauri — Claude Code dashboard front-end.
//
// Two paths to current data:
//   1. Push: subscribe to `status-update` events that the Rust backend
//      emits on every accepted broadcast (≤ every 30s by default).
//   2. Pull: poll `get_status` every 5s as a safety net + initial paint.
//
// No build step — vanilla module JS.

const tauri = window.__TAURI__ ?? {};
const invoke = tauri.core?.invoke;
const listen = tauri.event?.listen;

if (!invoke) {
  console.error("Tauri invoke API not available — window.__TAURI__:", tauri);
}

function colorClass(pct) {
  if (pct > 90) return "red";
  if (pct > 70) return "yellow";
  return "";
}

function ctxLabel(maxK) {
  if (maxK >= 1000) return `${maxK / 1000}M context`;
  return `${maxK}K context`;
}

function fmtClock() {
  const d = new Date();
  return d.toLocaleTimeString("en-GB", { hour12: false });
}

const els = {
  dashboard: document.querySelector(".dashboard"),
  header: document.querySelector(".header"),
  model: document.getElementById("model"),
  modelCtx: document.getElementById("model-ctx"),
  ctxFill: document.getElementById("ctx-fill"),
  ctxLabel: document.getElementById("ctx-label"),
  q7dFill: document.getElementById("q7d-fill"),
  q7dLabel: document.getElementById("q7d-label"),
  session: document.getElementById("session"),
  lastRefresh: document.getElementById("last-refresh"),
};

// Uniformly scale the fixed-design card to fill the window, preserving
// aspect ratio (zoom, not reflow). Measures the card at scale 1, then
// picks the largest scale that fits within ~92% of the viewport.
function fitScale() {
  const card = els.dashboard;
  if (!card) return;
  const MARGIN = 0.92;
  card.style.transform = "scale(1)";
  const w = card.offsetWidth || 1;
  const h = card.offsetHeight || 1;
  const scale = Math.min(
    (window.innerWidth * MARGIN) / w,
    (window.innerHeight * MARGIN) / h,
  );
  card.style.transform = `scale(${scale})`;
}
window.addEventListener("resize", fitScale);

function render(s) {
  // Header shows WHICH project's context this is (the pinned/active
  // session can change via the tray submenu).
  if (s.project) els.header.textContent = `ψ ${s.project}`;
  els.model.textContent = s.model_friendly;
  els.modelCtx.textContent = ctxLabel(s.ctx_max_k);

  const ctxPct = Math.min(s.ctx_pct, 100);
  els.ctxFill.style.width = `${ctxPct}%`;
  els.ctxFill.className = `bar-fill ctx ${colorClass(s.ctx_pct)}`;
  els.ctxLabel.textContent = `${s.ctx_pct}% · ${s.ctx_used_k}k / ${s.ctx_max_k}k`;

  const q7dPct = Math.min(s.q7d_pct, 100);
  els.q7dFill.style.width = `${q7dPct}%`;
  els.q7dFill.className = `bar-fill q7d ${colorClass(s.q7d_pct)}`;
  els.q7dLabel.textContent = `${s.q7d_pct}% · resets ${s.reset_human}`;

  els.session.textContent = `session: ${s.session_id || "—"}`;
  els.lastRefresh.textContent = fmtClock();
  // Content size can change (longer model name / project) — refit.
  fitScale();
}

// ── Debounce + coalesce incoming updates ──────────────────────────────
// Updates arrive from two sources (BLE-driven push every ~5s + a 2s poll)
// and can burst. Rather than re-rendering on every one (flicker), we keep
// only the LATEST pending value and apply it after a short quiet window.
// A continuous stream still updates at most every MAX_WAIT.
//
// Coalescing is keyed by a content signature: if the newest value is
// identical to what's on screen we skip the render entirely (no flicker);
// if the same project sends a new value we just use the newest; a new
// project simply becomes the latest pending and renders after the wait.
const QUIET_MS = 600;     // wait for a burst to settle before drawing
const MAX_WAIT_MS = 1800; // …but don't starve under a steady stream

let pending = null;
let applyTimer = null;
let firstSeenAt = 0;
let lastKey = null;

function sigOf(s) {
  return [s.project, s.model_friendly, s.ctx_used_k, s.q7d_pct, s.reset_human].join("|");
}

function enqueue(s) {
  if (!s) return;
  pending = s; // coalesce: only the most recent survives
  const now = Date.now();
  if (applyTimer === null) firstSeenAt = now;
  else clearTimeout(applyTimer);
  const waited = now - firstSeenAt;
  const delay = Math.min(QUIET_MS, Math.max(0, MAX_WAIT_MS - waited));
  applyTimer = setTimeout(flush, delay);
}

function flush() {
  applyTimer = null;
  const s = pending;
  pending = null;
  if (!s) return;
  const key = sigOf(s);
  if (key === lastKey) return; // unchanged → don't redraw (kills flicker)
  lastKey = key;
  render(s);
}

async function refresh() {
  if (!invoke) {
    els.modelCtx.textContent = "Tauri bridge unavailable — see console";
    return;
  }
  try {
    enqueue(await invoke("get_status"));
  } catch (e) {
    console.error("refresh failed:", e);
    els.modelCtx.textContent = `error: ${e}`;
  }
}

// Push: receive every accepted broadcast in real time (if event API loaded).
if (listen) {
  listen("status-update", (event) => enqueue(event.payload));
} else {
  console.warn("event API unavailable — falling back to pure polling");
}

// Initial fit (before data lands) + pull: initial paint + 2s polling.
fitScale();
refresh();
setInterval(refresh, 2000);
