// Bridge to the Emscripten factory (oracle_term_sim.js → globalThis.createOracleTerm,
// loaded via <script> in index.html — same artifact + mount as the sim-gallery).
// Exposes the device's exported C funcs (term_set_content / term_set_header).

export interface TermModule {
  ccall: (name: string, ret: string | null, argTypes: string[], args: unknown[]) => unknown
}

type Factory = (arg?: Record<string, unknown>) => Promise<TermModule>

declare global {
  interface Window { createOracleTerm?: Factory }
}

let modPromise: Promise<TermModule> | null = null

function waitForFactory(timeoutMs = 12000): Promise<Factory> {
  return new Promise((resolve, reject) => {
    const t0 = Date.now()
    const tick = () => {
      if (window.createOracleTerm) return resolve(window.createOracleTerm)
      if (Date.now() - t0 > timeoutMs) return reject(new Error('createOracleTerm not loaded (script /oracle_term_sim.js)'))
      setTimeout(tick, 30)
    }
    tick()
  })
}

/** Load the WASM once, bound to the given canvas. Idempotent. */
export function loadTerm(canvas: HTMLCanvasElement): Promise<TermModule> {
  if (modPromise) return modPromise
  modPromise = (async () => {
    const factory = await waitForFactory()
    return factory({
      canvas,
      locateFile: (p: string) => '/' + p, // resolve the .wasm next to the .js
      print: (s: string) => console.log(s),
      printErr: (s: string) => console.warn(s),
    })
  })()
  return modPromise
}

export function setContent(mod: TermModule, text: string): void {
  mod.ccall('term_set_content', null, ['string'], [text])
}

export function setHeader(mod: TermModule, text: string): void {
  try { mod.ccall('term_set_header', null, ['string'], [text]) } catch { /* optional export */ }
}
