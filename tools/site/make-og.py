#!/usr/bin/env python3
"""Render site/public/og.png (1200x630), the link preview, from the character packs' idle GIFs.

usage: python3 tools/site/make-og.py [font.ttf] [bold-font.ttf]
Needs Pillow. The PNG is committed, so CI does not run this.
"""
import json
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[2]
CHARS = ROOT / "lab/buddy/characters"
OUT = ROOT / "site/public/og.png"
FONT = sys.argv[1] if len(sys.argv) > 1 else "/System/Library/Fonts/SFNS.ttf"
BOLD = sys.argv[2] if len(sys.argv) > 2 else "/System/Library/Fonts/Supplemental/Arial Bold.ttf"

BG, INK, DIM, AMBER, TILE, LINE = "#12100c", "#f4eee4", "#a89a84", "#f0a830", "#000000", "#3a3226"
W, H = 1200, 630


def idle_frame(pack: Path) -> Image.Image | None:
    m = json.loads((pack / "manifest.json").read_text())
    idle = m.get("states", {}).get("idle")
    idle = idle if isinstance(idle, str) else (idle or [None])[0]
    if not idle or not (pack / idle).exists():
        return None
    g = Image.open(pack / idle)
    g.seek(0)
    return g.convert("RGBA")


img = Image.new("RGB", (W, H), BG)
d = ImageDraw.Draw(img)

# right: a 3x3 board of characters, nearest-neighbour scaled like the pet does
order = ["siamese", "bufo", "volt", "cat-orange", "clawd", "iris", "pulse", "neo", "atlas"]
frames = [f for f in (idle_frame(CHARS / p) for p in order if (CHARS / p / "manifest.json").exists()) if f][:9]
tile, gap = 150, 12
gx, gy = W - 3 * tile - 2 * gap - 60, (H - 3 * tile - 2 * gap) // 2
for i, f in enumerate(frames):
    x, y = gx + (i % 3) * (tile + gap), gy + (i // 3) * (tile + gap)
    d.rounded_rectangle((x, y, x + tile, y + tile), radius=14, fill=TILE, outline=LINE, width=2)
    s = min((tile - 16) / f.width, (tile - 16) / f.height)
    f = f.resize((max(1, int(f.width * s)), max(1, int(f.height * s))), Image.NEAREST)
    img.paste(f, (x + (tile - f.width) // 2, y + (tile - f.height) // 2), f)

# left: what it is
f_kicker = ImageFont.truetype(FONT, 26)
f_title = ImageFont.truetype(BOLD, 84)
f_sub = ImageFont.truetype(FONT, 34)
f_small = ImageFont.truetype(FONT, 24)
x0 = 64
d.text((x0, 120), "Open source · ESP32-S3 · MIT", font=f_kicker, fill=AMBER)
d.text((x0, 165), "OraclePet", font=f_title, fill=INK)
y = 280
for line in ["A desk pet you flash", "from the browser."]:
    d.text((x0, y), line, font=f_sub, fill=INK)
    y += 46
y += 22
for line in ["USB install with esptool-js", "Firmware, characters and apps", "over Bluetooth"]:
    d.text((x0, y), line, font=f_small, fill=DIM)
    y += 34
d.text((x0, H - 70), "soul-brews-studio.github.io/oraclepet", font=f_small, fill=AMBER)

OUT.parent.mkdir(parents=True, exist_ok=True)
img.save(OUT, optimize=True)
print(OUT, img.size, OUT.stat().st_size, "bytes")
