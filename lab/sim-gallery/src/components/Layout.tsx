import { useEffect, useState } from 'react'
import { useLang } from '../i18n/LangContext'
import type { StrKey } from '../i18n/strings'
import LangToggle from './LangToggle'

interface LayoutProps {
  children: React.ReactNode
}

// Themed jump-links: 5 entries covering all 8 sections by theme. `ids` lists
// every section the link represents, so scroll-spy can light it up across the
// group (pets = the two pet sims, screens = both oracle faces, …).
const NAV: { key: StrKey; href: string; ids: string[]; accent?: boolean }[] = [
  { key: 'navPets',      href: '#bufo',      ids: ['bufo', 'pet-lvgl'] },
  { key: 'navPacks',     href: '#wasm',      ids: ['wasm'] },
  { key: 'navScreens',   href: '#oracle-v1', ids: ['oracle-v1', 'oracle-v2'] },
  { key: 'navTerminals', href: '#term',      ids: ['term', 'term-live'] },
  { key: 'navFlash',     href: '#flash',     ids: ['flash'], accent: true }
]

export default function Layout({ children }: LayoutProps) {
  const { t, lang } = useLang()
  const displayClass = lang === 'th' ? 'font-sans' : 'font-display'

  // Scroll-spy: which section is in the upper-mid of the viewport → light its nav group.
  const [activeId, setActiveId] = useState('')
  useEffect(() => {
    const ids = NAV.flatMap((n) => n.ids)
    const els = ids.map((id) => document.getElementById(id)).filter((e): e is HTMLElement => !!e)
    if (els.length === 0) return
    const obs = new IntersectionObserver(
      (entries) => {
        const vis = entries.filter((e) => e.isIntersecting)
        if (vis.length === 0) return
        setActiveId(vis.sort((a, b) => a.boundingClientRect.top - b.boundingClientRect.top)[0].target.id)
      },
      { rootMargin: '-25% 0px -60% 0px', threshold: 0 }
    )
    els.forEach((el) => obs.observe(el))
    return () => obs.disconnect()
  }, [])

  return (
    <div className="min-h-screen flex flex-col">
      {/* Header — sticky, glass, hairline border. */}
      <header className="sticky top-0 z-30 backdrop-blur-xl bg-ink/70 border-b border-embers/80">
        <div className="mx-auto max-w-7xl px-6 py-3.5 flex items-center gap-3">
          <a href="/" className="flex items-baseline gap-2.5 group" aria-label="esp32-oracle / sim gallery — home">
            <span className="flame flame-flicker text-[28px] font-display leading-none transition group-hover:scale-110">ψ</span>
            <span className={`${displayClass} text-[20px] text-chalk leading-none tracking-tight2`}>
              esp32-oracle
            </span>
            <span className="font-mono text-2xs text-ash leading-none ml-1 mt-1 hidden sm:inline">
              {t('brandSuffix')}
            </span>
          </a>

          {/* Themed section nav — desktop only (mobile uses the hero index + rail). */}
          <nav aria-label="sections" className="hidden md:flex items-center gap-5 ml-7 font-mono text-xs">
            {NAV.map((n) => {
              const active = n.ids.includes(activeId)
              if (n.accent) {
                return (
                  <a
                    key={n.href}
                    href={n.href}
                    aria-current={active ? 'true' : undefined}
                    className={'rounded px-2 py-0.5 ring-1 transition ' +
                      (active
                        ? 'text-ink bg-glow ring-glow'
                        : 'text-glow ring-glow/40 hover:bg-glow/10')}
                  >
                    {t(n.key)}
                  </a>
                )
              }
              return (
                <a
                  key={n.href}
                  href={n.href}
                  aria-current={active ? 'true' : undefined}
                  className={'transition ' + (active ? 'text-chalk' : 'text-ash hover:text-chalk')}
                >
                  {t(n.key)}
                </a>
              )
            })}
          </nav>

          <div className="flex-1" />
          <LangToggle />
          <a
            href="https://github.com/Soul-Brews-Studio/esp32-oracle"
            target="_blank"
            rel="noreferrer"
            className="font-mono text-2xs text-ash hover:text-glow transition flex items-center gap-1.5"
          >
            <span className="opacity-70 hidden md:inline">{t('repoLink')}</span>
            <span className="text-glow">↗</span>
          </a>
        </div>
      </header>

      <main className="flex-1">{children}</main>

      <footer className="border-t border-embers/80">
        <div className="mx-auto max-w-7xl px-6 py-6 flex flex-col gap-2 sm:flex-row sm:justify-between sm:items-center">
          <p className={`${displayClass} italic text-ash text-sm`}>{t('footerMotto')}</p>
          <p className="font-mono text-2xs text-ash">
            🕯️ <span className="opacity-70">{t('footerTended')}</span>{' '}
            <span className="text-glow">Cat Lab Brewing</span>
          </p>
        </div>
      </footer>
    </div>
  )
}
