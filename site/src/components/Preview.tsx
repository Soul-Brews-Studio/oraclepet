import { useEffect, useRef, useState } from 'react'
import { Download, Hand, Play, Square, Eye } from 'lucide-react'
import { PetSim, STATE_GIF, CANVAS_W, CANVAS_H, type SimView } from '../lib/sim'
import type { Catalog } from '../lib/catalog'

const STATES = ['idle', 'busy', 'attention', 'sleep'] as const

/** The pet's 320×480 screen in the browser: a character's animations, and the example .wasm apps
 * running on the same API the firmware gives them (lab/pet-sdk/pet.h). Click the screen to tap. */
export function Preview({ cat, pick }: { cat: Catalog; pick?: { id: string; n: number } | null }) {
  const ids = cat.packs.map((p) => p.id)
  const [pack, setPack] = useState(ids.includes('siamese') ? 'siamese' : ids[0])
  const [app, setApp] = useState('')
  const [view, setView] = useState<SimView | null>(null)
  const [manualState, setManualState] = useState(0)
  const [, tick] = useState(0)
  const sim = useRef<PetSim | null>(null)
  const cv = useRef<HTMLCanvasElement>(null)

  useEffect(() => () => sim.current?.stop(), [])
  // a character clicked in the hero
  useEffect(() => { if (pick && ids.includes(pick.id)) choosePack(pick.id) }, [pick?.n]) // eslint-disable-line react-hooks/exhaustive-deps
  // heart / state changes are time based: redraw a few times a second while something runs
  useEffect(() => { const t = setInterval(() => tick((n) => n + 1), 250); return () => clearInterval(t) }, [])
  useEffect(() => {
    if (view?.canvas && sim.current && cv.current) cv.current.getContext('2d')!.drawImage(sim.current.cv, 0, 0)
  }, [view?.frame, view?.canvas])

  async function run(name: string) {
    sim.current?.stop()
    setApp(name)
    if (!name) { sim.current = null; setView(null); return }
    const s = new PetSim(pack, (v) => setView(v), ids)
    sim.current = s
    setView(s.view)
    const bytes = await (await fetch(`apps/${name}.wasm`)).arrayBuffer()
    await s.load(bytes)
  }

  function choosePack(p: string) {
    setPack(p)
    if (sim.current && view) { sim.current.view = { ...view, pack: p }; setView(sim.current.view) }
  }

  function tap(e: React.MouseEvent<HTMLDivElement>) {
    const r = e.currentTarget.getBoundingClientRect()
    const x = Math.round(((e.clientX - r.left) * 320) / r.width), y = Math.round(((e.clientY - r.top) * 480) / r.height)
    if (sim.current) sim.current.tap(x, y)
    else setHeartUntil(performance.now() + 1500)
  }
  const [heartUntil, setHeartUntil] = useState(0)

  const shown = view ?? { state: manualState, heartUntil, caption: '', status: 'no app: tap me, or pick a state', pack, log: [], canvas: false, frame: 0 }
  const gif = `packs/${shown.pack}/${shown.heartUntil > performance.now() ? 'heart' : STATE_GIF[shown.state]}.gif`

  return (
    <div className="grid gap-6 lg:grid-cols-[1fr_auto]">
      <div className="space-y-5">
        <div>
          <p className="mb-2 text-sm font-medium">Character</p>
          <div className="flex flex-wrap gap-2">
            {cat.packs.map((p) => (
              <button key={p.id} onClick={() => choosePack(p.id)} aria-pressed={p.id === shown.pack} title={p.name}
                className={`size-12 overflow-hidden rounded-lg border bg-black ${p.id === shown.pack ? 'border-amber' : 'border-ink-800 hover:border-ink-700'}`}>
                <img src={p.image} alt={p.name} className="pixel size-full object-contain" />
              </button>
            ))}
          </div>
        </div>
        {!view && (
          <div>
            <p className="mb-2 text-sm font-medium">State <span className="font-normal text-ink-400">· what a heartbeat from the host sets</span></p>
            <div className="flex flex-wrap gap-2">
              {STATES.map((s, i) => (
                <button key={s} onClick={() => setManualState(i)} aria-pressed={manualState === i}
                  className={`rounded-lg border px-3 py-1.5 text-sm ${manualState === i ? 'border-amber bg-ink-800' : 'border-ink-800 bg-ink-900 hover:border-ink-700'}`}>{s}</button>
              ))}
            </div>
          </div>
        )}
        <div>
          <p className="mb-2 text-sm font-medium">App <span className="font-normal text-ink-400">· example .wasm apps from lab/pet-sdk, running here as on the pet</span></p>
          {cat.apps.length === 0 && <p className="text-sm text-ink-400">No example apps in this build.</p>}
          <div className="flex flex-wrap gap-2">
            {cat.apps.map((a) => (
              <button key={a} onClick={() => run(a)} aria-pressed={app === a}
                className={`inline-flex items-center gap-1.5 rounded-lg border px-3 py-1.5 font-mono text-sm ${app === a ? 'border-amber bg-ink-800' : 'border-ink-800 bg-ink-900 hover:border-ink-700'}`}>
                <Play className="size-3.5" />{a}
              </button>
            ))}
            {app && (
              <button onClick={() => run('')} className="inline-flex items-center gap-1.5 rounded-lg border border-ink-800 px-3 py-1.5 text-sm hover:border-ink-700">
                <Square className="size-3.5" />Stop
              </button>
            )}
          </div>
          {app && (
            <a href={`apps/${app}.wasm`} download className="mt-3 inline-flex items-center gap-1.5 text-sm text-amber hover:underline">
              <Download className="size-4" />{app}.wasm · send it to a real pet from the Bluetooth tool
            </a>
          )}
        </div>
        {view && view.log.length > 0 && (
          <pre className="max-h-40 overflow-auto rounded-lg border border-ink-800 bg-ink-950 p-3 font-mono text-xs text-ink-400">{view.log.join('\n')}</pre>
        )}
      </div>

      <div className="mx-auto">
        <div id="pet-screen" onClick={tap} role="button" aria-label="The pet screen: click to tap"
          className="relative box-content h-[480px] w-[320px] cursor-pointer select-none overflow-hidden rounded-[22px] border-[10px] border-ink-800 bg-black shadow-2xl">
          {shown.canvas
            ? <canvas ref={cv} width={CANVAS_W} height={CANVAS_H} className="absolute inset-x-0 top-0" />
            : <>
                <img src={gif} alt="" className="pixel absolute left-[6px] top-[24px] h-[300px] w-[288px] object-contain" />
                <div className="absolute inset-x-0 top-[340px] text-center font-mono text-lg font-semibold text-white">{shown.caption}</div>
              </>}
          <div className="absolute inset-x-0 top-[400px] border-t border-ink-700" />
          <div className="absolute inset-x-3 top-[414px] truncate font-mono text-sm text-ink-200">{shown.status}</div>
          <span className="absolute right-2 top-2 rounded bg-ink-900/80 px-1.5 py-0.5 font-mono text-[10px] text-ink-400">
            {shown.canvas ? 'canvas' : STATE_GIF[shown.state]}
          </span>
        </div>
        <p className="mt-2 flex items-center justify-center gap-1.5 text-xs text-ink-400"><Hand className="size-3.5" />click the screen to tap</p>
      </div>
    </div>
  )
}

export const PreviewIcon = Eye
