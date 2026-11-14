#!/usr/bin/env bash
# Build every pet firmware the web flasher offers, into dist/.
# Runs inside the espressif/idf:v6.0.1 image (see .github/workflows/pages.yml),
# or locally after `. $IDF_PATH/export.sh`.
#
#   dist/jc3248/{bootloader,partition-table,ota_data_initial,storage}.bin
#   dist/jc3248/app-<pack>.bin       one app image per default pack
#   dist/sc01/…, dist/lcd28/…        default pack: siamese
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PET="$ROOT/lab/jc3248-pet-idf"
OUT="$ROOT/dist"
JC_PACKS=(${JC_PACKS:-bufo cat cat-orange cat-pet clawd siamese})

# The LittleFS image holds every pack; the build bakes it from data/.
rm -rf "$PET/data" && mkdir -p "$PET/data/characters"
for d in "$ROOT"/lab/buddy/characters/*/; do
  p="$(basename "$d")"
  [ -f "$d/manifest.json" ] || continue
  mkdir -p "$PET/data/characters/$p"
  cp "$d"/manifest.json "$d"/*.gif "$PET/data/characters/$p/"
done
du -sh "$PET/data"

copy_common() { # <build dir> <out dir>
  cp "$1/bootloader/bootloader.bin" "$1/partition_table/partition-table.bin" \
     "$1/ota_data_initial.bin" "$1/storage.bin" "$2/"
}

cd "$PET"
mkdir -p "$OUT/jc3248"
for pack in "${JC_PACKS[@]}"; do
  echo "── jc3248 · $pack"
  idf.py -B build -DPET_BOARD=jc3248 -DPET_DEFAULT_PACK="$pack" build
  cp build/jc3248_pet_idf.bin "$OUT/jc3248/app-$pack.bin"
done
copy_common build "$OUT/jc3248"

for board in sc01 lcd28; do
  echo "── $board · siamese"
  mkdir -p "$OUT/$board"
  idf.py -B "build-$board" -DPET_BOARD="$board" -DSDKCONFIG="build-$board/sdkconfig" \
         -DPET_DEFAULT_PACK=siamese build
  cp "build-$board/oraclepet_$board.bin" "$OUT/$board/app-siamese.bin"
  copy_common "build-$board" "$OUT/$board"
done

( cd "$OUT" && find . -name '*.bin' -exec ls -l {} \; && sha256sum */*.bin > SHA256SUMS )
