#!/usr/bin/env bash
# build-packs.sh — "multiple firmware": one .bin per character pack.
#
# Every pack (bufo + cat + cat-orange + cat-pet + clawd) is baked into the SAME 3 MB
# LittleFS storage image; the firmware's PET_DEFAULT_PACK compile-define picks
# which one boots. This builds a separate app .bin per pack so you can flash
# whichever cat (or frog) you want onto a given pet — many bodies, one soul.
#
#   ./tools/build-packs.sh                 # all packs in data/characters/
#   ./tools/build-packs.sh cat cat-orange  # just these
#
# Output → dist-firmware/:
#   jc3248_pet_idf-<pack>.bin   (the app, one per pack)
#   bootloader.bin, partition-table.bin, storage.bin   (shared — same for all)
#
# Flash one pack (device on PORT):
#   esptool.py -p <PORT> -b 460800 write_flash \
#     0x0      dist-firmware/bootloader.bin \
#     0x8000   dist-firmware/partition-table.bin \
#     0x10000  dist-firmware/jc3248_pet_idf-<pack>.bin \
#     0x290000 dist-firmware/storage.bin
#   (offsets: app @ 0x10000, storage @ 0x290000 per partitions.csv)
set -euo pipefail
cd "$(dirname "$0")/.."          # → lab/jc3248-pet-idf/

# ── Deterministic ESP-IDF env (mirrors ./Makefile — the py3.13-vs-3.14 fix) ──
# A bare `. export.sh` derives idf6.0_py3.14_env from the system python3 (3.14)
# and dies (the venv is py3.13). So: a shim makes python/python3 → 3.13, and we
# pin IDF_PYTHON_ENV_PATH so the venv is never re-derived. See Makefile header.
IDF_PATH="${IDF_PATH:-$HOME/esp/esp-idf}"
IDF_VENV="$HOME/.espressif/python_env/idf6.0_py3.13_env"
PY313="$(command -v python3.13 || true)"
[ -n "$PY313" ]      || { echo "❌ python3.13 not on PATH — run: brew install python@3.13"; exit 1; }
[ -d "$IDF_VENV" ]   || { echo "❌ IDF venv not found: $IDF_VENV"; exit 1; }
[ -f "$IDF_PATH/export.sh" ] || { echo "❌ IDF_PATH invalid: $IDF_PATH"; exit 1; }
SHIM="${TMPDIR:-/tmp}/idf-pyshim"
mkdir -p "$SHIM"; ln -sf "$PY313" "$SHIM/python"; ln -sf "$PY313" "$SHIM/python3"
export IDF_PATH IDF_PYTHON_ENV_PATH="$IDF_VENV" PATH="$SHIM:$PATH"
# shellcheck disable=SC1091
. "$IDF_PATH/export.sh" >/dev/null 2>&1
IDFPY=("$IDF_VENV/bin/python3" "$IDF_PATH/tools/idf.py")   # absolute, deterministic

PACKS=("$@"); [ ${#PACKS[@]} -eq 0 ] && PACKS=(bufo cat cat-orange cat-pet clawd)
OUT=dist-firmware; mkdir -p "$OUT"
for pack in "${PACKS[@]}"; do
  [ -d "data/characters/$pack" ] || { echo "⚠ skip '$pack' — no data/characters/$pack"; continue; }
  echo "──────── building firmware: pack=$pack ────────"
  "${IDFPY[@]}" -DPET_DEFAULT_PACK="$pack" build
  cp -f build/jc3248_pet_idf.bin "$OUT/jc3248_pet_idf-$pack.bin"
  echo "  → $OUT/jc3248_pet_idf-$pack.bin"
done
# Shared flashing companions (identical across packs — all packs live in one image)
cp -f build/bootloader/bootloader.bin           "$OUT/" 2>/dev/null || true
cp -f build/partition_table/partition-table.bin "$OUT/" 2>/dev/null || true
cp -f build/storage.bin                         "$OUT/" 2>/dev/null || true
echo "──────── done ────────"
( cd "$OUT" && ls -la *.bin && echo && shasum -a 256 jc3248_pet_idf-*.bin )
