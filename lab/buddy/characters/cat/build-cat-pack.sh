#!/usr/bin/env bash
# Build the "cat" GIF pack (sibling to bufo) from CC0 Tiny Cat Sprite frames.
# Source: https://opengameart.org/content/tiny-kitten-game-sprite  (CC0)
# Pipeline per state: union-trim across the animation (zoom to the cat, no jitter)
#   -> composite onto black -> fit 96x100 centered -> palette-reduce -> gifsicle -O3.
# Output: <this dir>/<state>.gif + manifest.json. Re-runnable. Needs: magick (IM7), gifsicle.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"                 # lab/buddy/characters/cat
ROOT="$(cd "$HERE/../../../.." && pwd)"                # repo worktree root
RAW="$ROOT/.tmp/cat-assets/raw/tiny_cat/TINY CAT SPRITE"
W=96; H=100; COLORS=128

[ -d "$RAW" ] || { echo "❌ raw frames missing: $RAW  (run fetch first)"; exit 1; }
command -v magick   >/dev/null || { echo "❌ need ImageMagick 7 (magick)"; exit 1; }
command -v gifsicle >/dev/null || { echo "❌ need gifsicle"; exit 1; }

# mkgif <out-name> <delay-cs> <frame.png ...>
# Computes the union bounding box across the frames (so the cat fills the canvas and
# stays aligned), crops all frames to it, flattens onto black, fits 96x100, quantizes.
mkgif() {
  local name="$1" delay="$2"; shift 2
  local frames=("$@")
  [ "${#frames[@]}" -gt 0 ] || { echo "  ⚠️  $name: no frames"; return 1; }
  # union bbox across the whole animation (merge -> trim), in original frame coords
  local box
  box="$(magick "${frames[@]}" -background none -layers merge -trim -format '%@' info: 2>/dev/null)"
  [ -n "$box" ] || box="${W}x${H}+0+0"
  magick "${frames[@]}" \
      -crop "$box" +repage \
      -background black -alpha remove -alpha off \
      -filter Lanczos -resize "${W}x${H}" \
      -background black -gravity center -extent "${W}x${H}" \
      -set delay "$delay" -loop 0 \
      -colors "$COLORS" +dither \
      "$HERE/$name.gif"
  gifsicle -O3 --colors "$COLORS" -o "$HERE/$name.gif" "$HERE/$name.gif"
  printf "  ✅ %-12s %2d frames  %5sB  delay=%scs\n" "$name.gif" "${#frames[@]}" \
      "$(wc -c <"$HERE/$name.gif" | tr -d ' ')" "$delay"
}

# sorted-numeric glob into an array
glob() { local IFS=$'\n'; printf '%s\n' "$RAW/$1"/*.png | sort -V; }

echo "🐱 building cat pack → $HERE"
mapfile -t IDLE  < <(glob "01_Idle")
mapfile -t RUN   < <(glob "02_Run")
mapfile -t JUP   < <(glob "03_Jump/01_Up")
mapfile -t JFALL < <(glob "03_Jump/02_Fall")
mapfile -t HURT  < <(glob "04_Hurt")
mapfile -t DEAD  < <(glob "05_Dead")

mkgif idle       10 "${IDLE[@]}"
mkgif busy        7 "${RUN[@]}"
mkgif attention   9 "${JUP[@]}"
mkgif celebrate   7 "${JUP[@]}" "${JFALL[@]}"
mkgif dizzy       9 "${HURT[@]}"
mkgif sleep      18 "${DEAD[@]}"

# --- heart.gif: composed tap-reaction (static cat + a heart that pops & rises/fades) ---
echo "  · composing heart.gif"
TMP="$(mktemp -d)"
# processed static cat base (first idle frame, same 96x100 pipeline)
ibox="$(magick "${IDLE[0]}" -background none -trim -format '%@' info:)"
magick "${IDLE[0]}" -crop "$ibox" +repage -background black -alpha remove -alpha off \
   -filter Lanczos -resize "${W}x${H}" -background black -gravity center -extent "${W}x${H}" \
   "$TMP/base.png"
# heart sprite (vector path, transparent)
magick -size 28x28 xc:none -fill '#FF4D6D' -stroke '#FFFFFF' -strokewidth 1 \
   -draw "path 'M14,25 C14,25 3,17 3,9 C3,5 6,3 8.5,3 C11,3 14,6 14,7 C14,6 17,3 19.5,3 C22,3 25,5 25,9 C25,17 14,25 14,25 Z'" \
   "$TMP/heart.png"
for i in 0 1 2 3 4 5; do
  yoff=$(( 30 - i*5 ))                 # rise
  scale=$(( 60 + i*8 ))                # pop (60%→100%+)
  op=$(awk "BEGIN{print (i<3)?1.0:(1.0-(($i-2)*0.28))}" i="$i")
  magick "$TMP/heart.png" -resize "${scale}%" -channel A -evaluate multiply "$op" +channel "$TMP/h_$i.png"
  magick "$TMP/base.png" "$TMP/h_$i.png" -gravity north -geometry "+0+${yoff}" -composite \
     -background black -alpha remove -alpha off "$TMP/frame_$i.png"
done
magick -delay 12 -loop 0 "$TMP/frame_0.png" "$TMP/frame_1.png" "$TMP/frame_2.png" \
   "$TMP/frame_3.png" "$TMP/frame_4.png" "$TMP/frame_5.png" -colors "$COLORS" +dither "$HERE/heart.gif"
gifsicle -O3 --colors "$COLORS" -o "$HERE/heart.gif" "$HERE/heart.gif"
printf "  ✅ %-12s  6 frames  %5sB  delay=12cs (composed)\n" "heart.gif" "$(wc -c <"$HERE/heart.gif"|tr -d ' ')"
rm -rf "$TMP"

echo "🐱 done. pack total: $(du -sh "$HERE" | awk '{print $1}')"
