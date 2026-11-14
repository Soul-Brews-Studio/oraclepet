#!/usr/bin/env bash
# Build the "cat-pet" GIF pack from CC0 LuizMelo "Pet Cats Pack" (itch.io, CC0).
# These are 50x50 horizontal sprite STRIPS (pixel art) → slice → union-trim →
# NEAREST-neighbor upscale (crisp pixels) → 96x100 on black → palette → gifsicle.
# Arg 1 = which cat (Cat-1..Cat-6). Default Cat-5 (fluffy white — pops on black bg).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../../.." && pwd)"
PACK="$ROOT/.tmp/cat-assets/raw/pet_cats/Pet Cats Pack"
CAT="${1:-Cat-5}"
SRC="$PACK/$CAT"
W=96; H=100; COLORS=128
[ -d "$SRC" ] || { echo "❌ $SRC missing (run the itch fetch / unzip first)"; exit 1; }
echo "🐱 building cat-pet from $CAT → $HERE"

# slice a horizontal 50px strip into frames, union-trim, NEAREST upscale to 96x100
slicegif() {
  local name="$1" strip="$2" delay="$3"
  [ -f "$strip" ] || { echo "  ⚠️  $name: missing $(basename "$strip")"; return 0; }
  local T; T="$(mktemp -d)"
  magick "$strip" -crop 50x50 +repage "$T/f_%03d.png"
  local frames=( "$T"/f_*.png )
  local box; box="$(magick "${frames[@]}" -background none -flatten -trim -format '%wx%h%O' info: 2>/dev/null)"
  [ -n "$box" ] || box="50x50+0+0"
  magick "${frames[@]}" -crop "$box" +repage \
      -background black -alpha remove -alpha off \
      -filter point -resize "${W}x${H}" -background black -gravity center -extent "${W}x${H}" \
      -set delay "$delay" -loop 0 -colors "$COLORS" +dither "$HERE/$name.gif"
  gifsicle -O3 --colors "$COLORS" -o "$HERE/$name.gif" "$HERE/$name.gif" 2>/dev/null
  printf "  ✅ %-12s %2d frames  %5sB\n" "$name.gif" "${#frames[@]}" "$(wc -c <"$HERE/$name.gif"|tr -d ' ')"
}

slicegif idle      "$SRC/$CAT-Idle.png"       12
slicegif busy      "$SRC/$CAT-Run.png"         7
slicegif attention "$SRC/$CAT-Meow.png"       12
slicegif celebrate "$SRC/$CAT-Stretching.png"  8
slicegif sleep     "$SRC/$CAT-Laying.png"     16

# base = first idle frame, NEAREST-upscaled to 96x100 on black (for composed states)
T="$(mktemp -d)"
magick "$SRC/$CAT-Idle.png" -crop 50x50 +repage "$T/i_%03d.png"
i0=$(ls "$T"/i_*.png | head -1)
ibox="$(magick "$i0" -background none -trim -format '%wx%h%O' info:)"
magick "$i0" -crop "$ibox" +repage -background black -alpha remove -alpha off \
   -filter point -resize "${W}x${H}" -background black -gravity center -extent "${W}x${H}" "$T/base.png"

# dizzy ← idle[0] woozy wobble (rotate sweep)
echo "  · composing dizzy.gif (wobble)"
gi=0; gargs=()
for ang in -9 -5 0 5 9 5 0 -5; do
  magick "$T/base.png" -background black -distort SRT "$ang" -background black -alpha remove "$T/dz_$gi.png"
  gargs+=( "$T/dz_$gi.png" ); gi=$((gi+1))
done
magick -delay 8 -loop 0 "${gargs[@]}" -colors $COLORS +dither "$HERE/dizzy.gif"
gifsicle -O3 -o "$HERE/dizzy.gif" "$HERE/dizzy.gif" 2>/dev/null
printf "  ✅ %-12s  8 frames  %5sB\n" "dizzy.gif" "$(wc -c <"$HERE/dizzy.gif"|tr -d ' ')"

# heart ← idle[0] + rising/popping heart
echo "  · composing heart.gif"
magick -size 28x28 xc:none -fill '#FF4D6D' -stroke '#FFFFFF' -strokewidth 1 \
   -draw "path 'M14,25 C14,25 3,17 3,9 C3,5 6,3 8.5,3 C11,3 14,6 14,7 C14,6 17,3 19.5,3 C22,3 25,5 25,9 C25,17 14,25 14,25 Z'" "$T/heart.png"
hargs=()
for i in 0 1 2 3 4 5; do
  yoff=$(( 24 - i*4 )); scale=$(( 62 + i*8 ))
  op=$(awk "BEGIN{print (i<3)?1.0:(1.0-((i-2)*0.28))}" i=$i)
  magick "$T/heart.png" -resize "${scale}%" -channel A -evaluate multiply "$op" +channel "$T/hh_$i.png"
  magick "$T/base.png" "$T/hh_$i.png" -gravity north -geometry "+0+${yoff}" -composite -background black -alpha remove "$T/hf_$i.png"
  hargs+=( "$T/hf_$i.png" )
done
magick -delay 12 -loop 0 "${hargs[@]}" -colors $COLORS +dither "$HERE/heart.gif"
gifsicle -O3 -o "$HERE/heart.gif" "$HERE/heart.gif" 2>/dev/null
printf "  ✅ %-12s  6 frames  %5sB\n" "heart.gif" "$(wc -c <"$HERE/heart.gif"|tr -d ' ')"

echo "🐱 done ($CAT). pack total: $(du -sh "$HERE" | awk '{print $1}')"
