/**
 * A little "glass slab" bezel around a sim canvas/iframe — makes each sim read
 * as the physical 320×480 board it actually runs on (the desk-pet / fleet-in-
 * your-palm identity), and visually contains the black screen so it doesn't
 * float in a vast empty card. Pure CSS, no deps.
 */
export default function DeviceFrame({
  children,
  label,
  glow = false
}: {
  children: React.ReactNode
  label?: string
  glow?: boolean
}) {
  return (
    <div className="inline-block">
      <div
        className={
          'relative rounded-[22px] p-3 bg-gradient-to-b from-[#1a1b20] to-[#0c0d10] ' +
          'ring-1 ring-white/10 shadow-[0_30px_60px_-25px_rgba(0,0,0,0.9)] ' +
          (glow ? 'shadow-flame' : '')
        }
      >
        {/* top bezel: speaker slit + camera dot — reads as a device top edge */}
        <div className="flex items-center justify-center gap-2 pb-2 pt-0.5">
          <span className="h-1 w-10 rounded-full bg-white/10" />
          <span className="h-1 w-1 rounded-full bg-white/15" />
        </div>
        {/* the screen */}
        <div className="overflow-hidden rounded-[10px] bg-black ring-1 ring-black/60">
          {children}
        </div>
        {/* bottom bezel label (board id) */}
        {label && (
          <div className="pt-2 text-center font-mono text-[9px] tracking-wider2 uppercase text-ash/60">
            {label}
          </div>
        )}
      </div>
    </div>
  )
}
