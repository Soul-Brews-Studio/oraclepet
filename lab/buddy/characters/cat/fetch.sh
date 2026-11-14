#!/usr/bin/env bash
# Fetch the CC0 source art for the cat pack into the gitignored staging dir.
# Source: Tiny Kitten Game Sprite — OpenGameArt, CC0 1.0.
# Idempotent: skips files already present. Run before build-cat-pack.sh.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../../.." && pwd)"
RAW="$ROOT/.tmp/cat-assets/raw"
UA="Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15) AppleWebKit/537.36 esp32-oracle-asset-fetch"
mkdir -p "$RAW/tiny_cat"

ZIP="$RAW/tiny_cat_sprite.zip"
if [ ! -f "$ZIP" ]; then
  echo "↓ tiny_cat_sprite.zip (CC0, OpenGameArt)"
  curl -fsSL -A "$UA" -o "$ZIP" \
    "https://opengameart.org/sites/default/files/tiny_cat_sprite.zip"
fi
unzip -o -q "$ZIP" -d "$RAW/tiny_cat"
echo "✅ frames at: $RAW/tiny_cat/TINY CAT SPRITE/  ($(find "$RAW/tiny_cat" -iname '*.png' | wc -l | tr -d ' ') PNGs)"
