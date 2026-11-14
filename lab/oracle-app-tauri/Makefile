.PHONY: help check test test-slow build dev release clean

help:
	@echo "Tauri 2.x desktop dashboard for Claude Code status."
	@echo "Renders the same layout as the ESP32 firmware, polling the"
	@echo "shared oracle_beacon lib over a single tauri command."
	@echo ""
	@echo "  check       cargo check on the Rust backend (FAST — no link,"
	@echo "              isolated target dir so it never blocks 'make dev')"
	@echo "  test        compile-check the backend (FAST). Logic is tested in"
	@echo "              ../beacon/cli-rs — run 'make test' there for unit tests."
	@echo "  test-slow   full cargo test of the Tauri crate (SLOW ~2-3min — it"
	@echo "              statically links the whole webkit/wry/objc2 dep tree"
	@echo "              just to run a handful of unit tests)."
	@echo "  dev         launch the desktop window in dev mode"
	@echo "  release     production bundle (.app on macOS)"
	@echo "  build       same as release (alias)"
	@echo "  clean       wipe build artifacts"

# Verify commands (check/test) use a SEPARATE target dir so they never
# fight `cargo tauri dev` for target/debug/.cargo-lock. Symptom of the
# clash: "Blocking waiting for file lock on artifact directory" — the dev
# server (or a verify run) stalls until the other finishes. With distinct
# CARGO_TARGET_DIR the two never share a lock, so they run concurrently.
# (dev/build/release keep the default target/ for cache reuse + bundling.)
VERIFY_TARGET := $(CURDIR)/src-tauri/target/verify

# Default 'test' is a fast compile-check, NOT a full test-binary link.
# Why: `cargo test` on a Tauri crate links ~250 crates (webkit, wry,
# objc2, …) into one test binary — a 2-3 minute link step — just to run
# a few glue-layer unit tests. The actual logic (version parsing, gates,
# payload encoding) lives in oracle_beacon and tests there in <1s.
# Use `make test-slow` when you specifically want the Tauri unit tests.
check test:
	cd src-tauri && CARGO_TARGET_DIR=$(VERIFY_TARGET) cargo check

test-slow:
	cd src-tauri && CARGO_TARGET_DIR=$(VERIFY_TARGET) cargo test

dev:
	cargo tauri dev

build release:
	cargo tauri build

clean:
	rm -rf src-tauri/target src-tauri/gen dist
