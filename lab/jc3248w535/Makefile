YAML    ?= jc3248w535.yaml
ESPHOME  = uvx --from esphome esphome

.PHONY: help compile run upload logs clean wipe rand secrets sim sim-v1 sim-v2 sim-build sim-clean sim-fonts sim-web sim-web-serve sim-web-clean

help:
	@echo "Firmware (ESPHome via uvx):"
	@echo "  compile   - uvx esphome compile $(YAML)"
	@echo "  run       - uvx esphome run $(YAML)   (compile + upload + logs)"
	@echo "  upload    - uvx esphome upload $(YAML)"
	@echo "  logs      - uvx esphome logs $(YAML)"
	@echo "  rand      - print a fresh API encryption key"
	@echo "  secrets   - copy secrets.yaml.example -> secrets.yaml (if missing)"
	@echo "  clean     - remove .esphome build cache"
	@echo "  wipe      - clean + remove generated binaries"
	@echo ""
	@echo "Simulator (LVGL + SDL2 on host):"
	@echo "  sim            - alias for sim-v2 (the brewing dashboard)"
	@echo "  sim-v1         - build + run the original heartbeat layout"
	@echo "  sim-v2         - build + run the 10-card brewing dashboard"
	@echo "  sim-build      - configure + build both v1 + v2"
	@echo "  sim-clean      - rm sim/build"
	@echo ""
	@echo "Simulator → WASM (same C, browser-hosted):"
	@echo "  sim-web        - compile sim v2 to WebAssembly via Emscripten"
	@echo "  sim-web-serve  - sim-web + start http://localhost:8000 server"
	@echo "  sim-web-clean  - rm sim/build-web"

compile: secrets
	$(ESPHOME) compile $(YAML)

run: secrets
	$(ESPHOME) run $(YAML)

upload:
	$(ESPHOME) upload $(YAML)

logs:
	$(ESPHOME) logs $(YAML)

rand:
	@echo "api_encryption_key: \"$$(python3 -c 'import secrets,base64; print(base64.b64encode(secrets.token_bytes(32)).decode())')\""
	@echo "ota_password:       \"$$(python3 -c 'import secrets; print(secrets.token_urlsafe(24))')\""
	@echo "ap_fallback_password: \"$$(python3 -c 'import secrets; print(secrets.token_urlsafe(12))')\""

secrets:
	@test -f secrets.yaml || cp secrets.yaml.example secrets.yaml

clean:
	rm -rf .esphome

wipe: clean
	rm -rf build/ .pioenvs/ .pio/

# ── LVGL + SDL2 desktop simulator ──────────────────────────────────
sim-build:
	cmake -S sim -B sim/build -G Ninja 2>/dev/null || cmake -S sim -B sim/build
	cmake --build sim/build

sim: sim-v2

sim-v1: sim-build
	./sim/build/oracle_face_sim_v1

sim-v2: sim-build
	./sim/build/oracle_face_sim_v2

sim-clean:
	rm -rf sim/build

# ── WASM build (browser-hosted sim) ────────────────────────────────
# emcmake injects the Emscripten toolchain; output lands in sim/build-web/
# as oracle_face_sim_v{1,2}.{html,js,wasm}.
sim-web:
	emcmake cmake -S sim -B sim/build-web -DCMAKE_BUILD_TYPE=Release
	cmake --build sim/build-web

sim-web-serve: sim-web
	@echo "open http://localhost:8000/oracle_face_sim_v2.html"
	cd sim/build-web && python3 -m http.server 8000

sim-web-clean:
	rm -rf sim/build-web

# Regenerate LVGL C font tables from fonts/NotoSans-Regular.ttf.
# Range: ASCII printable (0x20–0x7F) + U+03C8 (ψ). 4 bpp, compressed.
sim-fonts:
	@for SZ in 14 22 28; do \
	  npx --yes lv_font_conv@latest \
	    --size $$SZ --bpp 4 --format lvgl \
	    --font sim/fonts/NotoSans-Regular.ttf \
	    -r 0x20-0x7F -r 0x3C8 \
	    --lv-font-name noto_$$SZ \
	    -o sim/fonts_gen/noto_$$SZ.c && \
	  echo "✓ sim/fonts_gen/noto_$$SZ.c"; \
	done
