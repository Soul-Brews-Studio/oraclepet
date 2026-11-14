#!/usr/bin/env bash
# Build the "cat-orange" GIF pack (sibling to bufo) from CC0 Orange Fat Cat art.
# Source: https://opengameart.org/content/orange-fat-cat  (CC0)
# A chunky meme cat: real Idle/Walk/Jump/Dead animations + expression poses
# (GRUMPY/HAPPY/HI) used for the single-pose states. 96x100, palette, gifsicle -O3.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../../.." && pwd)"
RAW="$ROOT/.tmp/cat-assets/raw/orange_fat/Orange Fat Cat"
W=96; H=100; COLORS=128
FONT=""; for f in /System/Library/Fonts/Supplemental/Arial.ttf /System/Library/Fonts/Helvetica.ttc; do [ -f "$f" ] && FONT="$f" && break; done
[ -d "$RAW" ] || { echo "❌ raw frames missing: $RAW (run fetch first)"; exit 1; }

# union-bbox trim across frames -> black -> fit 96x100 -> palette -> optimize
mkgif() {
  local name="$1" delay="$2"; shift 2; local frames=("$@")
  [ "${#frames[@]}" -gt 0 ] || { echo "  ⚠️  $name: no frames"; return 1; }
  local box; box="$(magick "${frames[@]}" -background none -layers merge -trim -format '%@' info: 2>/dev/null)"
  [ -n "$box" ] || box="${W}x${H}+0+0"
  magick "${frames[@]}" -crop "$box" +repage \
      -background black -alpha remove -alpha off \
      -filter Lanczos -resize "${W}x${H}" -background black -gravity center -extent "${W}x${H}" \
      -set delay "$delay" -loop 0 -colors "$COLORS" +dither "$HERE/$name.gif"
  gifsicle -O3 --colors "$COLORS" -o "$HERE/$name.gif" "$HERE/$name.gif"
  printf "  ✅ %-12s %2d frames  %5sB\n" "$name.gif" "${#frames[@]}" "$(wc -c <"$HERE/$name.gif"|tr -d ' ')"
}
glob()  { local IFS=$'\n'; printf '%s\n' "$RAW/$1"/*.png | sort -V; }
sub()   { local n="$1"; shift; local i=0; for f in "$@"; do [ $((i % n)) -eq 0 ] && printf '%s\n' "$f"; i=$((i+1)); done; }   # every nth

# proc a single PNG to a 96x100-on-black base
base96() { magick "$1" -background none -trim +repage -background black -alpha remove -alpha off \
  -filter Lanczos -resize "${W}x${H}" -background black -gravity center -extent "${W}x${H}" "$2"; }

echo "🐱 building cat-orange → $HERE"
mapfile -t IDLE < <(glob "Idle")
mapfile -t WALK < <(glob "Walk")
mapfile -t JUMP < <(glob "Jump")
mapfile -t DEAD < <(glob "Dead")
mapfile -t WALK10 < <(sub 2 "${WALK[@]}")
mapfile -t DEAD8  < <(sub 2 "${DEAD[@]}")

mkgif idle      28 "${IDLE[@]}"
mkgif busy       6 "${WALK10[@]}"
mkgif celebrate  6 "${JUMP[@]}"
mkgif sleep     18 "${DEAD8[@]}"

# attention ← HI CAT (gentle breathe: full + 94% scaled)
echo "  · composing attention.gif (HI CAT bob)"
T=$(mktemp -d); base96 "$RAW/Extra/HI CAT.png" "$T/hi.png"
magick "$T/hi.png" \( "$T/hi.png" -resize 94% -background black -gravity center -extent ${W}x${H} \) \
   -set delay 14 -loop 0 -colors $COLORS +dither "$HERE/attention.gif"
gifsicle -O3 -o "$HERE/attention.gif" "$HERE/attention.gif"
printf "  ✅ %-12s  2 frames  %5sB (HI CAT)\n" "attention.gif" "$(wc -c <"$HERE/attention.gif"|tr -d ' ')"

# dizzy ← GRUMPY CAT woozy wobble (rotate sweep)
echo "  · composing dizzy.gif (GRUMPY wobble)"
base96 "$RAW/Extra/GRUMPYCAT.png" "$T/grumpy.png"
gi=0; gargs=()
for ang in -9 -5 0 5 9 5 0 -5; do
  magick "$T/grumpy.png" -background black -distort SRT "$ang" -background black -alpha remove "$T/dz_$gi.png"
  gargs+=( "$T/dz_$gi.png" ); gi=$((gi+1))
done
magick -delay 8 -loop 0 "${gargs[@]}" -colors $COLORS +dither "$HERE/dizzy.gif"
gifsicle -O3 -o "$HERE/dizzy.gif" "$HERE/dizzy.gif"
printf "  ✅ %-12s  8 frames  %5sB (GRUMPY)\n" "dizzy.gif" "$(wc -c <"$HERE/dizzy.gif"|tr -d ' ')"

# heart ← HAPPY CAT + rising/popping heart
echo "  · composing heart.gif (HAPPY CAT + heart)"
base96 "$RAW/Extra/HAPPY CAT.png" "$T/happy.png"
magick -size 28x28 xc:none -fill '#FF4D6D' -stroke '#FFFFFF' -strokewidth 1 \
   -draw "path 'M14,25 C14,25 3,17 3,9 C3,5 6,3 8.5,3 C11,3 14,6 14,7 C14,6 17,3 19.5,3 C22,3 25,5 25,9 C25,17 14,25 14,25 Z'" "$T/heart.png"
hargs=()
for i in 0 1 2 3 4 5; do
  yoff=$(( 28 - i*4 )); scale=$(( 62 + i*8 ))
  op=$(awk "BEGIN{print (i<3)?1.0:(1.0-((i-2)*0.28))}" i=$i)
  magick "$T/heart.png" -resize "${scale}%" -channel A -evaluate multiply "$op" +channel "$T/hh_$i.png"
  magick "$T/happy.png" "$T/hh_$i.png" -gravity north -geometry "+0+${yoff}" -composite -background black -alpha remove "$T/hf_$i.png"
  hargs+=( "$T/hf_$i.png" )
done
magick -delay 12 -loop 0 "${hargs[@]}" -colors $COLORS +dither "$HERE/heart.gif"
gifsicle -O3 -o "$HERE/heart.gif" "$HERE/heart.gif"
printf "  ✅ %-12s  6 frames  %5sB (HAPPY)\n" "heart.gif" "$(wc -c <"$HERE/heart.gif"|tr -d ' ')"

echo "🐱 done. pack total: $(du -sh "$HERE" | awk '{print $1}')"
