# gif-wasm — the bufo GIF decoder in WebAssembly (WASM + WASI)

The ESP-IDF pet's animated-GIF decoder, lifted off the ESP32 and compiled to run
**two more places** from one shared C++ core:

| Target | Toolchain | Output | Runs with | Size |
|--------|-----------|--------|-----------|------|
| **WASI** | `zig c++ -target wasm32-wasi` | `dist/gifdec.wasm` | `wasmtime` (or any WASI host) | ~37 K |
| **WASM** | `emcc` (Emscripten) | `web/gifdec.{js,wasm}` | a browser canvas (or Node) | ~17 K wasm + 9 K js |

Same decoder — `src/gifcore.cpp` + the vendored [AnimatedGIF](https://github.com/bitbank2/AnimatedGIF)
(Apache-2.0) — behind a tiny C-linkage API (`gif_open / gif_play / gif_fb / …`).
*Many bodies, one soul* — now the frog decodes on silicon, in the terminal, and in the browser.

## Build

```bash
make            # both targets
make wasi       # dist/gifdec.wasm   (zig; -Wl,--strip-all keeps it ~37 K)
make web        # web/gifdec.{js,wasm} (emcc)
```
Toolchain: `zig` (wasm32-wasi C++ + bundled WASI libc), `emcc` (browser), optional
`wasm-opt`. The Makefile skips `wasm-opt` gracefully if it can't parse zig's debug
info — `--strip-all` already does the shrinking.

## Run — WASI (no browser, no JS)

Pure stdin → stdout, so no filesystem preopen is needed:

```bash
wasmtime dist/gifdec.wasm < web/gifs/busy.gif > out.ppm   # all frames as a P6 stream
wasmtime --env GIF_FRAME=0 dist/gifdec.wasm < web/gifs/idle_0.gif > frame0.ppm
make run-wasi GIF=web/gifs/celebrate.gif                  # convenience wrapper
```
Per-frame log (size, canvas, delays) goes to stderr. Verified decoding the bufo pack:
`busy` 2 frames, `idle_0` 9, `celebrate` 27 — all 96×100.

## Run — WASM (browser)

```bash
make run-web        # serves web/ at http://localhost:8011  (fetch() needs a server)
```
`web/index.html` decodes a GIF entirely in wasm, reads the RGBA canvas straight out
of wasm memory (`HEAPU8` at `gif_fb()`), and animates it on a `<canvas>` at 3×
pixelated — pick a bundled bufo or drop your own `.gif`.

Headless check (same module, no browser):
```bash
node -e 'require("./web/gifdec.js")().then(M=>{const fs=require("fs");
  const g=fs.readFileSync("web/gifs/busy.gif");const p=M._malloc(g.length);
  M.HEAPU8.set(g,p);console.log("rc",M._gif_open(p,g.length),M._gif_width()+"x"+M._gif_height());})'
```

## Layout
```
src/gifcore.{h,cpp}   shared decode core (memory GIF -> RGBA8888 canvas, composing)
src/wasi_main.cpp     WASI entry: stdin GIF -> P6 PPM on stdout
src/compat.h          millis()/delay() stubs (AnimatedGIF's Arduino path, never run)
vendor/AnimatedGIF/   the decoder (Apache-2.0), NO_SIMD portable-C path
web/index.html        browser demo (canvas + file drop)
web/gifs/             a few bufo frames to play with
Makefile              wasi · web · run-wasi · run-web · clean
```

Source GIFs come from the pet's pack (`../jc3248-pet-idf/data/characters/bufo/`).
The RGB565→RGB888 path uses host-order (little-endian) palette — no panel byte-swap
here (that quirk is the AXS15231's, not the GIF's).
