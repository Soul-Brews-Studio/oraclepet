import PetSim from '../sims/pet/PetSim'
import EmscriptenSim from '../components/EmscriptenSim'
import DeviceFrame from '../components/DeviceFrame'
import SectionRail from '../components/SectionRail'
import { useLang } from '../i18n/LangContext'
import type { StrKey } from '../i18n/strings'
import { FLEET_SNAPSHOT } from '../sims/term/snapshot'

// esp32-exp's terminal sim exposes term_set_content(const char*) via ccall.
// Push a real (frozen) fleet snapshot once the module is live so the browser
// terminal shows authentic fleet text — "the fleet in your palm", baked in.
type TermInstance = { ccall?: (name: string, ret: string | null, types: string[], args: unknown[]) => void }
function pushFleetSnapshot(inst: unknown) {
  const term = inst as TermInstance
  // esp32-exp's contract (sim/GALLERY-INTEGRATION.md): ret type is null (void).
  term.ccall?.('term_set_header', null, ['string'], ['maw fleet | m5 | 6 nodes'])
  term.ccall?.('term_set_content', null, ['string'], [FLEET_SNAPSHOT])
}

/**
 * Single-page gallery: every sim alive on the home screen, no nested routes.
 * Hero opens the page; sections stack below with hairline cards. The left
 * SectionRail (≥lg) gives the long scroll a spine.
 */
