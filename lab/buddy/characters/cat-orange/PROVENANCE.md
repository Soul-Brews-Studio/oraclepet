# cat-orange pack — provenance & license

A chunky **meme-cat** pet pack (sibling to `bufo`). Same `manifest.json` schema, same
96×100 frame format, one AnimatedGIF decoder.

## Source art

| Asset | Source | License | URL |
|---|---|---|---|
| Orange Fat Cat — Idle/Walk/Jump/Dead + expression poses (GRUMPY/HAPPY/HI CAT) | **Orange Fat Cat** (OpenGameArt) | **CC0 1.0** (no attribution required) | https://opengameart.org/content/orange-fat-cat |

License verified 2026-05-31 via the deep-research adversarial CC0 check. CC0 is
uploader-declared on OpenGameArt.

## State mapping (built by `build-orange-pack.sh`)

| state | ← source | note |
|---|---|---|
| `idle` | Idle (2) | slow breathe |
| `busy` | Walk (every-other → 10) | |
| `celebrate` | Jump (10) | a hop |
| `sleep` | Dead (every-other → 8) | X-eyes, lying |
| `attention` | **HI CAT** pose | 2-frame breathe bob |
| `dizzy` | **GRUMPY CAT** pose | woozy ±9° rotate wobble (composed) |
| `heart` | **HAPPY CAT** pose | + original rising/popping heart (composed) |

Single-pose "Extra" frames (HI/GRUMPY/HAPPY CAT) are animated here via bob/wobble/heart
compositing so every state moves. The heart sprite is an original drawing. Whole pack is
attribution-free (CC0 source + original derivative). Pack ≈ 132 KB.

## Reproduce
```bash
bash ../cat/fetch.sh 2>/dev/null || true   # (cat/fetch.sh also has the orange URL; or:)
# orange zip: https://opengameart.org/sites/default/files/Orange%20Fat%20Cat.zip → .tmp/cat-assets/raw/orange_fat/
bash build-orange-pack.sh
```
