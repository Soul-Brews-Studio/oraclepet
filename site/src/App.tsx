import { useEffect, useMemo, useState } from 'react'
import {
  Bluetooth, Check, ChevronRight, Cpu, Download, FileCode2, FolderGit2, RotateCcw, ShieldAlert, TriangleAlert, Usb, Zap,
} from 'lucide-react'
import { loadCatalog, partsFor, kb, hex, type Board, type Catalog, type Pack } from './lib/catalog'
import { flash, usbApi, type Progress } from './lib/flash'
import { Preview, PreviewIcon } from './components/Preview'

const REPO = 'https://github.com/Soul-Brews-Studio/oraclepet'

export default function App() {
  const [cat, setCat] = useState<Catalog | null>(null)
  const [err, setErr] = useState('')
  useEffect(() => { loadCatalog().then(setCat, (e) => setErr(String(e))) }, [])

  return (
    <div className="min-h-dvh">
      <Header />
      <main className="mx-auto max-w-5xl px-5 pb-24">
        <Hero packs={cat?.packs ?? []} />
        <Steps />
        {err && <Note tone="bad">Could not load the firmware list: {err}</Note>}
        {cat && (
          <section id="preview" className="mt-16 scroll-mt-6">
            <SectionTitle icon={PreviewIcon} title="Preview" sub="The pet's screen in your browser: every character's animations, and the example WebAssembly apps running on the same API the firmware gives them." />
            <Preview cat={cat} />
          </section>
        )}
        {cat && <Flasher cat={cat} />}
        {cat && <BaseSection cat={cat} />}
        <BleSection />
        <Footer cat={cat} />
      </main>
    </div>
  )
}

function Header() {
  return (
    <header className="border-b border-ink-800">
      <div className="mx-auto flex max-w-5xl items-center justify-between px-5 py-4">
        <a href="./" className="flex items-center gap-2 font-semibold tracking-tight">
          <img src="packs/bufo.gif" alt="" className="pixel size-7" />
          OraclePet
        </a>
        <nav className="flex items-center gap-4 text-sm text-ink-400 sm:gap-5">
          <a href="#preview" className="hidden hover:text-ink-50 sm:inline">Preview</a>
          <a href="#flash" className="hidden hover:text-ink-50 sm:inline">USB install</a>
          <a href="ble/" className="flex items-center gap-1.5 hover:text-ink-50" aria-label="Bluetooth tool"><Bluetooth className="size-4 sm:hidden" /><span className="hidden sm:inline">Bluetooth tool</span></a>
          <a href="firmware/" className="hidden hover:text-ink-50 sm:inline">Firmware</a>
          <a href={REPO} className="flex items-center gap-1.5 hover:text-ink-50" aria-label="Source on GitHub"><FolderGit2 className="size-4" /><span className="hidden sm:inline">Source</span></a>
        </nav>
      </div>
    </header>
  )
}

function Hero({ packs }: { packs: Pack[] }) {
  return (
    <section className="grid items-center gap-10 py-14 md:grid-cols-[1.1fr_1fr]">
      <div>
        <p className="mb-3 text-sm font-medium text-amber">Open source · ESP32-S3 · MIT</p>
        <h1 className="text-4xl font-semibold leading-tight tracking-tight md:text-5xl">
          A desk pet you flash from the browser.
        </h1>
        <p className="mt-5 max-w-prose text-lg text-ink-200">
          Plug the board in once over USB. After that, firmware, characters and WebAssembly apps
          go over Bluetooth, from this page or the Mac app.
        </p>
        <div className="mt-8 flex flex-wrap gap-3">
          <a href="#flash" className="inline-flex items-center gap-2 rounded-lg bg-amber px-5 py-3 font-medium text-ink-950 hover:bg-amber/90">
            <Usb className="size-4" /> Install over USB
          </a>
          <a href="ble/" className="inline-flex items-center gap-2 rounded-lg border border-ink-700 px-5 py-3 font-medium hover:border-amber">
            <Bluetooth className="size-4" /> Update over Bluetooth
          </a>
        </div>
      </div>
      <div className="grid grid-cols-4 gap-2 rounded-2xl border border-ink-800 bg-ink-900 p-3 sm:grid-cols-5">
        {packs.slice(0, 15).map((p) => (
          <figure key={p.id} className="aspect-square overflow-hidden rounded-lg bg-black" title={p.name}>
            <img src={p.image} alt={p.name} loading="lazy" className="pixel size-full object-contain" />
          </figure>
        ))}
        {packs.length === 0 && Array.from({ length: 10 }, (_, i) => <div key={i} className="aspect-square animate-pulse rounded-lg bg-ink-800" />)}
      </div>
    </section>
  )
}

