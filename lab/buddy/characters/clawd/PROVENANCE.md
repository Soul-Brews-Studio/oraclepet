# clawd pack — provenance & license

The **official Claude mascot** ("clawd") as a pet pack — fitting for a Claude oracle's
screen. Same `manifest.json` schema as bufo/cat (the manifest here is **verbatim from
upstream**), 96×100 frames, one AnimatedGIF decoder.

## Source art & license

| Asset | Source | License |
|---|---|---|
| 8 clawd animations (120×120 GIFs) | **anthropics/claude-desktop-buddy** → its Waveshare-ESP32-S3 fork `eduardo-moro/claude-desktop-buddy-waveshare-esp32s3-touch` @ `3f2ce77` (`characters/clawd/`) | **MIT** |

> Copyright 2026 Anthropic, PBC. Permission is hereby granted, free of charge, to any
> person obtaining a copy of this software … to deal in the Software without restriction …
> The above copyright notice and this permission notice shall be included in all copies …

MIT requires the notice be preserved — hence this file. `claude-desktop-buddy` is an
official Anthropic project; the clawd mascot ships in it under MIT, so it is free to use,
modify, and redistribute (including this downscaled derivative). License verified by
reading the repo `LICENSE` 2026-05-31.

## What was built (`build-clawd-pack.sh`)

The upstream GIFs are 120×120, 31–60 frames. Each was coalesced → subsampled (~15–18
frames) → fit 96×100 on the manifest bg `#282828` → palette → `gifsicle -O3`. Upstream
filenames + manifest kept verbatim, so this is a faithful downscaled mirror of the
official clawd pack.

| state | gif | note |
|---|---|---|
| `sleep` | clawd-sleeping | |
| `idle` | clawd-idle, clawd-headphones-groove, clawd-idle-reading | 3-gif rotation (upstream) |
| `busy` | clawd-typing | |
| `attention` | clawd-react-double-jump | |
| `celebrate` / `heart` | clawd-happy | heart reuses celebrate (upstream) |
| `dizzy` | clawd-thinking | |

Pack ≈ 128 KB. Reproduce: re-fetch the 8 gifs from the commit above into
`.tmp/cat-assets/raw/clawd/`, then `bash build-clawd-pack.sh`.
