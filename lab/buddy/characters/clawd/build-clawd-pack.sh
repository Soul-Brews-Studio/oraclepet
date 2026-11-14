#!/usr/bin/env bash
# Build the "clawd" GIF pack — the official Claude mascot — for the pet roster.
# Source: anthropics/claude-desktop-buddy (MIT), via the eduardo-moro Waveshare
# ESP32-S3 fork's characters/clawd/ (120x120 animated GIFs).
# Pipeline: coalesce -> subsample (the sources are 31-60 frames) -> fit 96x100 on
# the manifest bg (#282828) -> palette -> gifsicle -O3. Keeps upstream filenames
# so characters/clawd/manifest.json (verbatim from upstream) works unchanged.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../../.." && pwd)"
RAW="$ROOT/.tmp/cat-assets/raw/clawd"
W=96; H=100; BG="#282828"; COLORS=128
[ -d "$RAW" ] || { echo "❌ raw clawd gifs missing: $RAW (run fetch first)"; exit 1; }
echo "🤖 building clawd pack → $HERE"

# gifproc <srcname> <maxframes> <delay-cs>  (out name == src name)
gifproc() {
  local name="$1" maxf="$2" delay="$3" src="$RAW/$1.gif"
  [ -f "$src" ] || { echo "  ⚠️  $name: missing"; return 0; }
  local T; T="$(mktemp -d)"
  magick "$src" -coalesce "$T/f_%03d.png"
  local all=( "$T"/f_*.png ); local total=${#all[@]}
  local step=$(( (total + maxf - 1) / maxf )); [ "$step" -lt 1 ] && step=1
  local sel=() i=0
  for f in "${all[@]}"; do [ $((i % step)) -eq 0 ] && sel+=("$f"); i=$((i+1)); done
  magick "${sel[@]}" -background "$BG" -alpha remove -alpha off \
      -filter Lanczos -resize "${W}x${H}" -background "$BG" -gravity center -extent "${W}x${H}" \
      -set delay "$delay" -loop 0 -colors "$COLORS" +dither "$HERE/$name.gif"
  gifsicle -O3 --colors "$COLORS" -o "$HERE/$name.gif" "$HERE/$name.gif" 2>/dev/null
  printf "  ✅ %-30s %2d/%-2d frames  %5sB\n" "$name.gif" "${#sel[@]}" "$total" "$(wc -c <"$HERE/$name.gif"|tr -d ' ')"
}

gifproc clawd-sleeping          16 12
gifproc clawd-idle              16  9
gifproc clawd-headphones-groove 16  8
gifproc clawd-idle-reading      18  9
gifproc clawd-typing            16  7
gifproc clawd-react-double-jump 16  7
gifproc clawd-happy             16  8
gifproc clawd-thinking          16  9

echo "🤖 done. pack total: $(du -sh "$HERE" | awk '{print $1}')"