function Steps() {
  const items = [
    { icon: Usb, title: 'Install once over USB', body: 'Pick your board and a character, press Install. Web Serial on desktop, WebUSB on Android.' },
    { icon: Bluetooth, title: 'Update over Bluetooth', body: 'New firmware goes to the idle slot and rolls back if it does not start. Characters and apps install without a reboot.' },
    { icon: FileCode2, title: 'Build your own', body: 'ESP-IDF firmware, a BLE protocol with C, Python, Swift and browser libraries, and an SDK for .wasm pet apps.' },
  ]
  return (
    <section className="grid gap-4 md:grid-cols-3">
      {items.map(({ icon: Icon, title, body }, i) => (
        <div key={title} className="rounded-xl border border-ink-800 bg-ink-900 p-5">
          <div className="mb-3 flex items-center gap-3">
            <span className="grid size-8 place-items-center rounded-lg bg-ink-800 text-amber"><Icon className="size-4" /></span>
            <span className="text-xs font-medium text-ink-400">Step {i + 1}</span>
          </div>
          <h3 className="font-semibold">{title}</h3>
          <p className="mt-1.5 text-sm text-ink-400">{body}</p>
        </div>
      ))}
    </section>
  )
}

function Flasher({ cat }: { cat: Catalog }) {
  const pets = cat.boards.filter((b) => !b.base)
  const [boardId, setBoardId] = useState(pets[0]?.id ?? '')
  const board = pets.find((b) => b.id === boardId) ?? pets[0]
  const [pack, setPack] = useState(board?.defaultPack ?? '')
  useEffect(() => { if (board && !board.packs.includes(pack)) setPack(board.defaultPack) }, [board, pack])
  const packs = useMemo(() => cat.packs.filter((p) => board?.packs.includes(p.id)), [cat, board])
  if (!board) return null

  return (
    <section id="flash" className="mt-16 scroll-mt-6">
      <SectionTitle icon={Usb} title="Install over USB" sub="Bootloader, partition table, the pet firmware and the 3 MB character storage." />
      <div className="grid gap-6 lg:grid-cols-[1fr_22rem]">
        <div className="space-y-6">
          <div>
            <Label>Board</Label>
            <div className="grid gap-2 sm:grid-cols-3">
              {pets.map((b) => (
                <Choice key={b.id} active={b.id === board.id} onClick={() => setBoardId(b.id)}>
                  <span className="font-medium">{b.name}</span>
                  <span className="text-xs text-ink-400">{b.screen}</span>
                </Choice>
              ))}
            </div>
            {board.note && <p className="mt-2 text-sm text-ink-400">{board.note}</p>}
          </div>
          <div>
            <Label>Character it starts with <span className="font-normal text-ink-400">· all {cat.packs.length} are installed, switch any time over Bluetooth</span></Label>
            <div className="grid grid-cols-3 gap-2 sm:grid-cols-4 md:grid-cols-5">
              {packs.map((p) => (
                <button
                  key={p.id}
                  onClick={() => setPack(p.id)}
                  aria-pressed={p.id === pack}
                  className={`group rounded-xl border p-2 text-left transition focus-visible:outline-2 focus-visible:outline-amber ${p.id === pack ? 'border-amber bg-ink-800' : 'border-ink-800 bg-ink-900 hover:border-ink-700'}`}
                >
                  <div className="aspect-square overflow-hidden rounded-lg bg-black">
                    <img src={p.image} alt="" loading="lazy" className="pixel size-full object-contain" />
                  </div>
                  <div className="mt-1.5 flex items-center justify-between text-sm">
                    <span className="truncate">{p.name}</span>
                    {p.id === pack && <Check className="size-4 text-amber" />}
                  </div>
                </button>
              ))}
            </div>
          </div>
        </div>
        <InstallPanel key={`${board.id}-${pack}`} board={board} pack={pack} label={`${board.name} · ${pack}`} />
      </div>
    </section>
  )
}

