# OraclePet host library: one protocol (PROTOCOL.md), three ports checked against Swift.
#   make test          every suite below; all must pass (the golden-vector rule, README.md)
#   make test-core     C core: native tests, the wasm build, and the same vectors through wasm
#   make test-python   Python: unittest (stdlib only; uses python/.venv when it exists)
#   make test-web      Web: node --test, run from inside web/ (it fails from the folder above)
#   make vectors       regenerate golden/vectors.json from the Swift reference (macOS + swift)
# Everything is offline: no test scans, connects or flashes a pet.

PYTHON ?= $(if $(wildcard python/.venv/bin/python),$(CURDIR)/python/.venv/bin/python,python3)
NODE   ?= node

.PHONY: test test-core test-python test-web vectors clean

test: test-core test-python test-web
	@echo "oraclepet-lib: core, python and web suites all passed"

test-core:
	$(MAKE) -C core test wasm wasm-test

test-python:
	cd python && $(PYTHON) -m unittest discover -s tests

test-web:
	cd web && $(NODE) --test

vectors:
	tools/gen_vectors.sh

clean:
	$(MAKE) -C core clean
