CARGO ?= cargo
WASM_TARGET = wasm32-wasip1

CLI_BIN  = target/release/oracle-beacon
TRAY_BIN = target/release/oracle-beacon-tray
WASM_BIN = target/$(WASM_TARGET)/release/oracle-beacon.wasm

.PHONY: help check test build tray wasm run run-loop run-tray wasm-run clean install kill

help:
	@echo "Rust port of the BLE broadcaster — native CoreBluetooth, no Python."
	@echo ""
	@echo "  check        cargo check (cheap pre-build validation)"
	@echo "  test         run unit tests + doctests (8 tests)"
	@echo "  build        CLI binary at $(CLI_BIN)"
	@echo "  tray         menu-bar app at $(TRAY_BIN) (requires --features tray)"
	@echo "  wasm         WASI binary at $(WASM_BIN)"
	@echo "  run          single broadcast via CLI"
	@echo "  run-loop     CLI in loop mode (30s refresh)"
	@echo "  run-tray     launch menu-bar app (foreground)"
	@echo "  wasm-run     WASI dry-run via wasmtime"
	@echo "  install      copy both binaries to ~/.local/bin/"
	@echo "  kill         pkill any running oracle-beacon process"

check:
	$(CARGO) check --all-features

test:
	$(CARGO) test --all-features

build:
	$(CARGO) build --release
	@ls -lh $(CLI_BIN) | awk '{print "  built:", $$5, $$NF}'

tray:
	$(CARGO) build --release --features tray
	@ls -lh $(TRAY_BIN) | awk '{print "  built:", $$5, $$NF}'

wasm:
	$(CARGO) build --release --target $(WASM_TARGET)
	@ls -lh $(WASM_BIN) | awk '{print "  built:", $$5, $$NF}'

run: build
	$(CLI_BIN)

run-loop: build
	$(CLI_BIN) --loop --loop-every 30

run-tray: tray
	$(TRAY_BIN)

wasm-run: wasm
	wasmtime --dir=/tmp::/tmp $(WASM_BIN) --dry-run --json /tmp/claude-statusline/raw.json

clean:
	$(CARGO) clean

install: build tray
	mkdir -p $$HOME/.local/bin
	cp $(CLI_BIN) $$HOME/.local/bin/oracle-beacon
	cp $(TRAY_BIN) $$HOME/.local/bin/oracle-beacon-tray
	@echo "installed:"
	@echo "  $$HOME/.local/bin/oracle-beacon"
	@echo "  $$HOME/.local/bin/oracle-beacon-tray"

kill:
	-pkill -f "oracle-beacon" 2>/dev/null || true
	@echo "killed any running oracle-beacon processes"
