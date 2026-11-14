# cat-pet pack — provenance & license

A fluffy pixel-art cat pet pack (sibling to `bufo`). Same `manifest.json` schema, 96×100
frames, one AnimatedGIF decoder.

## Source art

| Asset | Source | License | URL |
|---|---|---|---|
| Pet Cats Pack — 50×50 sprite strips (Idle/Run/Meow/Stretching/Laying), **6 colour variants** | **Pet Cats Pack** by LuizMelo (itch.io) | **CC0 1.0** ("Creative Commons Zero v1.0 Universal" on the asset page — no attribution required) | https://luizmelo.itch.io/pet-cat-pack |

License verified 2026-05-31 via the deep-research adversarial CC0 check (license field
reads "Creative Commons Zero v1.0 Universal"). Downloaded the free `Pet Cats Pack.zip`
(72 kB) and sliced the strips.

## Build (`build-pet-pack.sh [Cat-N]`)

Horizontal 50×50 strips → slice into frames → union-trim (zoom to the cat) →
**nearest-neighbor** upscale (crisp pixels) → 96×100 on black → palette → `gifsicle -O3`.

| state | ← source strip | frames |
|---|---|---|
| `idle` | Idle | 10 |
| `busy` | Run | 8 |
| `attention` | Meow | 4 |
| `celebrate` | Stretching | 13 |
| `sleep` | Laying | 8 |
| `dizzy` | idle[0] woozy ±9° wobble (composed) | 8 |
| `heart` | idle[0] + original rising heart (composed) | 6 |

**Colour variants:** the pack ships 6 cats (`Cat-1`…`Cat-6`). This pack defaults to
**Cat-5** (fluffy white/cream — high contrast on the pet's black bg). Rebuild any colour:
`bash build-pet-pack.sh Cat-3`  (Cat-2 is a black cat — avoid; it vanishes on black).

Pack ≈ 52 KB. Attribution-free (CC0 source + original composed states).
