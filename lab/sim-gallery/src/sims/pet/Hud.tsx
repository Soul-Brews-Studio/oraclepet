import {
  HUD_H,
  PACK,
  STALE_MS,
  fmtWindow,
  haveUsage,
  type Link,
  type Palette,
  type Snapshot
} from './petLogic'

interface HudProps {
  pal: Palette
  snap: Snapshot
  link: Link
  reacting: boolean
  now: number
}

export default function Hud({ pal, snap, link, reacting, now }: HudProps) {
  let lines: string[]
  let dim: boolean[] = []

  const usage = haveUsage(snap)
  if (link.connected && now - link.lastSnap <= STALE_MS && usage) {
    // Lines 2-3: the two usage windows — "5h 8% 3h55m" / "wk 41% 4d23h".
    lines = [
      fmtWindow('5h', snap.pct5h, snap.reset5h),
      fmtWindow('wk', snap.pct7d, snap.reset7d)
    ]
    dim = [false, true]                          // wk line uses textDim
  } else {
    // Line 2: activity when linked, else local state + uptime.
    let status: string
    if (link.connected) {
      if (now - link.lastSnap > STALE_MS) status = 'BLE link stale'
      else if (snap.msg) status = snap.msg
      else status = `run ${snap.running}  wait ${snap.waiting}  ${snap.tokens}t`
    } else {
      const up = Math.floor(now / 1000)
      status = `${reacting ? 'heart' : 'idle'}  BLE adv  ${Math.floor(up / 60)}:${String(up % 60).padStart(2, '0')}`
    }
    lines = [status]
    dim = [true]
  }

  // dot: orange while reacting (stands in for pairing), green linked, dim advertising.
  const dotColor = reacting ? '#ffae33' : link.connected ? '#33d17a' : '#5a6072'

  return (
    <div
      className="absolute inset-x-0 bottom-0 px-3 py-2.5 font-mono"
      style={{ height: HUD_H, borderTop: `1px solid ${pal.textDim}` }}
    >
      <span
        className="absolute rounded-full"
        style={{
          right: 12, top: 14, width: 12, height: 12, background: dotColor,
          boxShadow: `0 0 6px ${dotColor}`
        }}
      />
      <div className="font-bold leading-none" style={{ fontSize: 22, color: pal.text }}>
        {snap.name || PACK}
      </div>
      <div className="mt-2.5 space-y-1">
        {lines.map((t, i) => (
          <div key={i} className="leading-tight" style={{ fontSize: 14, color: dim[i] ? pal.textDim : pal.text }}>
            {t}
          </div>
        ))}
      </div>
    </div>
  )
}
