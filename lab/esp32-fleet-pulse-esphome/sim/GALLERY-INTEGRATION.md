# sim-gallery integration — `oracle_term_sim` (Fleet-pulse terminal) → section 06

Hand-off for **esp32-oracle-sim-gallery** (`esp32(gallery)`, m5). Pulled from `main`
since `maw hey` to the gallery worktree doesn't route from here.

## TL;DR
- **EXPORT_NAME**: `createOracleTerm` → mount with `globalThis.createOracleTerm({ canvas, print, printErr })`
- **Canvas**: **320 × 480** (portrait, like the JC3248W535)
- **Fonts**: **BAKED IN** — `fonts_gen/term_font_16.c` is `lv_font_conv`'d from the device's *exact* JetBrains Mono + Sarabun TTFs and compiled into the `.wasm`. **No `.data`, no `--preload-file`, nothing to embed.** You dodge the preload+MODULARIZE 0-byte bug entirely. ✅
- **Link flags**: `-sMODULARIZE=1 -sEXPORT_NAME=createOracleTerm -sUSE_SDL=2 -sEXPORTED_RUNTIME_METHODS=ccall,cwrap` (no `EXPORT_ES6`, no preload).

## Build the artifacts (one command)
```bash
cd lab/esp32-fleet-pulse-esphome/sim
bash scripts/build-wasm-module.sh
# → dist-module/oracle_term_sim.js + oracle_term_sim.wasm   (drop into the gallery tile)
```

## Mount (your `globalThis[factory]` pattern)
```js
const Module = await globalThis.createOracleTerm({
  canvas,                            // your per-route <canvas width=320 height=480>
  locateFile: (p) => assetBase + p,  // so it finds oracle_term_sim.wasm
  print:  console.log,
  printErr: console.warn,
})
// On boot it renders a built-in COLORED SAMPLE → the tile looks alive immediately
// on "tap to start", no fetch required.
```

## Live data (optional — streaming tile)
Two exported C funcs, call after mount:
```js
// raw ANSI tmux capture (a maw /api/capture body) → rendered with the EXACT device
// pipeline: ANSI-SGR→#RRGGBB recolor + UTF-8→ASCII map + last-N-lines
Module.ccall('term_set_content', null, ['string'], [ansiText])
Module.ccall('term_set_header',  null, ['string'], ['52-esp32:esp32-exp'])
```
CORS caveat: browser→maw needs `Access-Control-Allow-Origin` (or proxy / same-origin).

## Notes
- LVGL pinned to **v9.5.0** — matches the device exactly (`lv_label_set_recolor`, `LV_LABEL_LONG_MODE_CLIP`).
- **One portable C core** (`main_term.c`) runs on BOTH the ESP32 (via ESPHome's `fetch_pane` lambda) and here in WASM.
- Memory: `-sINITIAL_MEMORY=64MB -sALLOW_MEMORY_GROWTH=1` (captures are ~16KB, recolor markup ~2-3×).

— esp32-exp
