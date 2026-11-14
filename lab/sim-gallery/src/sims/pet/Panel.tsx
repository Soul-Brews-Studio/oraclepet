import { useCallback, useRef, useState } from 'react'
import { H, HUD_H, SCALE, W, type Link, type Palette, type Snapshot } from './petLogic'
import Hud from './Hud'
import { useLang } from '../../i18n/LangContext'

interface PanelProps {
  pal: Palette
  gifSrc: string
  snap: Snapshot
  link: Link
  reacting: boolean
  now: number
  onTap: () => void
}

export default function Panel({ pal, gifSrc, snap, link, reacting, now, onTap }: PanelProps) {
  const { t } = useLang()
  const imgRef = useRef<HTMLImageElement | null>(null)
  const [pos, setPos] = useState({ w: 0, h: 0, left: 0, top: 0 })

  // 3x nearest-neighbor, centered in the (H - HUD_H) area above the HUD (mirror gifPlace).
  const place = useCallback(() => {
    const img = imgRef.current
    if (!img || !img.naturalWidth) return
    const ow = img.naturalWidth * SCALE
    const oh = img.naturalHeight * SCALE
    setPos({
      w: ow, h: oh,
      left: Math.max(0, (W - ow) / 2),
      top:  Math.max(0, ((H - HUD_H) - oh) / 2)
    })
  }, [])

  return (
    <div
      onClick={onTap}
      title="click = tap the pet (heart)"
      className="relative overflow-hidden rounded-lg cursor-pointer select-none"
      style={{ width: W, height: H, background: pal.bg }}
    >
      <div
        className="absolute inset-x-0 text-center pointer-events-none"
        style={{ top: 6, fontSize: 11, color: '#ffffff55' }}
      >
        {t('petTapHint')}
      </div>

      {gifSrc && (
        <img
          ref={imgRef}
          src={gifSrc}
          onLoad={place}
          alt=""
          className="absolute"
          style={{
            width: pos.w, height: pos.h, left: pos.left, top: pos.top,
            imageRendering: 'pixelated'
          }}
        />
      )}

      <Hud pal={pal} snap={snap} link={link} reacting={reacting} now={now} />
    </div>
  )
}
