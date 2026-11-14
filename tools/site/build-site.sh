#!/usr/bin/env bash
# Assemble the GitHub Pages site into _site/ from dist/ (build-firmware.sh) and the
# web pages already in the repo. No build tools needed beyond bash and python3.
#
#   /            hub (tools/site/index.html)
#   /usb/        USB flasher for the JC3248 (esp-web-tools), with live pack preview
#   /usb/<board>/  one-button USB flasher for sc01 / lcd28
#   /ble/        Web Bluetooth tool: OTA firmware, packs, apps (lab/oraclepet-lib/web)
#   /firmware/   every .bin + SHA256SUMS, for BLE OTA and esptool users
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DIST="$ROOT/dist"
SITE="$ROOT/_site"
rm -rf "$SITE" && mkdir -p "$SITE"

# The home page: the React app in site/ (USB install with esptool-js, the OTA base,
# links to the BLE tool). style.css serves the plain sub-pages below.
( cd "$ROOT/site" && { [ -d node_modules ] || npm ci --no-audit --no-fund; } && npm run build )
cp -R "$ROOT/site/dist/." "$SITE/"
cp "$ROOT/tools/site/style.css" "$SITE/"
touch "$SITE/.nojekyll"

# Firmware files, served once and referenced by every manifest.
mkdir -p "$SITE/firmware"
cp -R "$DIST"/. "$SITE/firmware/"
# Pages has no directory listing, so write one.
{
  echo '<!doctype html><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">'
  echo '<title>OraclePet firmware</title><link rel="stylesheet" href="../style.css"><main>'
  echo '<p class="crumb"><a href="../">OraclePet</a> / firmware</p><h1>Firmware</h1>'
  echo '<p class="note">App images (<code>app-*.bin</code>) are what the BLE tool sends. The other files are for a full USB or esptool flash.</p><ul>'
  ( cd "$DIST" && find . -type f | sed 's#^\./##' | sort ) | while read -r f; do
    echo "<li><a href=\"$f\">$f</a> <span class=\"note\">$(wc -c < "$DIST/$f" | tr -d ' ') bytes</span></li>"
  done
  echo '</ul></main>'
} > "$SITE/firmware/index.html"

# Example pet apps for the preview and for sending over BLE (needs zig; skipped without it).
if command -v zig >/dev/null; then
  mkdir -p "$SITE/apps"
  # build.sh also runs an optional load check in the native WAMR (sim/petsim), which needs the
  # firmware's managed_components and exits non-zero without them: judge by the .wasm itself.
  ( cd "$ROOT/lab/pet-sdk" && for src in examples/*.c examples/*.zig; do
      out="build/$(basename "${src%.*}").wasm"
      rm -f "$out"
      ./build.sh "$src" >/tmp/petapp-build.log 2>&1 || true
      if [ -s "$out" ]; then cp "$out" "$SITE/apps/"
      else echo "skip app $src (no wasm):"; tail -5 /tmp/petapp-build.log; fi
    done )
else
  echo "no zig: the preview has no example apps"
fi

# firmware/index.json + packs/<id>.gif for the React page.
python3 "$ROOT/tools/site/catalog.py" "$DIST" "$SITE" "${GITHUB_SHA:-$(git -C "$ROOT" rev-parse HEAD 2>/dev/null || echo local)}"

manifest() { # <name> <board dir rel to manifest> <app file>
  cat <<JSON
{
  "name": "$1",
  "version": "${GITHUB_SHA:-local}",
  "new_install_prompt_erase": true,
  "builds": [
    {
      "chipFamily": "ESP32-S3",
      "parts": [
        { "path": "$2/bootloader.bin",       "offset": 0 },
        { "path": "$2/partition-table.bin",  "offset": 32768 },
        { "path": "$2/ota_data_initial.bin", "offset": 53248 },
        { "path": "$2/$3",                   "offset": 65536 },
        { "path": "$2/storage.bin",          "offset": 2686976 }
      ]
    }
  ]
}
JSON
}

# JC3248: the existing flasher page, with its pack picker and live WASM preview.
U="$SITE/usb"; mkdir -p "$U/preview/gifs"
cp "$ROOT/lab/jc3248-pet-idf/webflasher/index.html" "$U/"
P="$ROOT/lab/gif-wasm/web"
cp "$P/gifdec.js" "$P/gifdec.wasm" "$P/index.html" "$U/preview/"
cp -R "$P/gifs/." "$U/preview/gifs/"
for app in "$DIST"/jc3248/app-*.bin; do
  pack="$(basename "$app" .bin)"; pack="${pack#app-}"
  manifest "OraclePet JC3248W535 · $pack" ../firmware/jc3248 "app-$pack.bin" > "$U/manifest-$pack.json"
done

# SC01 Plus and the 2.8in board: one button each.
for board in sc01 lcd28; do
  [ -d "$DIST/$board" ] || continue
  mkdir -p "$U/$board"
  manifest "OraclePet $board · siamese" ../../firmware/$board app-siamese.bin > "$U/$board/manifest.json"
  sed "s/__BOARD__/$board/g" "$ROOT/tools/site/board.html" > "$U/$board/index.html"
done

# Web Bluetooth tool.
mkdir -p "$SITE/ble"
cp "$ROOT"/lab/oraclepet-lib/web/{index.html,oraclepet.js,simpet.js} "$SITE/ble/"

find "$SITE" -type f | sed "s#^$SITE##" | sort
