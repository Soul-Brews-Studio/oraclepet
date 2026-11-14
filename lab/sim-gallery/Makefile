# sim-gallery — host every esp32-oracle simulator under one React app.
#
# `make` prints help. `make embeds` syncs the static HTML sims into public/.
# `make build-sims` emcc-builds the LVGL→WASM sims and copies them into public/.
# `make dev` runs both prereqs then `pnpm dev`.

REPO_ROOT := $(realpath ..)
LAB        := $(REPO_ROOT)
PUBLIC     := public
EMBEDS     := $(PUBLIC)/embeds
SIMS       := $(PUBLIC)/sims

EMCMAKE   := $(shell command -v emcmake 2>/dev/null)

.PHONY: help install embeds flash build-sims build-pet build-oracle clean dev

help:
	@echo "sim-gallery targets:"
	@echo "  install     pnpm install (deps for the React app)"
	@echo "  embeds      copy zero-build HTML sims (pet-sim, gif-wasm, term-live) into public/"
	@echo "  flash       stage the §08 web flasher (built bins + esp-web-tools) into public/flash/"
	@echo "  build-sims  emcc-build the LVGL sims (pet, oracle-face v1/v2) into public/sims/"
	@echo "  dev         install + embeds + vite dev server"
	@echo "  clean       remove public/embeds/, public/sims/, dist/"
	@echo ""
	@echo "Status: emcmake = $(if $(EMCMAKE),$(EMCMAKE),NOT FOUND — install emscripten)"

install:
	@pnpm install

embeds: $(EMBEDS)/pet-sim.html $(EMBEDS)/gif-wasm/index.html $(EMBEDS)/term-live/oracle_term_sim.html

