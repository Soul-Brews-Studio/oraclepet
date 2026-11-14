#!/usr/bin/env bash
# stage.sh — copy the per-pack firmware bins from ../dist-firmware into this
# webflasher dir and (re)generate one esp-web-tools manifest per pack.
#
# Run after ../tools/build-packs.sh. The *.bin and manifest-*.json here are
# gitignored build artifacts; index.html / serve.sh / README.md are the
# committed source. Pattern learned from agents/1-waveshare7/webflasher
# (esp-web-tools), adapted: separate parts (shared storage/bootloader) so the
# 4 packs differ only by the app image, and one selectable install button.
#
# Flash layout (build/flash_args): bootloader@0x0, partition-table@0x8000,
# app@0x10000, storage@0x290000.
set -euo pipefail
cd "$(dirname "$0")"
SRC=../dist-firmware
[ -d "$SRC" ] || { echo "❌ run ../tools/build-packs.sh first ($SRC missing)"; exit 1; }

cp -f "$SRC"/bootloader.bin "$SRC"/partition-table.bin "$SRC"/storage.bin .
cp -f "$SRC"/jc3248_pet_idf-*.bin .

# Live preview: the real gif-wasm decoder (the same AnimatedGIF C core the device
# runs) showing each pack's actual 96×100 frames. Self-contained copy so the
# flasher previews offline; driven via ?pack=<name>&embed=1 from index.html.
PREV=../../gif-wasm/web
if [ -d "$PREV" ]; then
  mkdir -p preview/gifs
  cp -f "$PREV"/gifdec.js "$PREV"/gifdec.wasm "$PREV"/index.html preview/
  cp -R "$PREV"/gifs/. preview/gifs/
  echo "  preview/ ← gif-wasm decoder (live pack preview)"
else
  echo "⚠ no $PREV — preview disabled (build lab/gif-wasm first)"
fi

for app in jc3248_pet_idf-*.bin; do
  pack="${app#jc3248_pet_idf-}"; pack="${pack%.bin}"
  cat > "manifest-$pack.json" <<JSON
{
  "name": "ESP32 Oracle desk-pet — $pack",
  "version": "1.0.0",
  "new_install_prompt_erase": false,
  "builds": [
    {
      "chipFamily": "ESP32-S3",
      "parts": [
        { "path": "bootloader.bin",      "offset": 0 },
        { "path": "partition-table.bin", "offset": 32768 },
        { "path": "$app",                "offset": 65536 },
        { "path": "storage.bin",         "offset": 2686976 }
      ]
    }
  ]
}
JSON
  echo "  manifest-$pack.json → $app"
done
echo "✓ staged $(ls jc3248_pet_idf-*.bin | wc -l | tr -d ' ') pack(s) — now: ./serve.sh"
