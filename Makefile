# ════════════════════════════════════════════════════════════════════════
#  esp32-oracle — firmware CLI
#
#  Auto-discovers every ESPHome firmware config under lab/ (any *.yaml with a
#  top-level `esphome:` block), lists them, detects the connected board, and
#  flashes the one you pick. No hardcoded app list — drop a new yaml in and it
#  shows up.
#
#  Quick start:
#     make list                 # show all flashable apps + the desktop apps
#     make ports                # detect serial ports
#     make chipid               # identify the connected ESP32 (read-only)
#     make flash APP=beacon-display          # compile + upload + logs
#     make upload APP=beacon-display         # upload only (no log tail)
#     make logs  APP=beacon-display          # serial logs
#     make clean APP=beacon-display          # esphome clean (free build cache)
#
#  PORT is auto-detected (first /dev/cu.usb*). Override: make flash APP=x PORT=/dev/cu.usbmodemXXXX
# ════════════════════════════════════════════════════════════════════════

ESPHOME ?= uvx esphome

# Auto-discover ESPHome firmware: yaml files with a top-level `esphome:` block.
FIRMWARE := $(sort $(shell grep -rlE '^esphome:' lab --include='*.yaml' \
  --exclude-dir=.esphome --exclude-dir=.pio --exclude-dir=target --exclude-dir=build 2>/dev/null))

# Resolve APP (a yaml basename, e.g. "beacon-display") → its yaml path.
APP_YAML := $(firstword $(foreach f,$(FIRMWARE),$(if $(filter $(APP),$(basename $(notdir $(f)))),$(f))))

# Auto-detect the serial port (native-USB S3/C3 first, then UART bridges).
PORT ?= $(firstword $(wildcard /dev/cu.usbmodem*) $(wildcard /dev/cu.usbserial*) $(wildcard /dev/cu.wchusbserial*))

# esp-idf's esptool (for read-only chip id) if present.
ESPTOOL_PY := $(firstword $(wildcard $(HOME)/.espressif/python_env/*/bin/esptool.py))
ESPTOOL_PYBIN := $(firstword $(wildcard $(HOME)/.espressif/python_env/*/bin/python))

.PHONY: help list ports boards chipid flash flashx upload logs clean _need_app _need_port

help:
	@echo "esp32-oracle firmware CLI — see 'make list'. Targets:"
	@echo "  list  ports  boards  chipid"
	@echo "  flash/upload/logs/clean APP=<key> [PORT=/dev/cu.x]"
	@echo "  flashx PAIRS=\"app:/dev/cu.x app2:/dev/cu.y\"   # flash multiple boards in PARALLEL"

## list — every flashable firmware app + the desktop apps
list:
	@echo ""
	@echo "  📡 FLASHABLE FIRMWARE (ESPHome)            board                 node name"
	@echo "  ─────────────────────────────────────────────────────────────────────────"
	@for y in $(FIRMWARE); do \
	  key=$$(basename $$y .yaml); \
	  name=$$(grep -m1 -E '^[[:space:]]+name:' $$y | sed 's/.*name: *//;s/["'\'']//g'); \
	  board=$$(grep -m1 -E '^[[:space:]]+board:' $$y | sed 's/.*board: *//'); \
	  printf "  %-22s %-21s %s\n" "$$key" "$$board" "$$name"; \
	  printf "      ↳ %s\n" "$$y"; \
	done
	@echo ""
	@echo "  🖥  DESKTOP APPS (not flashed — run locally)"
	@echo "  ─────────────────────────────────────────────────────────────────────────"
	@echo "  oracle-app-tauri       macOS BLE broadcaster + tray + dashboard   (cd lab/oracle-app-tauri && make dev)"
	@echo "  beacon/cli-rs          Rust BLE broadcaster CLI                    (cd lab/beacon/cli-rs && cargo run)"
	@echo "  buddy-port             claude-desktop-buddy port (WIP, not building yet)"
	@echo ""
	@echo "  Flash:  make flash APP=<key>   (port auto-detected: $(if $(PORT),$(PORT),NONE FOUND))"
	@echo ""

## ports — list candidate serial ports
ports:
	@echo "Serial ports (excluding Bluetooth/debug):"
	@ls /dev/cu.* 2>/dev/null | grep -viE 'Bluetooth|debug-console' | sed 's/^/  /' || echo "  (none)"
	@echo "Auto-selected PORT = $(if $(PORT),$(PORT),<none>)"

