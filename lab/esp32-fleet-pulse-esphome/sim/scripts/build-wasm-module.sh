#!/usr/bin/env bash
# Build the LVGL terminal as a globalThis.createOracleTerm factory module
# (oracle_term_sim.js + .wasm). Drops it into the React app's public/ AND a
# clean dist-module/ for the sim-gallery hand-off. Fonts are baked into the
# .wasm (fonts_gen .c) — no .data / preload.
set -euo pipefail
cd "$(dirname "$0")/.."          # → sim/
emcmake cmake -B build-mod -DSIM_MODULE=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-mod -j4
mkdir -p app/public dist-module
cp build-mod/oracle_term_sim.js   app/public/oracle_term_sim.js
cp build-mod/oracle_term_sim.wasm app/public/oracle_term_sim.wasm
cp build-mod/oracle_term_sim.js   dist-module/oracle_term_sim.js
cp build-mod/oracle_term_sim.wasm dist-module/oracle_term_sim.wasm
echo "✅ globalThis.createOracleTerm → app/public/ + dist-module/ (oracle_term_sim.js + .wasm)"
