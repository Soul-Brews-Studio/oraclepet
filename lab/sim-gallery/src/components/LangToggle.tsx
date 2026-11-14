import { useLang } from '../i18n/LangContext'
import type { Lang } from '../i18n/strings'

/** Two-pill EN / TH switch. Slides a glow under the active language. */
export default function LangToggle() {
  const { lang, setLang } = useLang()
  const options: Lang[] = ['en', 'th']
  return (
    <div
      role="group"
      aria-label="language"
      className="relative inline-flex font-mono text-2xs items-center gap-0 border border-embers rounded-full bg-coal/60 p-0.5 select-none"
    >
      {options.map((l) => {
        const active = lang === l
        return (
          <button
            key={l}
            type="button"
            onClick={() => setLang(l)}
            aria-pressed={active}
            className={
              'px-2.5 py-0.5 rounded-full transition ' +
              (active
                ? 'bg-glow/15 text-glow shadow-[0_0_10px_-2px_rgba(245,158,11,0.55)]'
                : 'text-ash hover:text-chalk')
            }
          >
            {l.toUpperCase()}
          </button>
        )
      })}
    </div>
  )
}
