import { useCallback, useRef, useState } from 'react'
import TerminalCanvas from './components/TerminalCanvas'
import Controls from './components/Controls'
import { setContent, setHeader, type TermModule } from './lib/wasmBridge'
import { fetchCapture } from './lib/mawClient'

export default function App() {
  const modRef = useRef<TermModule | null>(null)
  const timerRef = useRef<number | null>(null)
  const [ready, setReady] = useState(false)
  const [streaming, setStreaming] = useState(false)
  const [status, setStatus] = useState('Loading WASM… the built-in sample renders on boot.')
  const [error, setError] = useState(false)

  const onReady = useCallback((mod: TermModule) => {
    modRef.current = mod
    setReady(true)
    setStatus('Ready. Set the maw URL + pane and press stream.')
  }, [])

  const poll = useCallback(async (base: string, target: string) => {
    const mod = modRef.current
    if (!mod) return
    try {
      const content = await fetchCapture(base, target)
      setContent(mod, content)
      setHeader(mod, target)
      setError(false)
      setStatus(`streaming ${target} — ${content.length} chars · ${new Date().toLocaleTimeString()}`)
    } catch (e) {
      setError(true)
      setStatus(`fetch failed (${(e as Error).message}) — CORS? wrong IP? maw down?`)
    }
  }, [])

  const onStream = useCallback((base: string, target: string) => {
    if (timerRef.current) clearInterval(timerRef.current)
    setStreaming(true)
    void poll(base, target)
    timerRef.current = window.setInterval(() => void poll(base, target), 3000)
  }, [poll])

  const onStop = useCallback(() => {
    if (timerRef.current) { clearInterval(timerRef.current); timerRef.current = null }
    setStreaming(false)
    setStatus('stopped.')
  }, [])

  return (
    <div className="flex min-h-full flex-col items-center gap-5 bg-[#04080a] px-4 py-8 text-slate-300"
         style={{ fontFamily: "'Sarabun','JetBrains Mono',system-ui,sans-serif" }}>
      <h1 className="text-[17px] font-light tracking-wide text-slate-500">
        <b className="font-medium text-cyan-400">oracle-term</b> — LVGL-WASM replica · React + Vite + Tailwind
      </h1>
      <TerminalCanvas onReady={onReady} />
      <Controls ready={ready} streaming={streaming} onStream={onStream} onStop={onStop} status={status} error={error} />
      <p className="max-w-xl text-center text-xs text-slate-600">
        Same LVGL&nbsp;9.5 recolor pipeline + the <b className="text-slate-400">exact JetBrains&nbsp;Mono + Sarabun</b> fonts as
        the JC3248W535 — one portable C core, on the ESP32 and in your browser.
      </p>
    </div>
  )
}
