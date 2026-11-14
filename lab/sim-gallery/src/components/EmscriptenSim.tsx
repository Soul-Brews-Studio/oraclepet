import { useCallback, useEffect, useRef, useState } from 'react'
import { useLang } from '../i18n/LangContext'

/**
 * Mounts an Emscripten-built LVGL+SDL sim into a React-owned <canvas>.
 *
 * Built with `-sMODULARIZE=1 -sEXPORT_NAME=<factory>`, so the .js is a factory:
 * `<factory>({ canvas, … })` → Promise<instance>. We load it as a classic
 * <script>, call the factory with our per-route canvas, and on unmount tear the
 * runtime down.
 *
 * AUTO-LOAD, bounded by visibility. Each LVGL sim runs a forever 60fps rAF; N of
 * them running at once pegs the main thread (that was the scroll-freeze). So:
 *   - first scroll INTO view → instantiate + run (one-time)
 *   - scroll OUT of view      → Module.pauseMainLoop()  (rAF stops, CPU freed)
 *   - scroll back IN          → Module.resumeMainLoop()
 * Only the visible sim(s) burn CPU. Each MODULARIZE instance owns its runtime,
 * so pause/resume is per-instance. A manual ▶ also starts it early.
 */
export default function EmscriptenSim({
  src,
  factory,
  width,
  height,
  onReady
}: {
  src: string
  factory: string
  width: number
  height: number
  onReady?: (instance: unknown) => void
}) {
  const { t } = useLang()
  const canvasRef = useRef<HTMLCanvasElement | null>(null)
  const wrapRef = useRef<HTMLDivElement | null>(null)
  const [status, setStatus] = useState<'idle' | 'probing' | 'loading' | 'ready' | 'error' | 'not-built'>('idle')
  const [errMsg, setErrMsg] = useState<string>('')
  const startedRef = useRef(false)
  const instanceRef = useRef<{ pauseMainLoop?: () => void; resumeMainLoop?: () => void; _exit_runtime?: () => void } | null>(null)

  const start = useCallback(async () => {
    if (startedRef.current) return
    startedRef.current = true
    setStatus('probing')

    // Probe: a 404 or Cloudflare's SPA-fallback HTML → "not built" card.
    try {
      const probe = await fetch(src, { headers: { Range: 'bytes=0-127' } })
      if (!probe.ok) { setStatus('not-built'); return }
      const ctype = (probe.headers.get('content-type') ?? '').toLowerCase()
      if (ctype.startsWith('text/html')) { setStatus('not-built'); return }
      const head = (await probe.text()).trimStart()
      if (head.startsWith('<')) { setStatus('not-built'); return }
    } catch (e) {
      setStatus('not-built'); setErrMsg(String(e)); return
    }

    setStatus('loading')
    const existing = document.querySelector<HTMLScriptElement>(`script[data-emsim="${factory}"]`)
    if (!existing) {
      const tag = document.createElement('script')
      tag.src = src
      tag.async = true
      tag.dataset.emsim = factory
      document.head.appendChild(tag)
      await new Promise<void>((resolve, reject) => {
        tag.addEventListener('load', () => resolve())
        tag.addEventListener('error', () => reject(new Error(`failed to load ${src}`)))
      }).catch((e) => { setStatus('error'); setErrMsg(String(e)) })
    }

    const create = (globalThis as unknown as Record<string, unknown>)[factory] as
      | ((opts: Record<string, unknown>) => Promise<typeof instanceRef.current>)
      | undefined
    if (typeof create !== 'function') {
      setStatus('error')
      setErrMsg(`global '${factory}' is not a function (EXPORT_NAME mismatch?)`)
      return
    }

    try {
      instanceRef.current = await create({
        canvas: canvasRef.current,
        print:    (...args: unknown[]) => console.log(`[${factory}]`, ...args),
        printErr: (...args: unknown[]) => console.warn(`[${factory}]`, ...args)
      })
      setStatus('ready')
      if (onReady) setTimeout(() => { try { onReady(instanceRef.current) } catch (e) { console.warn(`[${factory}] onReady`, e) } }, 50)
    } catch (e) {
      setStatus('error'); setErrMsg(String(e))
    }
  }, [src, factory, onReady])

  // Visibility-gated auto-load: start on first enter, pause/resume on leave/return.
  useEffect(() => {
    const el = wrapRef.current
    if (!el) return
    const obs = new IntersectionObserver(
      (entries) => {
        const visible = entries.some((e) => e.isIntersecting)
        if (visible) {
          if (!startedRef.current) start()
          else { try { instanceRef.current?.resumeMainLoop?.() } catch { /* ignore */ } }
        } else if (startedRef.current) {
          try { instanceRef.current?.pauseMainLoop?.() } catch { /* ignore */ }
        }
      },
      { rootMargin: '120px 0px', threshold: 0 }
    )
    obs.observe(el)
    return () => obs.disconnect()
  }, [start])

  useEffect(() => {
    return () => { try { instanceRef.current?._exit_runtime?.() } catch { /* ignore */ } }
  }, [])

  const showOverlay = status !== 'ready'
  return (
    <div ref={wrapRef} className="relative inline-block">
      <canvas
        ref={canvasRef}
        width={width}
        height={height}
        className="bg-black rounded-md"
        style={{ width, height, display: 'block' }}
      />
      {showOverlay && (
        <div className="absolute inset-0 flex items-center justify-center bg-black/80 rounded-md text-sm">
          {(status === 'idle' || status === 'probing' || status === 'loading') && (
            <button
              type="button"
              onClick={() => start()}
              className="group flex flex-col items-center gap-2 text-ash hover:text-glow transition px-4 py-3 rounded-md"
              aria-label="run sim"
            >
              <span className={'text-2xl ' + (status === 'idle' ? 'group-hover:scale-110 transition-transform' : 'animate-pulse')}>
                {status === 'idle' ? '▶' : '◌'}
              </span>
              <span className="font-mono text-2xs">
                {status === 'loading' || status === 'probing' ? t('esmLoading') : t('specTapRun')}
              </span>
            </button>
          )}
          {status === 'not-built' && (
            <div className="text-center text-chalk px-6">
              <div className="text-2xl mb-2">🛠️</div>
              <div className="font-mono text-sm">{t('esmNotBuilt')}</div>
              <div className="text-xs text-ash mt-1">
                {t('esmRun')} <code className="text-glow">make build-sims</code>{' '}
                {t('esmFrom')} <code className="text-ash">lab/sim-gallery/</code>
              </div>
            </div>
          )}
          {status === 'error' && (
            <div className="text-center text-rust px-6">
              <div className="text-2xl mb-2">⛔</div>
              <div className="font-mono text-sm">{t('esmFailed')}</div>
              {errMsg && <pre className="text-xs text-ash mt-2 max-w-sm whitespace-pre-wrap">{errMsg}</pre>}
            </div>
          )}
        </div>
      )}
    </div>
  )
}