## chipid — identify the connected ESP32 (read-only, no write)
chipid: _need_port
	@if [ -z "$(ESPTOOL_PY)" ]; then echo "esp-idf esptool not found; try: . ~/esp/esp-idf/export.sh"; exit 1; fi
	@echo "Reading chip on $(PORT) (read-only)…"
	@"$(ESPTOOL_PYBIN)" "$(ESPTOOL_PY)" --port "$(PORT)" --before default_reset --after hard_reset chip_id 2>&1 \
	  | grep -iE 'esptool v|chip is|features|crystal|MAC' || true

## boards — identify EVERY connected board: port + chip + PSRAM + MAC (read-only)
##   tells the JC3248W535 (8MB) from the WT32-SC01 Plus (2MB) and their ports.
boards:
	@if [ -z "$(ESPTOOL_PY)" ]; then echo "esp-idf esptool not found; try: . ~/esp/esp-idf/export.sh"; exit 1; fi
	@ports="$$(ls /dev/cu.usbmodem* /dev/cu.usbserial* /dev/cu.wchusbserial* 2>/dev/null)"; \
	if [ -z "$$ports" ]; then echo "  (no boards connected)"; exit 0; fi; \
	printf "  %-26s %-26s %-10s %-7s %s\n" PORT CHIP PSRAM FLASH MAC; \
	printf "  %-26s %-26s %-10s %-7s %s\n" "────" "────" "─────" "─────" "───"; \
	for p in $$ports; do \
	  info=$$("$(ESPTOOL_PYBIN)" "$(ESPTOOL_PY)" --port "$$p" flash_id 2>/dev/null); \
	  chip=$$(echo "$$info" | grep -i 'Chip type' | sed 's/.*Chip type: *//;s/ *$$//'); \
	  psram=$$(echo "$$info" | grep -ioE 'PSRAM [0-9]+MB' | head -1); \
	  mac=$$(echo "$$info"  | grep -i 'MAC:' | head -1 | sed 's/.*MAC: *//;s/ *$$//'); \
	  flash=$$(echo "$$info"| grep -i 'Detected flash' | sed 's/.*: *//;s/ *$$//'); \
	  printf "  %-26s %-26s %-10s %-7s %s\n" "$$p" "$${chip:-?}" "$${psram:-?}" "$${flash:-?}" "$${mac:-?}"; \
	done

## flashx — flash multiple apps to multiple ports IN PARALLEL
##   make flashx PAIRS="heartbeat:/dev/cu.usbmodem13301 wt32-sc01-plus:/dev/cu.usbmodem1201"
##   per-app output goes to /tmp/flashx-<app>.log (parallel streams would interleave).
flashx:
	@if [ -z "$(PAIRS)" ]; then echo '✗ set PAIRS="app:/dev/cu.x app2:/dev/cu.y" (keys from `make list`)'; exit 1; fi
	@for pair in $(PAIRS); do \
	  app=$${pair%%:*}; port=$${pair#*:}; \
	  yaml=$$(for f in $(FIRMWARE); do [ "$$(basename $$f .yaml)" = "$$app" ] && echo $$f; done | head -1); \
	  if [ -z "$$yaml" ]; then echo "✗ no app '$$app' — skipping"; continue; fi; \
	  echo "▶ $$app → $$port  (log: /tmp/flashx-$$app.log)"; \
	  ( $(ESPHOME) run "$$yaml" --device "$$port" >/tmp/flashx-$$app.log 2>&1 \
	      && echo "✓ $$app done" || echo "✗ $$app FAILED — tail /tmp/flashx-$$app.log" ) & \
	done; \
	wait; \
	echo "── all parallel flashes finished ──"

## flash — compile + upload + tail logs (the full cycle)
flash: _need_app _need_port
	@echo "▶ flashing '$(APP)' → $(APP_YAML) on $(PORT)"
	$(ESPHOME) run $(APP_YAML) --device $(PORT)

## upload — compile + upload only (no log tail)
upload: _need_app _need_port
	$(ESPHOME) upload $(APP_YAML) --device $(PORT)

## logs — serial logs for an app
logs: _need_app _need_port
	$(ESPHOME) logs $(APP_YAML) --device $(PORT)

## clean — esphome clean (frees the per-app .esphome build cache)
clean: _need_app
	$(ESPHOME) clean $(APP_YAML)

# ── guards ───────────────────────────────────────────────────────────────
_need_app:
	@if [ -z "$(APP)" ]; then echo "✗ set APP=<key> — run 'make list' to see keys"; exit 1; fi
	@if [ -z "$(APP_YAML)" ]; then echo "✗ no firmware named '$(APP)'. Run 'make list'."; exit 1; fi

_need_port:
	@if [ -z "$(PORT)" ]; then echo "✗ no serial port found. Plug in the board, or pass PORT=/dev/cu.xxx (see 'make ports')"; exit 1; fi
