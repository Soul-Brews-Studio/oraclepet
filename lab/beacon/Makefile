YAML    ?= beacon-display.yaml
ESPHOME  = uvx --from esphome esphome

.PHONY: help compile run upload logs clean broadcast

help:
	@echo "Firmware:"
	@echo "  compile             - uvx esphome compile $(YAML)"
	@echo "  run                 - uvx esphome run $(YAML)  (compile + upload + logs)"
	@echo "  upload              - uvx esphome upload $(YAML)"
	@echo "  logs                - uvx esphome logs $(YAML)"
	@echo "  clean               - rm .esphome cache"
	@echo ""
	@echo "BLE broadcaster:"
	@echo "  broadcast TEXT='hi' - advertise via ble-beacon.py"

compile:
	$(ESPHOME) compile $(YAML)

run:
	$(ESPHOME) run $(YAML)

upload:
	$(ESPHOME) upload $(YAML)

logs:
	$(ESPHOME) logs $(YAML)

clean:
	mv .esphome /tmp/beacon-esphome-$$(date +%s) 2>/dev/null || true

# ── Advertise a BLE beacon. Usage: make broadcast TEXT='hello catlab' ────
broadcast:
	@uv run ble-beacon.py "$(TEXT)"
