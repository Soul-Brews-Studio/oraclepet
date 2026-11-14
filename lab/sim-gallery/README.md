# sim-gallery

> Every body the esp32-oracle has, runnable in one browser tab.
> Many bodies, one soul.

A Vite + React + Tailwind app that hosts every simulator in this repo behind
one URL. LVGL+SDL→WASM sims get canvas-mounted into per-route React components;
zero-build HTML sims (like `pet-sim.html`) embed via iframe.

## What's in the gallery

| Slug | Source | Kind | Status |
|---|---|---|---|
| `pet-react` | `lab/jc3248-pet/sim/pet-sim.html` | React/CDN (iframe) | ✅ ready |
| `gif-wasm` | `lab/gif-wasm/web/` | emcc browser demo (iframe) | ✅ ready |
| `pet-lvgl` | `lab/jc3248-pet/sim/` (emcc) | LVGL+SDL→WASM (canvas mount) | needs `build-sims` |
| `oracle-face-v1` | `lab/jc3248w535/sim/` (emcc, v1) | LVGL+SDL→WASM | needs `build-sims` |
| `oracle-face-v2` | `lab/jc3248w535/sim/` (emcc, v2) | LVGL+SDL→WASM | needs `build-sims` |

## Run it

```bash
cd lab/sim-gallery
make dev           # install deps + sync HTML embeds + vite dev server
# → http://localhost:5173
```

To also wire up the LVGL sims:

```bash
make build-sims    # emcc-builds the LVGL+SDL sims to public/sims/<slug>/
```

This requires `emcmake` on PATH (`brew install emscripten`) and a one-time
patch to each sim's `CMakeLists.txt` adding the link flags
`-sMODULARIZE=1 -sEXPORT_NAME=<Factory>` so the gallery can call them as
factories from React. (Without `MODULARIZE` each sim would clobber the global
`Module` of the next one.)

## How embeds work

- **iframe** sims live under `public/embeds/<name>/`. The Makefile copies them
  from their source dirs at build time.
- **emscripten** sims live under `public/sims/<slug>/<factory>.js` + `.wasm`.
  `EmscriptenSim.tsx` injects the `.js` as a `<script>`, calls
  `globalThis[factory]({ canvas, ... })`, and tears the runtime down on
  unmount so route changes don't leak `Module` globals.

## Files

```
sim-gallery/
├── Makefile             # build-sims, embeds, dev convenience
├── package.json
├── vite.config.ts
├── index.html           # vite entry
├── src/
│   ├── main.tsx         # react root + router
│   ├── App.tsx          # routes
│   ├── tiles.ts         # ★ the manifest of every sim
│   ├── components/
│   │   ├── Layout.tsx           # nav + breadcrumb
│   │   ├── SimCard.tsx          # gallery tile
│   │   └── EmscriptenSim.tsx    # canvas mount for emcc factories
│   └── routes/
│       ├── Gallery.tsx          # `/` — grid of tiles
│       └── SimRoute.tsx         # `/sim/:slug` — single-sim view
└── public/
    ├── embeds/                  # iframe HTML sims (copied by `make embeds`)
    └── sims/                    # emcc factories (built by `make build-sims`)
```

Add a sim: append an entry to `src/tiles.ts`, drop its files in `public/`, done.
