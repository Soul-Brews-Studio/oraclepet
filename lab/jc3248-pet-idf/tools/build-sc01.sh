#!/usr/bin/env bash
# Build the pet firmware for the WT32-SC01 Plus (main/board.h: sc01) into build-sc01/.
# Separate build dir and sdkconfig, so the JC3248 and lcd28 builds are untouched.
#   tools/build-sc01.sh                  -> build-sc01/oraclepet_sc01.bin (+ bootloader, tables, storage)
set -euo pipefail
cd "$(dirname "$0")/.."
idf.py -B build-sc01 -DPET_BOARD=sc01 -DSDKCONFIG=build-sc01/sdkconfig -DPET_DEFAULT_PACK="${PACK:-siamese}" build
