import { useEffect, useState } from 'react'

interface RailItem {
  id: string
  n: string
  label: string
}

/**
 * Fixed left-side TOC: 5 numbered markers, the active one is highlighted.
 * Hidden under lg (the page is mobile-first single-column there). Uses
 * IntersectionObserver with a generous rootMargin so "active" feels like
 * "where the user is looking", not "what's pixel-touched the top".
 */
export default function SectionRail({ items }: { items: RailItem[] }) {
  const [active, setActive] = useState(items[0]?.id ?? '')

  useEffect(() => {
    if (items.length === 0) return
    const observed = new Map<string, HTMLElement>()
    items.forEach((it) => {
      const el = document.getElementById(it.id)
      if (el) observed.set(it.id, el)
    })
    if (observed.size === 0) return

    const obs = new IntersectionObserver(
      (entries) => {
        const visible = entries.filter((e) => e.isIntersecting)
        if (visible.length === 0) return
        // Pick the one closest to the rootMargin center
        const top = visible.sort((a, b) => a.boundingClientRect.top - b.boundingClientRect.top)[0]
        setActive(top.target.id)
      },
      // Mark "active" when the section's top is in the upper-mid of the viewport.
      { rootMargin: '-30% 0px -55% 0px', threshold: 0 }
    )
    observed.forEach((el) => obs.observe(el))
    return () => obs.disconnect()
  }, [items])

  return (
    <nav
      aria-label="section navigation"
      className="hidden lg:flex fixed right-6 top-1/2 -translate-y-1/2 z-20 flex-col items-end gap-3 font-mono text-2xs"
    >
      {items.map((it) => {
        const isActive = active === it.id
        return (
          <a
            key={it.id}
            href={`#${it.id}`}
            aria-current={isActive ? 'true' : undefined}
            className={
              // flex-row-reverse: tick hugs the right edge, number left of it,
              // label flies out LEFT on hover (away from the screen edge).
              'group flex flex-row-reverse items-center gap-2.5 outline-none transition ' +
              (isActive ? 'text-glow' : 'text-ash/40 hover:text-ash')
            }
          >
            <span
              className={
                'block h-px transition-all duration-300 ' +
                (isActive ? 'w-10 bg-glow' : 'w-4 bg-ash/40 group-hover:w-6 group-hover:bg-ash')
              }
            />
            <span className="tabular-nums tracking-wider2">{it.n}</span>
            {/* Label is hover-only + a dark chip — the active state is carried by the
             * glowing number + long tick. Showing the full title persistently
             * overflowed the centered-layout gutter and collided with the sim. */}
            <span
              className="opacity-0 group-hover:opacity-100 transition-opacity duration-200
                         whitespace-nowrap rounded bg-ink/90 px-2 py-0.5 pointer-events-none
                         ring-1 ring-embers/60"
            >
              {it.label}
            </span>
          </a>
        )
      })}
    </nav>
  )
}
