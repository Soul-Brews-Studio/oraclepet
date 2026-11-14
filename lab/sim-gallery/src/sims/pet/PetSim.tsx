import { useCallback, useEffect, useRef, useState } from 'react'
import {
  BASE, EMPTY_SNAP, IDLE_DWELL_MS, PACK, PRESETS, REACT_MS,
  computeDesired, toSnapshot,
  type Link, type Manifest, type Palette, type PetState, type Snapshot
} from './petLogic'
import Panel from './Panel'
import { useLang } from '../../i18n/LangContext'

/**
 * The Bufo desk-pet simulator, ported from pet-sim.html into a real React TS
 * component. Same rAF loop, same state machine, same HUD as the firmware. The
 * only difference: it loads the character pack from `/data/characters/bufo`
 * (Vite-served) instead of `../data/characters/bufo` (python -m http.server).
 */
export default function PetSim() {
  const { t } = useLang()

  // manifest-derived
  const [pal, setPal] = useState<Palette>({ bg: '#000000', text: '#FFFFFF', textDim: '#808080' })
  const statesRef = useRef<NonNullable<Manifest['states']>>({})
  const [petName, setPetName] = useState(PACK)
  const [loadErr, setLoadErr] = useState<string | null>(null)

  // live state (refs drive the rAF loop; mirrored into state for render)
  const linkRef = useRef<Link>({ connected: false, lastSnap: 0 })
  const snapRef = useRef<Snapshot>({ ...EMPTY_SNAP })
  const reactingRef = useRef(false)
  const reactUntilRef = useRef(0)
  const curRef = useRef<PetState | null>(null)
  const idleRotRef = useRef(0)
  const idleNextAtRef = useRef(0)

  // render mirrors
  const [link, setLink] = useState<Link>(linkRef.current)
  const [snap, setSnap] = useState<Snapshot>(snapRef.current)
  const [reacting, setReacting] = useState(false)
  const [cur, setCur] = useState<PetState | null>(null)
  const [gifSrc, setGifSrc] = useState('')
  const [now, setNow] = useState(0)
  // connLabel is derived from link state + current language — must NOT be
  // useState(t(…)), which would freeze the value at mount (the initial render
  // captures whatever `lang` was, then ignores later toggles).
  const [log, setLog] = useState<string[]>([])
  const [hbText, setHbText] = useState(
    JSON.stringify({ total: 1, running: 1, waiting: 0, msg: 'running: Bash', tokens: 1842,
                     pct5h: 8, reset5h: 14100, pct7d: 41, reset7d: 428400 }, null, 0)
  )

  const addLog = useCallback((m: string) => {
    setLog((prev) => [m, ...prev].slice(0, 40))
  }, [])

  const reactionGif = useCallback((): string | null => {
    const st = statesRef.current
    return (st.heart ?? st.celebrate) || null
  }, [])

  const gifForState = useCallback((s: PetState): string | null => {
    const st = statesRef.current
    if (s === 'idle') {
      const idle = Array.isArray(st.idle) ? st.idle : (st.idle ? [st.idle] : [])
      return idle.length ? idle[idleRotRef.current % idle.length] : null
    }
    if (s === 'heart') return reactionGif()
    return (st as Record<string, string | undefined>)[s] ?? null
  }, [reactionGif])

  const showGif = useCallback((file: string | null) => {
    if (!file) return
    // cache-bust so re-selecting the same gif restarts its animation
    setGifSrc(`${BASE}/${file}?t=${Math.floor(performance.now())}`)
  }, [])

  const enterState = useCallback((s: PetState) => {
    curRef.current = s
    setCur(s)
    if (s === 'idle') idleNextAtRef.current = performance.now() + IDLE_DWELL_MS
    showGif(gifForState(s))
    addLog(`state -> ${s}` + (s === 'idle' ? ` (idle_${idleRotRef.current})` : ''))
  }, [showGif, gifForState, addLog])

  // --- the rAF loop (mirror pet.cpp tick) ---
  useEffect(() => {
    let raf = 0
    const loop = () => {
      const t = performance.now()
      setNow(t)

      if (reactingRef.current && t >= reactUntilRef.current) {
        reactingRef.current = false
        setReacting(false)
      }
      if (reactingRef.current) {
        if (curRef.current !== 'heart') enterState('heart')
      } else {
        const want = computeDesired(linkRef.current, snapRef.current, t)
        const st = statesRef.current
        const idleList = Array.isArray(st.idle) ? st.idle : (st.idle ? [st.idle] : [])
        if (want !== curRef.current) {
          enterState(want)
        } else if (curRef.current === 'idle' && idleList.length > 1 && t >= idleNextAtRef.current) {
          idleRotRef.current = (idleRotRef.current + 1) % idleList.length
          enterState('idle')                     // rotate to the next idle gif
        }
      }
      raf = requestAnimationFrame(loop)
    }
    raf = requestAnimationFrame(loop)
    return () => cancelAnimationFrame(raf)
  }, [enterState])

  // --- boot: load the manifest (same one the firmware reads) ---
  useEffect(() => {
    fetch(`${BASE}/manifest.json`)
      .then((r) => {
        if (!r.ok) throw new Error(`HTTP ${r.status}`)
        return r.json() as Promise<Manifest>
      })
      .then((m) => {
        if (m.colors) setPal((p) => ({ ...p, ...m.colors }))
        const st = (m.states ?? {}) as NonNullable<Manifest['states']>
        if (typeof st.idle === 'string') st.idle = [st.idle]
        statesRef.current = st
        setPetName(m.name ?? PACK)
        addLog(`loaded '${m.name ?? PACK}'  states: ${Object.keys(st).join(', ')}`)
        enterState('idle')
      })
      .catch((e: Error) => {
        setLoadErr(e.message)
        addLog(`manifest fetch failed: ${e.message}  — needs /data/characters/bufo/manifest.json`)
      })
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [])

  // --- actions ---
  const applySnapshot = useCallback((o: Partial<Snapshot> | Record<string, unknown>) => {
    const s = toSnapshot(o as Record<string, unknown>)
    s.name = petName
    snapRef.current = s
    setSnap(s)
    const l: Link = { connected: true, lastSnap: performance.now() }
    linkRef.current = l
    setLink(l)
    addLog(`heartbeat total=${s.total} run=${s.running} wait=${s.waiting} tok=${s.tokens}`)
  }, [petName, addLog])

  const toggleConn = () => {
    if (linkRef.current.connected) {
      const l: Link = { connected: false, lastSnap: 0 }
      linkRef.current = l; setLink(l)
      addLog('disconnected')
    } else {
      const l: Link = { connected: true, lastSnap: performance.now() }
      linkRef.current = l; setLink(l)
    }
  }

  // Derived from link state + current language so it follows EN/TH toggles.
  const connLabel = link.connected ? t('petLinked') : t('petAdvertising')

  const tap = () => {
    if (!reactionGif()) { addLog('no reaction gif in pack'); return }
    reactUntilRef.current = performance.now() + REACT_MS
    reactingRef.current = true
    setReacting(true)
    enterState('heart')
    addLog('tap -> heart')
  }

  const sendHeartbeat = () => {
    try { applySnapshot(JSON.parse(hbText)) }
    catch (e) { addLog('bad JSON: ' + (e as Error).message) }
  }

  // keep snap.name fresh for the HUD even before first heartbeat
  const hudSnap: Snapshot = { ...snap, name: petName }

  // A plain function, not a component: a component defined inside PetSim is a new type on every
  // render, and the rAF loop re-renders every frame, so React remounted the buttons ~60x/s and a
  // click's mousedown and mouseup landed on different nodes (busy/attention "unclickable").
  const presetBtn = (id: keyof typeof PRESETS, label: string) => (
    <button
      onClick={() => applySnapshot(PRESETS[id])}
      className={
        'rounded-lg border px-3 py-1.5 text-sm transition ' +
        'bg-slate-700/60 border-white/10 hover:bg-slate-600/70 ' +
        (!reacting && cur === id ? 'outline outline-2 outline-sky-400' : '')
      }
    >
      {label}
    </button>
  )

  return (
    <div className="flex flex-wrap gap-8">
      {/* device column */}
      <div>
        <h2 className="mb-3 font-mono text-xs tracking-wider2 uppercase text-ash">
          {t('petBoardLabel')}
        </h2>
        <div className="inline-block rounded-3xl bg-slate-800 p-4 shadow-2xl ring-1 ring-white/10">
          <Panel
            pal={pal}
            gifSrc={gifSrc}
            snap={hudSnap}
            link={link}
            reacting={reacting}
            now={now}
            onTap={tap}
          />
        </div>
        {loadErr && (
          <p className="mt-3 max-w-[320px] text-xs text-amber-400">
            manifest load failed: <code className="ml-1 rounded bg-black/40 px-1">{loadErr}</code>
            <br />
            expected at <code className="text-emerald-300">{BASE}/manifest.json</code>
          </p>
        )}
      </div>

      {/* controls column */}
      <div className="min-w-[300px] max-w-[380px] flex-1">
        <h2 className="mb-3 font-mono text-xs tracking-wider2 uppercase text-ash">
          {t('petActivity')}
        </h2>

        <Card title={t('petLink')}>
          <button
            onClick={toggleConn}
            className={
              'rounded-lg border px-3 py-1.5 text-sm transition ' +
              (link.connected
                ? 'bg-moss/30 border-moss/60 text-chalk hover:bg-moss/40'
                : 'bg-coal/60 border-embers text-chalk hover:border-glow/40')
            }
          >
            {link.connected ? t('petDisconnect') : t('petConnect')}
          </button>
          <span className="ml-3 text-sm text-ash">{connLabel}</span>
        </Card>

        <Card title={t('petStates')}>
          <div className="flex flex-wrap gap-2">
            {presetBtn('idle', 'idle')}
            {presetBtn('busy', 'busy ⚙️')}
            {presetBtn('attention', 'attention 🟠')}
            {presetBtn('sleep', 'sleep 😴')}
            <button
              onClick={tap}
              className="rounded-lg border border-embers bg-coal/60 text-chalk px-3 py-1.5 text-sm hover:border-glow/40"
            >
              {t('petTapHeart')}
            </button>
          </div>
          <p className="mt-3 text-xs text-ash leading-relaxed">
            {t('petPresetsHelp')}
          </p>
        </Card>

        <Card title={t('petRawTitle')}>
          <textarea
            value={hbText}
            onChange={(e) => setHbText(e.target.value)}
            spellCheck={false}
            className="h-24 w-full resize-y rounded-lg border border-embers bg-ink/80 p-2 font-mono text-xs text-chalk"
          />
          <div className="mt-2 flex items-center gap-3">
            <button
              onClick={sendHeartbeat}
              className="rounded-lg border border-embers bg-coal/60 text-chalk px-3 py-1.5 text-sm hover:border-glow/40"
            >
              {t('petSend')}
            </button>
            <span className="text-xs text-ash">{t('petWireHint')}</span>
          </div>
        </Card>

        <Card title={t('petLogLabel')}>
          <div className="max-h-32 overflow-auto whitespace-pre-wrap font-mono text-[11px] text-ash no-scrollbar">
            {log.join('\n')}
          </div>
        </Card>
      </div>
    </div>
  )
}

function Card({ title, children }: { title: string; children: React.ReactNode }) {
  return (
    <div className="mb-3.5 rounded-xl border border-embers bg-dusk/60 p-3.5">
      <h3 className="mb-2.5 font-mono text-2xs tracking-wider2 uppercase text-ash">{title}</h3>
      {children}
    </div>
  )
}
