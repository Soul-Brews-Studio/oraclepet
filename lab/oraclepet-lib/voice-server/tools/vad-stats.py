"""Utterance stats from ~/OraclePet/voice-log/log.jsonl for one pet in a time window.

usage: python3 vad-stats.py SINCE [UNTIL] [PET]   (times as HH:MM, today, +07:00)
Coverage = seconds sent / wall time from the first start to the last end. Gaps = the time
between one utterance's end and the next one's start (audio the pet did not send).
"""
import datetime as dt
import json
import statistics as st
import sys
from collections import Counter
from pathlib import Path

day = dt.date.today().isoformat()
since = sys.argv[1]
until = sys.argv[2] if len(sys.argv) > 2 and sys.argv[2] != "-" else "23:59"
pet = sys.argv[3] if len(sys.argv) > 3 else "D350"
recs, upd = [], {}
for line in open(Path.home() / "OraclePet/voice-log/log.jsonl"):
    try:
        o = json.loads(line)
    except ValueError:
        continue
    if "update" in o:
        upd[o["update"]] = o
    elif o.get("pet", "").endswith(pet) and o.get("start", "").startswith(day) and since <= o["start"][11:16] <= until:
        recs.append(o)
if len(recs) < 2:
    sys.exit(f"{len(recs)} utterances from {pet} between {since} and {until}")
t = dt.datetime.fromisoformat
span = (t(recs[-1]["end"]) - t(recs[0]["start"])).total_seconds()
sent = sum(r["seconds"] for r in recs)
gaps = [(t(b["start"]) - t(a["end"])).total_seconds() for a, b in zip(recs, recs[1:])]
texts = [(upd.get(r["file"]) or {}).get("text") for r in recs]
print(f"{pet} {recs[0]['start'][11:19]}-{recs[-1]['end'][11:19]}: {len(recs)} utterances, "
      f"sent {sent:.0f} of {span:.0f} s = {100 * sent / span:.0f}%")
print(f"length median {st.median(r['seconds'] for r in recs):.2f} s, max {max(r['seconds'] for r in recs):.1f} s; "
      f"reasons {dict(Counter(r['reason'] for r in recs))}")
print(f"gaps median {st.median(gaps):.2f} s: <1 s {sum(g < 1 for g in gaps)}, 1-3 s {sum(1 <= g < 3 for g in gaps)}, "
      f">=3 s {sum(g >= 3 for g in gaps)}")
print(f"floor median {st.median(r['floor'] for r in recs):.0f}, rms median {st.median(r['rms'] for r in recs):.0f}; "
      f"with text {sum(1 for x in texts if x)} of {len(recs)}, characters {sum(len(x) for x in texts if x)}")
