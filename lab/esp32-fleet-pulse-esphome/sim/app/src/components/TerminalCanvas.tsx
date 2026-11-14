import { useEffect, useRef } from 'react'
import { loadTerm, type TermModule } from '../lib/wasmBridge'

interface Props { onReady: (mod: TermModule) => void }

/** The LVGL-WASM canvas — pixel-faithful to the JC3248W535 (320×480). */
export default function TerminalCanvas({ onReady }: Props) {
  const canvasRef = useRef<HTMLCanvasElement>(null)
  const started = useRef(false)

  useEffect(() => {
    if (started.current || !canvasRef.current) return
    started.current = true
    loadTerm(canvasRef.current).then(onReady).catch((e) => console.error('wasm load failed', e))
  }, [onReady])

  return (
    <div className="rounded-[26px] border border-cyan-900/40 bg-[#0a1419] p-4 shadow-2xl shadow-cyan-500/[0.06]">
      <canvas
        ref={canvasRef}
        id="canvas"
        width={320}
        height={480}
        tabIndex={-1}
        onContextMenu={(e) => e.preventDefault()}
        className="block h-[480px] w-[320px] rounded-xl bg-black [image-rendering:pixelated]"
      />
    </div>
  )
}
