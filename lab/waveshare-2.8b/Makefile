YAML    ?= waveshare-2.8b.yaml
ESPHOME  = uvx --from esphome esphome

.PHONY: help compile run upload logs clean wipe rand secrets

help:
	@echo "Waveshare ESP32-S3-LCD-2.8B (480x640, ST7701S RGB)"
	@echo ""
	@echo "  compile   - uvx esphome compile $(YAML)"
	@echo "  run       - uvx esphome run $(YAML)   (compile + upload + logs)"
	@echo "  upload    - uvx esphome upload $(YAML)"
	@echo "  logs      - uvx esphome logs $(YAML)"
	@echo "  rand      - print a fresh API encryption key"
	@echo "  secrets   - copy secrets.yaml.example -> secrets.yaml (if missing)"
	@echo "  clean     - remove .esphome build cache"
	@echo "  wipe      - clean + remove generated binaries"

compile: secrets
	$(ESPHOME) compile $(YAML)

run: secrets
	$(ESPHOME) run $(YAML)

upload:
	$(ESPHOME) upload $(YAML)

logs:
	$(ESPHOME) logs $(YAML)

rand:
	@echo "api_key: \"$$(python3 -c 'import secrets,base64; print(base64.b64encode(secrets.token_bytes(32)).decode())')\""

secrets:
	@test -f secrets.yaml || cp secrets.yaml.example secrets.yaml

clean:
	rm -rf .esphome

wipe: clean
	rm -rf build/ .pioenvs/ .pio/
