#!/usr/bin/env bash
# Build the "siamese" desk-pet GIF pack from frames rendered by render-frames.mjs.
# Same format as cat-orange: 96x100 on black, palette, whole frames (see fullframes), and a
# manifest.json with the 7 states the jc3248 pet firmware reads. Frames come from the sprite rig
# in nexus-oracle (laris-co/nexus-oracle ψ/lab/sprite-one-image); see PROVENANCE.md.
#
#   ego-browser nodejs < render-frames.mjs     # -> /tmp/siamese-frames/<state>/NN.png
#   bash build-siamese-pack.sh                 # -> the GIFs + manifest.json beside this script
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
RAW="${1:-/tmp/siamese-frames}"
OUT="$HERE"
W=96; H=100; COLORS=128
[ -d "$RAW/idle" ] || { echo "❌ frames missing: $RAW (run render-frames.mjs first)"; exit 1; }
mkdir -p "$OUT"

# One crop box for every state, so the cat keeps the same size when it changes state.
# %@ already is the trim box; adding -trim first would reset its offset to +0+0.
BOX="$(magick "$RAW"/*/*.png -background none -layers merge -format '%@' info:)"
echo "🐱 union box $BOX"

fit() { magick "$1" -crop "$BOX" +repage -background black -alpha remove -alpha off \
  -filter Lanczos -resize "${W}x${H}" -background black -gravity center -extent "${W}x${H}" "$2"; }

T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT

