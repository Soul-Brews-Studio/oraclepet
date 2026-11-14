# jc3248-pet-idf — reproducible ESP-IDF v6 build/flash for the IDF desk-pet port.
#
#   make build               # configure (if needed) + compile
#   make flash               # build + write firmware over USB-Serial-JTAG
#   make monitor             # serial monitor only (Ctrl-] to quit)
#   make run                 # build + flash + monitor (one shot)
#   make fullclean           # wipe build/ + CMake cache (use after toolchain churn)
#   make set-target          # (re)set target to esp32s3 — only needed once / after fullclean
#   make menuconfig          # interactive sdkconfig editor
#   make flash PORT=/dev/cu.usbmodemXXXX
#   make flash CARDPUTER_PORT_SKETCH=ON   # separate Cardputer-port demo firmware
#
# WHY THIS MAKEFILE EXISTS (the python-env mess):
#   ESP-IDF derives its venv name as  idf<idfver>_py<X.Y>_env  from whatever
#   python runs tools/activate.py (idf_tools.py:103 PYTHON_VER_MAJOR_MINOR).
#   The venv installed here is idf6.0_py3.13_env, but the system python3 is 3.14,
#   so a bare `. export.sh` looks for idf6.0_py3.14_env and dies.
#   Worse: idf.py bakes -DPYTHON=$sys.executable into CMakeCache at configure time
#   and string-compares it on every rebuild (tools.py:785). The venv ships BOTH
#   bin/python and bin/python3 (same real interpreter, two paths), and idf.py's
#   `#!/usr/bin/env python` shebang vs an interactive `python`->`python3` alias can
#   resolve to either string between runs -> "'.../python' is currently active while
#   the project was configured with '.../python3'. Run idf.py fullclean".
#
#   FIX (belt + suspenders):
#     1. A shim dir provides BOTH `python` and `python3` -> python3.13, prepended to
#        PATH, so export.sh's detect_python.sh picks 3.13 (-> idf6.0_py3.13_env).
#     2. IDF_PYTHON_ENV_PATH is pinned explicitly, so the venv is never re-derived
#        from the live interpreter version.
#     3. idf.py is ALWAYS launched via the venv's absolute bin/python3. That makes
#        sys.executable a byte-identical string on every run -> the cached -DPYTHON
#        always matches -> the "run fullclean" mismatch can never recur.

PORT      ?= /dev/cu.usbmodem2101
TARGET    ?= esp32s3
BAUD      ?= 115200
CARDPUTER_PORT_SKETCH ?= OFF

IDF_PATH  ?= $(HOME)/esp/esp-idf
# The venv that actually exists on this machine (py3.13). Pinning it means IDF
# never tries to derive idf6.0_py3.14_env from the system python3.
IDF_VENV  ?= $(HOME)/.espressif/python_env/idf6.0_py3.13_env

# Canonical interpreter: the venv's python3 by ABSOLUTE path. sys.executable will
# equal this exact string every run, so -DPYTHON in CMakeCache always matches.
IDF_PY    := $(IDF_VENV)/bin/python3
IDF_SCRIPT := $(IDF_PATH)/tools/idf.py

# Self-healing shim so detect_python.sh resolves python/python3 to 3.13.
SHIM      := /tmp/idf-pyshim
PY313     := $(shell command -v python3.13 2>/dev/null)

# One line that prepares a fully-activated, deterministic IDF environment in the
# make recipe's /bin/sh. Every target reuses it. The trailing `&&` chains the
# real command after the env is live.
#   - build the shim (idempotent)
#   - pin IDF_PATH + IDF_PYTHON_ENV_PATH
#   - put shim first on PATH, then source export.sh quietly
IDFENV = \
	mkdir -p $(SHIM) && \
	ln -sf "$(PY313)" $(SHIM)/python && \
	ln -sf "$(PY313)" $(SHIM)/python3 && \
	export IDF_PATH="$(IDF_PATH)" && \
	export IDF_PYTHON_ENV_PATH="$(IDF_VENV)" && \
	export PATH="$(SHIM):$$PATH" && \
	. "$(IDF_PATH)/export.sh" >/dev/null 2>&1 &&

# Always invoke idf.py through the pinned absolute interpreter (NOT the `idf.py`
# wrapper on PATH, whose shebang is non-deterministic).
IDFPY = "$(IDF_PY)" "$(IDF_SCRIPT)"

.PHONY: build flash monitor run fullclean set-target menuconfig size doctor deps

build:
	@test -n "$(PY313)" || { echo "python3.13 not found on PATH — run: brew install python@3.13"; exit 1; }
	$(IDFENV) $(IDFPY) $(if $(PACK),-DPET_DEFAULT_PACK=$(PACK)) -DCARDPUTER_PORT_SKETCH=$(CARDPUTER_PORT_SKETCH) build

# Download managed components only (esp_lcd_axs15231b ==2.1.0 + transitive
# esp_lcd_touch, cmake_utilities) and refresh dependencies.lock, without a full
# build. Populates managed_components/ — both are gitignored build artifacts.
deps:
	$(IDFENV) $(IDFPY) reconfigure

flash: build
	$(IDFENV) $(IDFPY) -p $(PORT) flash

monitor:
	$(IDFENV) $(IDFPY) -p $(PORT) monitor

run: build
	$(IDFENV) $(IDFPY) -p $(PORT) flash monitor

set-target:
	$(IDFENV) $(IDFPY) set-target $(TARGET)

menuconfig:
	$(IDFENV) $(IDFPY) menuconfig

fullclean:
	$(IDFENV) $(IDFPY) fullclean

size:
	$(IDFENV) $(IDFPY) size

# Print the resolved environment without building — for debugging the python mess.
doctor:
	@echo "PY313      = $(PY313)"
	@echo "IDF_PATH   = $(IDF_PATH)"
	@echo "IDF_VENV   = $(IDF_VENV)"
	@echo "IDF_PY     = $(IDF_PY)"
	@echo "PORT       = $(PORT)"
	$(IDFENV) $(IDFPY) --version && \
		echo "sys.executable = $$("$(IDF_PY)" -c 'import sys; print(sys.executable)')"
