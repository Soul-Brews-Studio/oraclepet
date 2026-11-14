"""Replay the pet's old and new voice gates (audio.c) over saved utterances, same input for both.

The WAVs are what the pet sent (already through its 200 Hz high-pass). The floor is fixed at the
value the pet logged for each utterance. Prints, per utterance, the seconds each gate would send.
usage: python3 gate-sim.py SINCE(HH:MM) [PET]
Keep gate() in step with voice_frame() in lab/jc3248-pet-idf/main/audio.c.
"""
import array, json, math, sys, wave
from pathlib import Path

ROOT = Path.home() / "OraclePet/voice-log"
VF, PRE = 320, 15

def frames_rms(path):
    with wave.open(str(path)) as w:
        a = array.array("h", w.readframes(w.getnframes()))
    return [math.sqrt(sum(x * x for x in a[i:i + VF]) / VF) for i in range(0, len(a) - VF + 1, VF)]

def gate(rms, floor, new):
    on_th = max(2 * floor, floor + (60 if new else 120))
    hold_th = max(1.5 * floor, floor + 30)
    hang, need = (60, 3) if new else (40, 4)
    on, kept, starts, hist, ring = False, 0, 0, [], 0
    for i, r in enumerate(rms):
        r1 = rms[i - 1] if i >= 1 else 0
        r2 = rms[i - 2] if i >= 2 else 0
        speechy = min(r, r1, r2) > on_th
        voiced = min(r, r1) > hold_th if new else speechy
        if not on:
            ring = min(ring + 1, PRE)
            if speechy:
                on, starts, hist = True, starts + 1, [1] * hang
                kept += ring
                ring = 0
            continue
        kept += 1
        hist = hist[1:] + [1 if voiced else 0]
        if sum(hist) < need:
            on = False
    return kept * VF / 16000, starts

day = __import__("datetime").date.today().isoformat()
since, pet = sys.argv[1], (sys.argv[2] if len(sys.argv) > 2 else "D350")
recs, upd = [], {}
for line in open(ROOT / "log.jsonl"):
    try:
        o = json.loads(line)
    except ValueError:
        continue
    if "update" in o:
        upd[o["update"]] = o
    elif o.get("pet", "").endswith(pet) and o.get("start", "") >= f"{day}T{since}":
        recs.append(o)
tot = {"len": 0, "old": 0, "new": 0}
for r in recs:
    u = upd.get(r["file"]) or {}
    if not u.get("speech_s"):
        continue                                       # Silero heard no speech: skip the noise
    rms = frames_rms(ROOT / r["file"])
    old, so = gate(rms, r["floor"], False)
    new, sn = gate(rms, r["floor"], True)
    tot["len"] += r["seconds"]; tot["old"] += old; tot["new"] += new
    print(f'{r["start"][11:19]} {r["seconds"]:5.2f}s rms {r["rms"]:3}  old gate {old:5.2f}s ({so} starts)  '
          f'new gate {new:5.2f}s ({sn})  | {(u.get("text") or "")[:40]}')
print(f'speech utterances: {tot["len"]:.1f} s recorded; old gate would send {tot["old"]:.1f} s, new gate {tot["new"]:.1f} s')
