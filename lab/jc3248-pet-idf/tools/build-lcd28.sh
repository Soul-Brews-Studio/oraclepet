#!/usr/bin/env bash
# Build the pet firmware for the 2.8" ILI9341 board (main/board.h: lcd28) into build-lcd28/.
# Separate build dir and sdkconfig, so the JC3248 build in build/ is untouched.
#   tools/build-lcd28.sh                 -> build-lcd28/oraclepet_lcd28.bin (+ bootloader, tables, storage)
set -euo pipefail
cd "$(dirname "$0")/.."
idf.py -B build-lcd28 -DPET_BOARD=lcd28 -DSDKCONFIG=build-lcd28/sdkconfig -DPET_DEFAULT_PACK=siamese build
