// firmware/index.json, written by tools/site/build-site.sh from dist/ and the partition tables.

export interface Part {
  file: string // relative to firmware/<board>/
  offset: number
  bytes: number
}

export interface Board {
  id: string
  name: string
  screen: string
  note: string
  project: string
  packs: string[] // app images exist for these default characters
  defaultPack: string
  common: Part[] // bootloader, partition table, otadata, storage
  app: { offset: number; files: Record<string, Part> } // pack -> app image
  base?: boolean // the OTA base: takes any firmware over BLE
}

export interface Pack {
  id: string
  name: string
  image: string // packs/<id>.gif
  license: string
}

export interface Catalog {
  commit: string
  built: string
  boards: Board[]
  packs: Pack[]
}

export async function loadCatalog(): Promise<Catalog> {
  const r = await fetch('firmware/index.json', { cache: 'no-cache' })
  if (!r.ok) throw new Error(`firmware/index.json: HTTP ${r.status}`)
  return r.json()
}

export function partsFor(board: Board, pack: string): (Part & { url: string })[] {
  const app = board.app.files[pack] ?? board.app.files[board.defaultPack]
  return [...board.common, app]
    .sort((a, b) => a.offset - b.offset)
    .map((p) => ({ ...p, url: `firmware/${board.id}/${p.file}` }))
}

export const kb = (n: number) => `${Math.round(n / 1024).toLocaleString()} KB`
export const hex = (n: number) => `0x${n.toString(16)}`
