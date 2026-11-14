/**
 * Pure helpers + constants for the Bufo desk-pet sim.
 *
 * Ported from lab/jc3248-pet/sim/pet-sim.html (the CDN-React one). Every
 * function here mirrors a routine in the firmware (lab/jc3248-pet/src/pet.cpp):
 * the JS sim is supposed to be byte-for-byte faithful to the on-device state
 * machine so the HUD reads identically on both.
 */

export const PACK = 'bufo'                       // character dir under /data/characters/
export const BASE = `/data/characters/${PACK}`
export const W = 320, H = 480, HUD_H = 80, SCALE = 3
export const REACT_MS = 4000                     // tap → heart duration
export const STALE_MS = 30000                    // BLE link goes stale after this
export const IDLE_DWELL_MS = 3000                // ~time per idle gif before rotating (ANIM_PAUSE_MS)

export interface Palette { bg: string; text: string; textDim: string }
export interface Link { connected: boolean; lastSnap: number }
export interface Snapshot {
  total: number
  running: number
  waiting: number
  msg: string
  tokens: number
  pct5h: number      // -1 means "absent"
  reset5h: number    // seconds until reset; -1 = absent
  pct7d: number
  reset7d: number
  name?: string
}

export type PetState = 'idle' | 'busy' | 'attention' | 'sleep' | 'heart'

export interface Manifest {
  name?: string
  colors?: Partial<Palette>
  states?: {
    idle?: string | string[]
    busy?: string
    attention?: string
    sleep?: string
    heart?: string
    celebrate?: string
  }
}

// Mirror of pet.cpp fmt_reset(): seconds-until-reset → "4d23h" / "3h55m" / "12m" / "--".
export function fmtReset(sec: number | null | undefined): string {
  if (sec == null || sec < 0) return '--'
  if (sec >= 86400) return `${Math.floor(sec / 86400)}d${Math.floor((sec % 86400) / 3600)}h`
  if (sec >= 3600) return `${Math.floor(sec / 3600)}h${String(Math.floor((sec % 3600) / 60)).padStart(2, '0')}m`
  return `${Math.floor(sec / 60)}m`
}

// Mirror of pet.cpp fmt_window(): "5h 8% 3h55m", or whichever parts exist.
// pct/reset of null|<0 means "absent".
export function fmtWindow(label: string, pct: number | null | undefined, reset: number | null | undefined): string {
  const havePct = pct != null && pct >= 0
  const haveReset = reset != null && reset >= 0
  if (havePct && haveReset) return `${label} ${pct}% ${fmtReset(reset)}`
  if (havePct) return `${label} ${pct}%`
  return `${label} ${fmtReset(reset)}`           // reset-only (or "--" if neither — matches firmware fallthrough)
}

// Mirror of pet.cpp computeDesired().
export function computeDesired(link: Link, snap: Snapshot, now: number): PetState {
  if (!link.connected) return 'idle'
  if (now - link.lastSnap > STALE_MS) return 'idle'
  if (snap.waiting > 0) return 'attention'
  if (snap.running > 0) return 'busy'
  if (snap.total === 0) return 'sleep'
  return 'idle'
}

// usage present when ANY of the four fields was provided (matches firmware haveUsage).
export function haveUsage(s: Snapshot): boolean {
  return (s.pct5h != null && s.pct5h >= 0) || (s.pct7d != null && s.pct7d >= 0) ||
    (s.reset5h != null && s.reset5h >= 0) || (s.reset7d != null && s.reset7d >= 0)
}

// Preset snapshots — include usage so the HUD shows the two windows.
// Same 8% / 3h55m and 41% / 4d23h as the dashboard + poke_pet.py.
const USAGE = { tokens: 1842, pct5h: 8, reset5h: 14100, pct7d: 41, reset7d: 428400 }
export const PRESETS: Record<'idle' | 'busy' | 'attention' | 'sleep', Snapshot> = {
  idle:      { total: 1, running: 0, waiting: 0, msg: 'idle', ...USAGE },
  busy:      { total: 1, running: 1, waiting: 0, msg: 'running: Bash', ...USAGE },
  attention: { total: 1, running: 0, waiting: 1, msg: 'approve: rm -rf /tmp/x', ...USAGE },
  sleep:     { total: 0, running: 0, waiting: 0, msg: 'all done', ...USAGE }
}

export const EMPTY_SNAP: Snapshot = {
  total: 0, running: 0, waiting: 0, msg: '', tokens: 0,
  pct5h: -1, reset5h: -1, pct7d: -1, reset7d: -1
}

// normalise an incoming heartbeat object → full snapshot (missing usage → -1 = absent).
export function toSnapshot(o: Partial<Snapshot> & Record<string, unknown>): Snapshot {
  const num = (v: unknown, d: number): number => {
    if (v === undefined || v === null) return d
    return Number(v) | 0
  }
  return {
    total:   num(o.total, 0),
    running: num(o.running, 0),
    waiting: num(o.waiting, 0),
    msg:     typeof o.msg === 'string' ? o.msg : '',
    tokens:  num(o.tokens, 0),
    pct5h:   num(o.pct5h, -1),
    reset5h: num(o.reset5h, -1),
    pct7d:   num(o.pct7d, -1),
    reset7d: num(o.reset7d, -1)
  }
}