# Every frame whole (96x100, no transparent index). The pet firmware (gif.cpp GIFDraw) paints a
# transparent pixel with the background colour instead of keeping the previous frame, so the
# delta frames that `gifsicle -O3` makes showed black specks inside the cat on the device.
fullframes() {
  gifsicle --colors "$COLORS" -o "$1" "$1"                      # one global palette for all frames
  gifsicle --unoptimize --no-transparent -o "$1" "$1"
  if gifsicle --info "$1" | grep -q transparent; then echo "❌ $1 still has transparency"; exit 1; fi
}
mkgif() {   # mkgif <state> <delay in 1/100 s>
  local s="$1" delay="$2" i=0 args=()
  mkdir -p "$T/$s"
  for f in "$RAW/$s"/*.png; do fit "$f" "$T/$s/$i.png"; args+=("$T/$s/$i.png"); i=$((i+1)); done
  case "$s" in heart) addheart "$T/$s" ;; attention) addbang "$T/$s" ;; sleep) addzzz "$T/$s" ;; esac
  magick -delay "$delay" -loop 0 "${args[@]}" -colors "$COLORS" +dither "$OUT/$s.gif"
  fullframes "$OUT/$s.gif"
  printf "  ✅ %-14s %2d frames  %6sB\n" "$s.gif" "$i" "$(wc -c <"$OUT/$s.gif" | tr -d ' ')"
}

# heart: a heart rises over the head and fades (same drawing as cat-orange's pack).
addheart() {
  magick -size 28x28 xc:none -fill '#FF4D6D' -stroke '#FFFFFF' -strokewidth 1 \
    -draw "path 'M14,25 C14,25 3,17 3,9 C3,5 6,3 8.5,3 C11,3 14,6 14,7 C14,6 17,3 19.5,3 C22,3 25,5 25,9 C25,17 14,25 14,25 Z'" "$T/heart.png"
  local n i; n=$(ls "$1"/*.png | wc -l | tr -d ' ')
  for ((i = 0; i < n; i++)); do
    local y=$(( 22 - i * 3 )) scale=$(( 60 + i * 6 )) op
    op=$(awk -v i="$i" -v n="$n" 'BEGIN{ print (i < n/2) ? 1.0 : 1.0 - (i - n/2) / (n/2) * 0.8 }')
    magick "$T/heart.png" -resize "${scale}%" -channel A -evaluate multiply "$op" +channel "$T/h.png"
    magick "$1/$i.png" "$T/h.png" -gravity north -geometry "+18+${y}" -composite -background black -alpha remove "$1/$i.png"
  done
}

# attention: a steady yellow "!" badge (no blinking: calmer), so a waiting prompt reads from across the desk.
addbang() {
  magick -size 26x26 xc:none -fill '#FFC83D' -stroke '#000000' -strokewidth 2 -draw "circle 13,13 13,2" \
    -fill '#000000' -stroke none -draw "roundrectangle 11,6 15,16 2,2" -draw "circle 13,20 13,18" "$T/bang.png"
  local n i; n=$(ls "$1"/*.png | wc -l | tr -d ' ')
  for ((i = 0; i < n; i++)); do
    magick "$1/$i.png" "$T/bang.png" -gravity northeast -geometry "+6+4" -composite -background black -alpha remove "$1/$i.png"
  done
}

# sleep: dimmed body and "z"s drifting up.
addzzz() {
  local n i dim="${2:-35}"; n=$(ls "$1"/*.png | wc -l | tr -d ' ')
  for ((i = 0; i < n; i++)); do
    local y=$(( 44 - i * 2 ))
    magick "$1/$i.png" -fill black -colorize "${dim}%" -fill '#B8D4FF' -stroke none -pointsize 14 -font '/System/Library/Fonts/Supplemental/Arial Bold.ttf' \
      -annotate "+68+${y}" z -pointsize 18 -annotate "+77+$(( y - 10 ))" Z \
      -background black -alpha remove "$1/$i.png"
  done
}

# sleep from its own drawing: the rig cannot close eyes or lie the body down, so sleep-src.png
# is a second Codex drawing of the same cat curled up asleep. Key out the white, put it on the
# standing cat's baseline, and breathe: the body rises and falls about its bottom edge.
sleep_from_image() {
  local src="$1" n=8 i
  mkdir -p "$T/sleep"
  magick "$src" -alpha set -bordercolor white -border 1 -fuzz 8% -fill none -draw "color 0,0 floodfill" \
    -shave 1x1 -trim +repage -filter Lanczos -resize 62x48 "$T/sleep-base.png"   # head about the size of the standing cat's
  local bw bh
  bw=$(magick identify -format '%w' "$T/sleep-base.png"); bh=$(magick identify -format '%h' "$T/sleep-base.png")
  local args=()
  for ((i = 0; i < n; i++)); do
    local h; h=$(awk -v b="$bh" -v i="$i" -v n="$n" 'BEGIN{ printf "%d", b * (1 + 0.04 * sin(6.2832 * i / n)) + 0.5 }')
    magick -size "${W}x${H}" xc:black \( "$T/sleep-base.png" -resize "${bw}x${h}!" \) \
      -gravity south -geometry +0+1 -composite "$T/sleep/$i.png"
    args+=("$T/sleep/$i.png")
  done
  addzzz "$T/sleep" 15
  magick -delay 30 -loop 0 "${args[@]}" -colors "$COLORS" +dither "$OUT/sleep.gif"
  fullframes "$OUT/sleep.gif"
  printf "  ✅ %-14s %2d frames  %6sB (from sleep-src.png)\n" "sleep.gif" "$n" "$(wc -c <"$OUT/sleep.gif" | tr -d ' ')"
}

echo "🐱 building siamese → $OUT"
mkgif idle      12
mkgif busy      12
mkgif celebrate  8
mkgif attention 10
mkgif dizzy      9
if [ -f "$HERE/sleep-src.png" ]; then sleep_from_image "$HERE/sleep-src.png"; else mkgif sleep 30; fi
mkgif heart     12

cat > "$OUT/manifest.json" <<'JSON'
{
  "name": "siamese",
  "colors": {
    "body": "#EADBC4",
    "bg": "#000000",
    "text": "#FFFFFF",
    "textDim": "#808080",
    "ink": "#000000"
  },
  "states": {
    "sleep": "sleep.gif",
    "idle": ["idle.gif"],
    "busy": "busy.gif",
    "attention": "attention.gif",
    "celebrate": "celebrate.gif",
    "dizzy": "dizzy.gif",
    "heart": "heart.gif"
  }
}
JSON
echo "🐱 done. pack total: $(du -sh "$OUT" | awk '{print $1}')"
