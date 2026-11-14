// maw backend client — fetch a live tmux pane capture.
// NOTE: browser→maw is cross-origin; maw must send Access-Control-Allow-Origin
// (or serve this app from the maw host / behind a proxy).

export interface Capture { content?: string }

export async function fetchCapture(base: string, target: string): Promise<string> {
  const url = base.replace(/\/+$/, '') + '/api/capture?target=' + encodeURIComponent(target)
  const r = await fetch(url)
  if (!r.ok) throw new Error('HTTP ' + r.status)
  const d = (await r.json()) as Capture
  return d.content ?? ''
}

export async function fetchPanes(base: string): Promise<string[]> {
  const r = await fetch(base.replace(/\/+$/, '') + '/api/ls')
  if (!r.ok) throw new Error('HTTP ' + r.status)
  const d = (await r.json()) as { sessions?: { name: string; windows?: { name: string }[] }[] }
  const out: string[] = []
  for (const s of d.sessions ?? [])
    for (const w of s.windows ?? [])
      if (s.name && w.name) out.push(`${s.name}:${w.name}`)
  return out
}
