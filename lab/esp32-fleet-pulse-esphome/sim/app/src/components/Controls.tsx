import { useState } from 'react'

interface Props {
  ready: boolean
  streaming: boolean
  onStream: (base: string, target: string) => void
  onStop: () => void
  status: string
  error: boolean
}

const input =
  'rounded-lg border border-cyan-900/40 bg-[#0a1419] px-3 py-1.5 font-mono text-slate-200 outline-none focus:border-cyan-600'

export default function Controls({ ready, streaming, onStream, onStop, status, error }: Props) {
  const [base, setBase] = useState('http://192.168.1.109:3456')
  const [target, setTarget] = useState('52-esp32:esp32-exp')
  return (
    <div className="flex flex-col items-center gap-3">
      <div className="flex flex-wrap items-center justify-center gap-2 text-sm">
        <input className={`${input} w-56`} value={base} onChange={(e) => setBase(e.target.value)} title="maw base URL" />
        <input className={`${input} w-44`} value={target} onChange={(e) => setTarget(e.target.value)} title="session:windowname" />
        <button
          disabled={!ready || streaming}
          onClick={() => onStream(base, target)}
          className="rounded-lg border border-cyan-900/40 bg-cyan-950/60 px-4 py-1.5 text-cyan-300 transition hover:bg-cyan-900/60 disabled:opacity-40"
        >▶ stream</button>
        <button
          onClick={onStop}
          className="rounded-lg border border-cyan-900/40 bg-[#0a1419] px-4 py-1.5 text-slate-300 transition hover:bg-slate-800/60"
        >⏸ stop</button>
      </div>
      <p className={`max-w-xl text-center text-xs leading-relaxed ${error ? 'text-rose-400' : 'text-slate-500'}`}>{status}</p>
    </div>
  )
}
