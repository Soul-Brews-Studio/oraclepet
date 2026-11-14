import { createContext, useCallback, useContext, useEffect, useMemo, useState } from 'react'
import { STR, type Lang, type StrKey } from './strings'

interface LangValue {
  lang: Lang
  setLang: (l: Lang) => void
  t: (key: StrKey) => string
}

const Ctx = createContext<LangValue | null>(null)
const STORAGE_KEY = 'sim-gallery.lang'

function initialLang(): Lang {
  // Thai-first gallery: default to TH. A saved choice (or the EN toggle) wins,
  // and persists in localStorage.
  if (typeof window === 'undefined') return 'th'
  const saved = window.localStorage.getItem(STORAGE_KEY)
  if (saved === 'en' || saved === 'th') return saved
  return 'th'
}

export function LangProvider({ children }: { children: React.ReactNode }) {
  const [lang, setLangRaw] = useState<Lang>(initialLang)

  const setLang = useCallback((l: Lang) => {
    setLangRaw(l)
    try { window.localStorage.setItem(STORAGE_KEY, l) } catch { /* ignore */ }
    // Keep the document language in sync for screen readers + Tailwind's lang hooks.
    if (typeof document !== 'undefined') document.documentElement.lang = l
  }, [])

  useEffect(() => { if (typeof document !== 'undefined') document.documentElement.lang = lang }, [lang])

  const t = useCallback((key: StrKey) => STR[key][lang], [lang])

  const value = useMemo<LangValue>(() => ({ lang, setLang, t }), [lang, setLang, t])
  return <Ctx.Provider value={value}>{children}</Ctx.Provider>
}

export function useLang(): LangValue {
  const v = useContext(Ctx)
  if (!v) throw new Error('useLang must be used inside <LangProvider>')
  return v
}