export default function Gallery() {
  const { t, lang } = useLang()
  const displayClass = lang === 'th' ? 'font-sans' : 'font-display'

  const sections: { id: string; n: string; titleKey: StrKey }[] = [
    { id: 'bufo',       n: '01', titleKey: 's1Title' },
    { id: 'wasm',       n: '02', titleKey: 's2Title' },
    { id: 'pet-lvgl',   n: '03', titleKey: 's3Title' },
    { id: 'oracle-v1',  n: '04', titleKey: 's4Title' },
    { id: 'oracle-v2',  n: '05', titleKey: 's5Title' },
    { id: 'term',       n: '06', titleKey: 's6Title' },
    { id: 'term-live',  n: '07', titleKey: 's7Title' },
    { id: 'flash',      n: '08', titleKey: 's8Title' }
  ]

  return (
    <div className="relative">
      <SectionRail
        items={sections.map((s) => ({ id: s.id, n: s.n, label: t(s.titleKey) }))}
      />

      {/* Hero — the gallery opening. */}
      <section
        aria-labelledby="hero-title"
        className="relative overflow-hidden bg-hero-flame"
      >
        <div className="mx-auto max-w-7xl px-6 pt-20 pb-12">
          <p className="kicker mb-6">{t('heroKicker')}</p>

          <h1
            id="hero-title"
            className={`${displayClass} text-4xl sm:text-5xl md:text-6xl text-chalk max-w-4xl ${lang === 'en' ? 'tracking-tight2' : 'leading-[1.15]'}`}
          >
            {lang === 'en' ? (
              <>
                {t('heroTitleA')}{' '}
                <span className="italic" style={{ fontVariationSettings: "'opsz' 144, 'wght' 400" }}>
                  {t('heroTitleEm')}
                </span>{' '}
                {t('heroTitleB')}
              </>
            ) : (
              <>
                {t('heroTitleA')}{' '}
                <span className="text-glow font-semibold">{t('heroTitleEm')}</span>{' '}
                {t('heroTitleB')}
              </>
            )}
          </h1>

          <p className="mt-6 max-w-2xl text-lg text-ash leading-relaxed">
            {t('heroSubtitle')}
          </p>

          <div className="mt-10 flex items-center gap-4 flex-wrap">
            {/* The headline act: flash a real pet from the browser. Amber-filled
             * + flame glow so it's the first thing the eye lands on. */}
            <a
              href="/flash/"
              className="group inline-flex items-center gap-2 font-mono text-sm font-semibold text-ink
                         bg-glow hover:bg-glow-soft rounded-md px-4 py-2.5 transition-colors shadow-flame"
            >
              <span aria-hidden="true">⚡</span>
              <span>{t('heroCtaFlash')}</span>
              <span className="transition group-hover:translate-x-0.5">→</span>
            </a>
            <a
              href="#bufo"
              className="group inline-flex items-center gap-2 font-mono text-sm text-chalk
                         border border-embers rounded-md px-4 py-2.5 bg-coal/60
                         hover:border-glow/80 hover:text-glow transition"
            >
              <span>{t('heroCtaPrimary')}</span>
              <span className="text-glow transition group-hover:translate-x-0.5">→</span>
            </a>
            <a
              href="#wasm"
              className="font-mono text-sm text-ash hover:text-chalk transition"
            >
              {t('heroCtaSecondary')}
            </a>
          </div>

          {/* The gallery's "track index" — one hairline-divided panel (not eight
           * floating tiles), big ghosted track-numbers that ignite amber on
           * hover, and a tab-style glow underline. 8-up on lg so 08 isn't an
           * orphan. Reads as a control surface, not a generic card grid. */}
          <nav
            aria-label="sections preview"
            className="mt-16 grid grid-cols-2 sm:grid-cols-4 lg:grid-cols-8 gap-px
                       overflow-hidden rounded-xl border border-embers/60 bg-embers/50 shadow-card"
          >
            {sections.map((s) => (
              <a
                key={s.id}
                href={`#${s.id}`}
                className="group relative flex flex-col gap-2.5 bg-coal/90 px-4 py-5
                           transition-colors duration-200 hover:bg-dusk"
              >
                <span
                  aria-hidden="true"
                  className="font-mono text-3xl leading-none tabular-nums text-ash/55
                             transition-colors duration-200
                             group-hover:text-glow group-focus-visible:text-glow"
                >
                  {s.n}
                </span>
                <span className="font-sans text-xs text-ash leading-snug line-clamp-2
                                 transition-colors duration-200 group-hover:text-chalk">
                  {t(s.titleKey)}
                </span>
                {/* tab-style indicator: an amber underline that wipes in on hover/focus */}
                <span
                  aria-hidden="true"
                  className="pointer-events-none absolute inset-x-0 bottom-0 h-px w-0 bg-glow
                             transition-all duration-300 group-hover:w-full group-focus-visible:w-full"
                />
              </a>
            ))}
          </nav>
        </div>
        <div className="hairline mx-auto max-w-7xl mt-2" />
      </section>

      <div className="mx-auto max-w-7xl px-6 py-16 space-y-20">
        <SimSection
          id="bufo" number="01"
          titleKey="s1Title" boardKey="s1Board" badgeKey="s1Badge"
          blurb={
            <>
              {t('s1BlurbHead')}
              <code className="font-mono text-glow/90">pet.cpp</code>{' '}
              — <code className="font-mono text-chalk/80">waiting&gt;0→attention</code>,{' '}
              <code className="font-mono text-chalk/80">running&gt;0→busy</code>,{' '}
              <code className="font-mono text-chalk/80">total==0→sleep</code>.
              {t('s1BlurbTail')}
            </>
          }
        >
          <PetSim />
        </SimSection>

        <SimSection
          id="wasm" number="02"
          titleKey="s2Title" boardKey="s2Board" badgeKey="s2Badge"
          blurb={t('s2Blurb')}
        >
          <iframe
            src="/embeds/gif-wasm/index.html"
            title="gif-wasm browser demo"
            className="w-full h-[640px] rounded-xl border border-embers bg-black"
            allow="autoplay"
          />
        </SimSection>

        <SimSection
          id="pet-lvgl" number="03"
          titleKey="s3Title" boardKey="s3Board" badgeKey="s3Badge"
          blurb={t('s3Blurb')}
          aside={<SpecSheet hint={t('specTapRun')} rows={[
            ['render', 'LVGL + AnimatedGIF'],
            ['canvas', '320×480'],
            ['build', 'emcc · MODULARIZE'],
            ['assets', <span key="a">GIFs <span className="text-glow/80">embedded</span> (.wasm)</span>]
          ]} />}
        >
          <DeviceFrame label="JC3248W535 · 320×480"><EmscriptenSim src="/sims/pet/pet_sim.js" factory="PetSim" width={320} height={620} /></DeviceFrame>
        </SimSection>

        <SimSection
          id="oracle-v1" number="04"
          titleKey="s4Title" boardKey="s4Board" badgeKey="s4Badge"
          blurb={t('s4Blurb')}
          aside={<SpecSheet hint={t('specTapRun')} rows={[
            ['ui', 'LVGL labels'],
            ['canvas', '320×480'],
            ['build', 'emcc · MODULARIZE'],
            ['note', 'first / skeleton sim']
          ]} />}
        >
          <DeviceFrame label="JC3248W535 · 320×480"><EmscriptenSim src="/sims/oracle-v1/oracle_face_sim_v1.js" factory="OracleFaceV1" width={320} height={480} /></DeviceFrame>
        </SimSection>

        <SimSection
          id="oracle-v2" number="05"
          titleKey="s5Title" boardKey="s5Board" badgeKey="s5Badge"
          blurb={t('s5Blurb')}
          aside={<SpecSheet hint={t('specTapRun')} rows={[
            ['ui', 'LVGL 10-card grid'],
            ['canvas', '320×480'],
            ['mirrors', <code key="m" className="text-glow/80">jc3248w535.yaml</code>],
            ['build', 'emcc · MODULARIZE']
          ]} />}
        >
          <DeviceFrame label="JC3248W535 · 320×480"><EmscriptenSim src="/sims/oracle-v2/oracle_face_sim_v2.js" factory="OracleFaceV2" width={320} height={480} /></DeviceFrame>
        </SimSection>

        {/* — esp32-exp's fleet-pulse terminal (sibling oracle, EXPORT_NAME=createOracleTerm) — */}
        <SimSection
          id="term" number="06"
          titleKey="s6Title" boardKey="s6Board" badgeKey="s6Badge"
          blurb={t('s6Blurb')}
          aside={<SpecSheet hint={t('specTapRun')} rows={[
            ['render', 'LVGL recolor terminal'],
            ['canvas', '320×480'],
            ['data', <span key="d">live <code className="text-glow/80">maw ls</code> (frozen)</span>],
            ['by', <span key="b">esp32-exp · <code className="text-glow/80">createOracleTerm</code></span>]
          ]} />}
        >
          <DeviceFrame label="JC3248W535 · 320×480" glow><EmscriptenSim
            src="/sims/term/oracle_term_sim.js"
            factory="createOracleTerm"
            width={320}
            height={480}
            onReady={pushFleetSnapshot}
          /></DeviceFrame>
        </SimSection>

        {/* — esp32-exp's LIVE terminal viewer: the standalone non-MODULARIZE
            .html (its own maw-URL/pane inputs + 3s /api/capture poll). Full-width
            iframe like §02 so its global `Module` stays isolated from React.
            Streams the real fleet on-LAN; the public https deploy can't reach
            the LAN maw (mixed-content + RFC1918) so it shows the built-in sample. */}
        <SimSection
          id="term-live" number="07"
          titleKey="s7Title" boardKey="s7Board" badgeKey="s7Badge"
          blurb={t('s7Blurb')}
        >
          <iframe
            src="/embeds/term-live/oracle_term_sim.html"
            title="live fleet terminal viewer"
            className="w-full h-[760px] rounded-xl border border-embers bg-black"
            allow="autoplay"
          />
        </SimSection>

        {/* — §08: the browser web flasher embedded INLINE (esp-web-tools). Same-
            origin iframe → Web Serial permission delegates fine; user picks a pack
            + CONNECTs + INSTALLs without leaving the home page. Public-safe. — */}
        <SimSection
          id="flash" number="08"
          titleKey="s8Title" boardKey="s8Board" badgeKey="s8Badge"
          blurb={t('s8Blurb')}
        >
          <div className="flex flex-col items-center gap-3">
            <iframe
              src="/flash/"
              title="esp-web-tools web flasher"
              className="w-full h-[1200px] rounded-xl border border-embers bg-ink"
              allow="serial; usb"
            />
            <a
              href="/flash/"
              target="_blank"
              rel="noreferrer"
              className="font-mono text-2xs text-ash hover:text-glow transition"
            >
              {t('s8Cta')} ↗
            </a>
          </div>
        </SimSection>

        <aside className="font-mono text-2xs text-ash space-y-1 pt-10 border-t border-embers/80">
          <div>
            {t('buildHintA')} <code className="text-glow">make build-sims</code>
          </div>
          <div>
            {t('buildHintB')} <code className="text-glow">pnpm dev</code> ·
            {' '}{t('buildHintC')} <code className="text-glow">pnpm build</code> ·
            {' '}{t('buildHintD')} <code className="text-glow">pnpm deploy</code>
          </div>
        </aside>
      </div>
    </div>
  )
}