function InstallPanel({ board, pack, label, recover }: { board: Board; pack: string; label: string; recover?: boolean }) {
  const api = usbApi()
  const [eraseAll, setEraseAll] = useState(false)
  const [busy, setBusy] = useState(false)
  const [prog, setProg] = useState<Progress | null>(null)
  const [error, setError] = useState('')
  const [log, setLog] = useState<string[]>([])
  const parts = recover ? board.common.filter((p) => p.file === 'ota_data_initial.bin').map((p) => ({ ...p, url: `firmware/${board.id}/${p.file}` })) : partsFor(board, pack)

  async function go() {
    setBusy(true); setError(''); setLog([]); setProg(null)
    try {
      await flash({
        images: parts.map((p) => ({ url: p.url, offset: p.offset })),
        eraseAll: !recover && eraseAll,
        log: (l) => setLog((x) => [...x.slice(-200), l]),
        progress: setProg,
      })
    } catch (e) {
      const msg = e instanceof Error ? e.message : String(e)
      setError(/No port selected|NotFoundError/i.test(msg) ? 'No board was picked.' : msg)
    } finally {
      setBusy(false)
    }
  }

  return (
    <div className="h-fit rounded-xl border border-ink-800 bg-ink-900 p-5 lg:sticky lg:top-6">
      <p className="text-sm text-ink-400">{recover ? 'Back to base' : 'Ready to install'}</p>
      <p className="mt-0.5 font-semibold">{label}</p>
      <ul className="mt-4 space-y-1 font-mono text-xs text-ink-400">
        {parts.map((p) => (
          <li key={p.file} className="flex justify-between gap-3"><span className="truncate">{p.file}</span><span>{hex(p.offset)} · {kb(p.bytes)}</span></li>
        ))}
      </ul>
      {!recover && (
        <label className="mt-4 flex items-start gap-2 text-sm text-ink-200">
          <input type="checkbox" checked={eraseAll} onChange={(e) => setEraseAll(e.target.checked)} className="mt-0.5 accent-amber" />
          <span>Erase the whole flash first <span className="text-ink-400">(clears saved Wi-Fi and settings; use it if the board ran other firmware)</span></span>
        </label>
      )}
      <button
        onClick={go}
        disabled={busy || !api}
        className="mt-5 flex w-full items-center justify-center gap-2 rounded-lg bg-amber px-4 py-3 font-medium text-ink-950 hover:bg-amber/90 disabled:cursor-not-allowed disabled:opacity-50"
      >
        {recover ? <RotateCcw className="size-4" /> : <Zap className="size-4" />}
        {busy ? 'Working…' : recover ? 'Boot the base again' : 'Connect and install'}
      </button>
      {!api && <Note tone="warn">This browser has no Web Serial or WebUSB. Use Chrome or Edge on a computer, or Chrome on Android.</Note>}
      {api === 'usb' && <p className="mt-2 text-xs text-ink-400">Android: connecting over WebUSB to the board's USB port.</p>}
      {prog && (
        <div className="mt-4">
          <div className="h-2 overflow-hidden rounded-full bg-ink-800">
            <div className="h-full bg-amber transition-[width]" style={{ width: `${Math.round(prog.done * 100)}%` }} />
          </div>
          <p className={`mt-2 text-sm ${prog.phase === 'done' ? 'text-amber' : 'text-ink-200'}`}>{prog.message}</p>
        </div>
      )}
      {error && <Note tone="bad">{error}</Note>}
      {log.length > 0 && (
        <details className="mt-3 text-xs">
          <summary className="cursor-pointer text-ink-400">Log</summary>
          <pre className="mt-2 max-h-48 overflow-auto rounded bg-ink-950 p-2 font-mono text-ink-400">{log.join('\n')}</pre>
        </details>
      )}
      <p className="mt-4 text-xs text-ink-400">If the board is not listed: hold BOOT, tap RESET, let go of BOOT, then try again.</p>
    </div>
  )
}

function BaseSection({ cat }: { cat: Catalog }) {
  const base = cat.boards.find((b) => b.base)
  if (!base) return null
  return (
    <section className="mt-16">
      <SectionTitle icon={Cpu} title="OTA base: any firmware over Bluetooth" sub="A small firmware for any ESP32-S3 with 8 MB of flash. It only waits for an image over BLE, installs it and boots it." />
      <div className="grid gap-6 lg:grid-cols-[1fr_22rem]">
        <div className="space-y-4 text-ink-200">
          <ol className="space-y-3">
            {[
              'Install the base over USB (right). It also writes the 3 MB character storage, so a pet firmware sent later finds its characters.',
              'Open the Bluetooth tool, connect to Claude-XXXX and send any ESP32-S3 app image (.bin): a pet build from Firmware, or your own.',
              'The base checks it is an ESP32-S3 app image, its size and CRC, writes it to ota_0 or ota_1 and boots it.',
            ].map((t, i) => (
              <li key={i} className="flex gap-3"><span className="grid size-6 shrink-0 place-items-center rounded-full bg-ink-800 text-xs text-amber">{i + 1}</span><span>{t}</span></li>
            ))}
          </ol>
          <Note tone="warn">
            The base stays in its own factory partition; OTA never overwrites it. If the firmware you sent has no
            Bluetooth update of its own, come back here and press <b>Boot the base again</b> (writes only the 8 KB OTA
            state, nothing else). The base has no rollback and no pairing: anyone in range can send it an image.
          </Note>
        </div>
        <div className="space-y-4">
          <InstallPanel board={base} pack="base" label="OTA base · any ESP32-S3, 8 MB" />
          <InstallPanel board={base} pack="base" label="Board running something else" recover />
        </div>
      </div>
    </section>
  )
}

function BleSection() {
  return (
    <section className="mt-16">
      <SectionTitle icon={Bluetooth} title="Update over Bluetooth" sub="Connect to a pet and send firmware, a character pack or a .wasm app. Chrome or Edge on desktop, Chrome on Android." />
      <div className="grid gap-3 sm:grid-cols-3">
        <LinkCard href="ble/" icon={Bluetooth} title="Bluetooth tool" body="OTA firmware, characters, apps, speaker and mic test" />
        <LinkCard href="firmware/" icon={Download} title="Firmware files" body="App images for OTA, full images for esptool, SHA256SUMS" />
        <LinkCard href="usb/" icon={Usb} title="Classic USB flasher" body="The ESP Web Tools page, with a live character preview" />
      </div>
      <Note tone="info">
        <ShieldAlert className="mr-1.5 inline size-4 align-[-3px]" />
        The pet's update service has no pairing. It checks size, CRC and board name, and rolls back an image that fails
        to start, but anyone in Bluetooth range can send one while it advertises.
      </Note>
    </section>
  )
}

function Footer({ cat }: { cat: Catalog | null }) {
  return (
    <footer className="mt-20 flex flex-wrap items-center justify-between gap-3 border-t border-ink-800 pt-6 text-sm text-ink-400">
      <span>MIT · third-party art keeps its own license ({cat?.packs.length ?? '…'} characters, see each pack's PROVENANCE.md)</span>
      <span className="font-mono text-xs">{cat ? `firmware ${cat.commit.slice(0, 7)} · ${cat.built}` : ''}</span>
    </footer>
  )
}

function SectionTitle({ icon: Icon, title, sub }: { icon: typeof Usb; title: string; sub: string }) {
  return (
    <div className="mb-6">
      <h2 className="flex items-center gap-2 text-2xl font-semibold tracking-tight"><Icon className="size-5 text-amber" />{title}</h2>
      <p className="mt-1.5 max-w-prose text-ink-400">{sub}</p>
    </div>
  )
}

const Label = ({ children }: { children: React.ReactNode }) => <p className="mb-2 text-sm font-medium">{children}</p>

function Choice({ active, onClick, children }: { active: boolean; onClick: () => void; children: React.ReactNode }) {
  return (
    <button
      onClick={onClick}
      aria-pressed={active}
      className={`flex flex-col items-start gap-0.5 rounded-xl border px-4 py-3 text-left transition focus-visible:outline-2 focus-visible:outline-amber ${active ? 'border-amber bg-ink-800' : 'border-ink-800 bg-ink-900 hover:border-ink-700'}`}
    >
      {children}
    </button>
  )
}

function LinkCard({ href, icon: Icon, title, body }: { href: string; icon: typeof Usb; title: string; body: string }) {
  return (
    <a href={href} className="group flex items-start gap-3 rounded-xl border border-ink-800 bg-ink-900 p-4 hover:border-amber">
      <Icon className="mt-0.5 size-5 text-amber" />
      <span className="flex-1">
        <span className="flex items-center justify-between font-medium">{title}<ChevronRight className="size-4 text-ink-400 group-hover:text-amber" /></span>
        <span className="mt-0.5 block text-sm text-ink-400">{body}</span>
      </span>
    </a>
  )
}

function Note({ tone, children }: { tone: 'warn' | 'bad' | 'info'; children: React.ReactNode }) {
  const c = tone === 'bad' ? 'border-red-900/60 bg-red-950/40 text-red-200' : tone === 'warn' ? 'border-amber-dim/50 bg-amber/5 text-ink-200' : 'border-ink-800 bg-ink-900 text-ink-400'
  return (
    <div className={`mt-4 flex gap-2 rounded-lg border p-3 text-sm ${c}`}>
      {tone !== 'info' && <TriangleAlert className="mt-0.5 size-4 shrink-0" />}
      <div>{children}</div>
    </div>
  )
}
