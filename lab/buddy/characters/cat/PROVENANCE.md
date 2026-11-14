# cat pack — provenance & license

A pet GIF pack (sibling to `bufo`) built from **CC0** source art. Same `manifest.json`
schema as bufo; same 96×100 frame format; one AnimatedGIF decoder serves it on the
device (LittleFS), in the browser (emcc), and anywhere else the core runs.

## Source art

| Asset | Source | License | URL |
|---|---|---|---|
| Cat frames (Idle/Run/Jump/Hurt/Dead) | **Tiny Kitten Game Sprite** (OpenGameArt) | **CC0 1.0** (public domain dedication — no attribution required) | https://opengameart.org/content/tiny-kitten-game-sprite |

License verified 2026-05-31 via a 98-agent deep-research pass with adversarial
"is-this-really-CC0?" verification (3 false-CC0 candidates were rejected: Pixabay,
Pexels, OpenClipart "Angelina"). CC0 is uploader-declared on OpenGameArt.

## What was built here (derivative, also CC0-equivalent)

Each pet state was produced from the source frames by `build-cat-pack.sh`:
union-bbox trim across the animation (zoom to the cat, no jitter) → composite onto
black → fit 96×100 centred → palette-reduce (≤128) → `gifsicle -O3`.

| state | ← source animation | frames |
|---|---|---|
| `idle` | 01_Idle | 12 |
| `busy` | 02_Run | 10 |
| `attention` | 03_Jump/01_Up | 5 |
| `celebrate` | 03_Jump (Up+Fall) | 10 |
| `dizzy` | 04_Hurt | 6 |
| `sleep` | 05_Dead (lying/curled) | 8 |
| `heart` | **original composition** — a vector heart (drawn in `build-cat-pack.sh`) popping & rising over a static idle frame | 6 |

The `heart` sprite is an original drawing (no third-party source). The whole pack is
free of attribution obligations (CC0 source + original derivative work).

## Reproduce

```bash
bash fetch.sh             # downloads the CC0 zip into .tmp/cat-assets/raw (gitignored)
bash build-cat-pack.sh    # rebuilds all 7 *.gif from the raw frames
```

Pack total ≈ 84 KB (vs bufo's ~596 KB) — comfortable on the pet's 3 MB LittleFS.
