#!/usr/bin/env python3
"""Write firmware/index.json for the React site from dist/ and the partition tables.

usage: catalog.py <dist> <site> <commit>
Offsets come from the boards' partitions.csv, so the page never hard-codes them.
"""
import csv
import json
import shutil
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
dist, site, commit = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3]

BOARDS = [
    dict(id="jc3248", name="Guition JC3248W535", screen="3.5″ 320×480 touch", project="jc3248_pet_idf",
         note="ESP32-S3 with an AXS15231B QSPI screen. The board the pet was built on.",
         table="lab/jc3248-pet-idf/partitions.csv", app="ota_0"),
    dict(id="sc01", name="WT32-SC01 Plus", screen="3.5″ 480×320 touch", project="oraclepet_sc01",
         note="ESP32-S3 with quad PSRAM.", table="lab/jc3248-pet-idf/partitions.csv", app="ota_0"),
    dict(id="lcd28", name="ESP32-S3 2.8″", screen="2.8″ 240×320, speaker + mic", project="oraclepet_lcd28",
         note="The 2.8″ ILI9341 board with the ES8311 audio codec.", table="lab/jc3248-pet-idf/partitions.csv", app="ota_0"),
    dict(id="base", name="OTA base", screen="any ESP32-S3, 8 MB flash", project="oraclepet_base",
         note="", table="lab/ota-base/partitions.csv", app="factory", base=True),
]


def offsets(table: Path) -> dict:
    out = {}
    for row in csv.reader(l for l in table.read_text().splitlines() if l.strip() and not l.lstrip().startswith("#")):
        row = [c.strip() for c in row]
        out[row[0]] = int(row[3], 0)
    return out


def part(board_dir: Path, file: str, offset: int) -> dict:
    return {"file": file, "offset": offset, "bytes": (board_dir / file).stat().st_size}


boards = []
for b in BOARDS:
    d = dist / b["id"]
    if not d.is_dir():
        continue
    po = offsets(ROOT / b["table"])
    common = [
        part(d, "bootloader.bin", 0x0),
        part(d, "partition-table.bin", 0x8000),
        part(d, "ota_data_initial.bin", po["otadata"]),
        part(d, "storage.bin", po["storage"]),
    ]
    apps = {f.stem.removeprefix("app-"): part(d, f.name, po[b["app"]]) for f in sorted(d.glob("app-*.bin"))}
    packs = sorted(apps)
    default = "bufo" if "bufo" in apps else ("siamese" if "siamese" in apps else packs[0])
    boards.append(dict(
        id=b["id"], name=b["name"], screen=b["screen"], note=b["note"], project=b["project"],
        packs=packs, defaultPack=default, common=common,
        app={"offset": po[b["app"]], "files": apps}, base=b.get("base", False),
    ))

# Character images: each pack's idle GIF, from its manifest.
packs = []
(site / "packs").mkdir(parents=True, exist_ok=True)
for m in sorted((ROOT / "lab/buddy/characters").glob("*/manifest.json")):
    pid = m.parent.name
    data = json.loads(m.read_text())
    idle = data.get("states", {}).get("idle")
    idle = idle if isinstance(idle, str) else (idle or [None])[0]
    if not idle or not (m.parent / idle).exists():
        continue
    shutil.copy(m.parent / idle, site / "packs" / f"{pid}.gif")
    # every state GIF under one name, for the preview: packs/<id>/<state>.gif
    (site / "packs" / pid).mkdir(exist_ok=True)
    for state, f in data.get("states", {}).items():
        f = f if isinstance(f, str) else (f or [None])[0]
        if f and (m.parent / f).exists():
            shutil.copy(m.parent / f, site / "packs" / pid / f"{state}.gif")
    lic = "see PROVENANCE.md" if (m.parent / "PROVENANCE.md").exists() else "see README.md"
    # the pack as the pet takes it over BLE (manifest.json + GIFs), for the Bluetooth tool
    files = sorted(f.name for f in m.parent.iterdir() if f.suffix in (".gif", ".json"))
    (site / "characters" / pid).mkdir(parents=True, exist_ok=True)
    for f in files:
        shutil.copy(m.parent / f, site / "characters" / pid / f)
    packs.append({"id": pid, "name": data.get("name", pid), "image": f"packs/{pid}.gif", "license": lic,
                  "files": files, "bytes": sum((m.parent / f).stat().st_size for f in files)})

# bufo first: it is the default and the site icon.
packs.sort(key=lambda p: (p["id"] != "bufo", p["id"]))
# Example pet apps (lab/pet-sdk/examples), built by build-site.sh into apps/<name>.wasm.
apps = sorted(f.stem for f in (site / "apps").glob("*.wasm")) if (site / "apps").is_dir() else []
cat = {"apps": apps, "commit": commit, "built": time.strftime("%Y-%m-%d", time.gmtime()), "boards": boards, "packs": packs}
(site / "firmware").mkdir(parents=True, exist_ok=True)
(site / "firmware" / "index.json").write_text(json.dumps(cat, indent=1))
print(f"index.json: {len(boards)} boards, {len(packs)} characters, {len(apps)} apps")