# §08 — the esp-web-tools web flasher (/flash/). Hosts the built firmware bins +
# the install page + the live-preview decoder. The bins need ESP-IDF to build,
# so they're staged out-of-band (NOT part of `embeds` — keeps the common path
# fast/toolchain-free). Whoever has the IDF env runs:
#   (cd $(LAB)/jc3248-pet-idf && tools/build-packs.sh && cd webflasher && ./stage.sh)
# then `make flash` mirrors that staged output into public/flash/.
WEBFLASHER := $(LAB)/jc3248-pet-idf/webflasher
flash: $(PUBLIC)/flash/index.html
# Depend on the bins + manifests too, not just index.html — a firmware rebuild
# (same index.html) must still re-stage, or the deploy silently ships stale bins.
$(PUBLIC)/flash/index.html: $(WEBFLASHER)/index.html $(wildcard $(WEBFLASHER)/*.bin) $(wildcard $(WEBFLASHER)/manifest-*.json)
	@test -f $(WEBFLASHER)/manifest-bufo.json || { echo "no staged flasher — run: (cd $(LAB)/jc3248-pet-idf && tools/build-packs.sh) then (cd $(WEBFLASHER) && ./stage.sh)"; exit 1; }
	@mkdir -p $(PUBLIC)/flash/preview
	@cp -f $(WEBFLASHER)/index.html $(WEBFLASHER)/manifest-*.json $(WEBFLASHER)/*.bin $(PUBLIC)/flash/
	@cp -R $(WEBFLASHER)/preview/. $(PUBLIC)/flash/preview/
	@echo "flash: /flash/ staged (packs + esp-web-tools + preview)"

# esp32-exp's STANDALONE LIVE terminal viewer — the non-MODULARIZE .html build
# (its own cyan UI + maw-URL/pane inputs + 3s /api/capture poll). Distinct from
# section 06's baked MODULARIZE snapshot.
# Live stream works on-LAN only; the public https deploy falls back to the
# built-in sample (mixed-content + RFC1918 LAN IP). Do NOT proxy maw publicly
# (/api/capture is unauthed — it streams live pane contents = fleet topology).
#
# DURABILITY (agreed with esp32-exp 2026-05-31): the COMMITTED source is
# $(LAB)/esp32-fleet-pulse-esphome/sim/ (main_term.c, CMakeLists.txt,
# fonts_gen/term_font_16.c, web/shell.html). The transient build-wasm/ output
# below lives under agents/ (gitignored, dies on worktree cleanup) — so this
# rule copies a VENDORED SNAPSHOT as the interim fallback. When esp32-exp lands
# scripts/build-wasm-standalone.sh (SIM_MODULE=OFF), repoint this rule to
# emcmake-build from the committed sim/ source so the embed reproduces on a
# fresh checkout (emcmake is available). Do NOT symlink into the agents/ tree.
TERM_LIVE_SRC := $(LAB)/../agents/1-exp/lab/esp32-fleet-pulse-esphome/sim/build-wasm

$(EMBEDS)/pet-sim.html: $(LAB)/jc3248-pet/sim/pet-sim.html
	@mkdir -p $(EMBEDS)
	@cp -f $< $@
	@# The original loads ../data/characters/bufo (relative to the file).
	@# We need it relative to /embeds/, so symlink the data alongside.
	@mkdir -p $(EMBEDS)/data
	@rm -f $(EMBEDS)/data/characters
	@ln -s $(LAB)/jc3248-pet/data/characters $(EMBEDS)/data/characters
	@echo "embeds: pet-sim.html ready (data/characters symlinked)"

$(EMBEDS)/gif-wasm/index.html: $(LAB)/gif-wasm/web/index.html $(wildcard $(LAB)/gif-wasm/web/gifs/*.gif) $(wildcard $(LAB)/gif-wasm/web/gifs/*/*.gif)
	@mkdir -p $(EMBEDS)/gif-wasm
	@cp -f $(LAB)/gif-wasm/web/*.html $(LAB)/gif-wasm/web/*.js $(LAB)/gif-wasm/web/*.wasm $(EMBEDS)/gif-wasm/
	@mkdir -p $(EMBEDS)/gif-wasm/gifs
	@# Copy the whole gifs/ tree, not just top-level *.gif — bufo is flat
	@# (gifs/*.gif) but the cat packs live in gifs/cat|cat-orange|cat-pet/.
	@cp -R $(LAB)/gif-wasm/web/gifs/. $(EMBEDS)/gif-wasm/gifs/
	@echo "embeds: gif-wasm ready (bufo + cat packs)"

$(EMBEDS)/term-live/oracle_term_sim.html: $(TERM_LIVE_SRC)/oracle_term_sim.html
	@mkdir -p $(EMBEDS)/term-live
	@cp -f $(TERM_LIVE_SRC)/oracle_term_sim.html $(TERM_LIVE_SRC)/oracle_term_sim.js $(TERM_LIVE_SRC)/oracle_term_sim.wasm $(EMBEDS)/term-live/
	@echo "embeds: term-live ready (esp32-exp live viewer)"

# The LVGL sims need MODULARIZE=1 + EXPORT_NAME=<factory> added to their
# CMakeLists.txt link flags. Until that patch lands, this target prints a
# pointer instead of building.
build-sims:
	@echo "LVGL sims need their CMakeLists patched with MODULARIZE=1 +"
	@echo "EXPORT_NAME flags before this Makefile can wire them up."
	@echo ""
	@echo "Next step: edit"
	@echo "  $(LAB)/jc3248-pet/sim/CMakeLists.txt"
	@echo "  $(LAB)/jc3248w535/sim/CMakeLists.txt"
	@echo "to add (inside the EMSCRIPTEN branch):"
	@echo "  target_link_options(<target> PRIVATE -sMODULARIZE=1 -sEXPORT_NAME=<Factory>)"
	@echo ""
	@echo "Then this Makefile will call emcmake + cp into public/sims/<slug>/."
	@false

clean:
	@rm -rf $(EMBEDS) $(SIMS) dist node_modules/.vite

dev: install embeds
	@pnpm dev