/** A polished section card — hairline border, hover-glow, indexed kicker.
 * When `aside` is given, the body is a device-left / info-right two-column on
 * lg+ (kills the dead space beside a narrow 320px sim + adds a spec sheet). */
function SimSection({
  id, number, titleKey, boardKey, blurb, badgeKey, children, aside
}: {
  id: string
  number: string
  titleKey: StrKey
  boardKey: StrKey
  badgeKey: StrKey
  blurb: React.ReactNode
  children: React.ReactNode
  aside?: React.ReactNode
}) {
  const { t, lang } = useLang()
  const displayClass = lang === 'th' ? 'font-sans' : 'font-display'
  return (
    <section id={id} className="scroll-mt-24 group">
      <div className="flex items-start gap-5 mb-5">
        <div className="font-mono text-xs text-glow/70 mt-2 tabular-nums tracking-wider2">
          {number}
        </div>
        <div className="flex-1 min-w-0">
          <div className="flex items-baseline gap-3 flex-wrap">
            <h2 className={`${displayClass} text-2xl text-chalk tracking-tight2`}>{t(titleKey)}</h2>
            <span className="font-mono text-2xs text-glow/80 border border-glow/30 rounded px-1.5 py-0.5 uppercase tracking-wider2">
              {t(badgeKey)}
            </span>
          </div>
          <div className="font-mono text-2xs text-ash mt-1.5">{t(boardKey)}</div>
          <p className="mt-3 text-sm text-ash max-w-3xl leading-relaxed">{blurb}</p>
        </div>
      </div>

      <div className="relative rounded-2xl border border-embers/80 bg-coal/60 p-6 sm:p-8
                      transition-colors duration-200
                      group-hover:border-glow/25 focus-within:border-glow/40
                      shadow-card">
        {aside ? (
          <div className="grid gap-8 lg:grid-cols-[max-content_1fr] items-start justify-items-center lg:justify-items-start">
            <div>{children}</div>
            <div className="w-full max-w-md">{aside}</div>
          </div>
        ) : children}
      </div>
    </section>
  )
}

/** A small mono spec sheet shown beside a device-framed sim. */
function SpecSheet({ rows, hint }: { rows: [string, React.ReactNode][]; hint?: string }) {
  return (
    <div className="font-mono text-xs">
      <dl className="grid grid-cols-[max-content_1fr] gap-x-5 gap-y-2">
        {rows.map(([k, v], i) => (
          <div key={i} className="contents">
            <dt className="text-ash/70">{k}</dt>
            <dd className="text-chalk/90">{v}</dd>
          </div>
        ))}
      </dl>
      {hint && (
        <p className="mt-5 text-2xs text-ash/70 flex items-center gap-2">
          <span className="text-glow">▶</span> {hint}
        </p>
      )}
    </div>
  )
}
