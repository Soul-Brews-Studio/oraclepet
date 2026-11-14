# jc3248-pet — plain build/flash for the single growing pet program.
#   make build      # compile
#   make flash      # compile + upload
#   make run        # compile + upload + serial monitor (one shot)
#   make monitor    # just the serial monitor (Ctrl-C to quit)
#   make uploadfs   # flash the LittleFS data/ image (the character pack — step 3+)
#   make flash PORT=/dev/cu.usbmodemXXXX
PORT ?= /dev/cu.usbmodem13301
ENV   = jc3248w535c
# Resolve pio: PATH first, then the standard PlatformIO venv (make recipes run via
# /bin/sh, which doesn't see interactive shell aliases — so fall back here).
PIO  := $(shell command -v pio 2>/dev/null || echo $(HOME)/.platformio/penv/bin/pio)

.PHONY: build flash run monitor uploadfs
build:
	$(PIO) run -e $(ENV)
flash:
	$(PIO) run -e $(ENV) -t upload --upload-port $(PORT)
run:
	$(PIO) run -e $(ENV) -t upload -t monitor --upload-port $(PORT)
monitor:
	$(PIO) device monitor -p $(PORT) -b 115200
uploadfs:
	$(PIO) run -e $(ENV) -t uploadfs --upload-port $(PORT)
