#!/usr/bin/env bash
# Build the LVGL terminal as a STANDALONE 3-file bundle:
#   oracle_term_sim.html + oracle_term_sim.js + oracle_term_sim.wasm
# This is the LIVE viewer (built-in maw-URL input + 3s /api/capture poll via
# web/shell.html) that the sim-gallery vendors as section 07 (/embeds/term-live/).
#
# WHY THIS SCRIPT EXISTS: build-wasm/ is gitignored + transient (dies on worktree
# cleanup), so the gallery must NOT depend on it. This script + the committed
# source (main_term.c, CMakeLists.txt, fonts_gen/term_font_16.c, web/shell.html)
# ARE the source-of-truth. Point the gallery's Makefile rule at this script.
#
# Fonts are baked into the .wasm (fonts_gen .c) — no .data / preload.
# Sibling: scripts/build-wasm-module.sh builds the MODULARIZE factory (section 06).
set -euo pipefail
cd "$(dirname "$0")/.."          # → sim/
command -v emcmake >/dev/null || { echo "❌ emscripten not on PATH — run: source <emsdk>/emsdk_env.sh"; exit 1; }

emcmake cmake -B build-wasm -DCMAKE_BUILD_TYPE=Release   # SIM_MODULE unset → standalone .html shell
cmake --build build-wasm -j4

mkdir -p dist-standalone
for f in html js wasm; do
  cp "build-wasm/oracle_term_sim.$f" "dist-standalone/oracle_term_sim.$f"
done
echo "✅ standalone live viewer → dist-standalone/ (oracle_term_sim.{html,js,wasm})"
echo "   gallery: vendor these 3 into public/embeds/term-live/"
echo "   (rename oracle_term_sim.html → index.html if the iframe loads the dir root)"
