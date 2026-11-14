"""Heartbeat clipping by grapheme cluster, against Swift on scripts the golden file leaves out.

Swift cuts msg (63 bytes) and who (20 characters) on Character boundaries, so the bytes match
only when Python breaks text exactly as Swift does. tests/grapheme_cases.json holds lines
Swift itself wrote (tests/swift/gen_grapheme_cases.sh); oraclepet/_gcb.py must stay the same
table as core/oraclepet.c's gcb_table, which both come from (tools/gen_gcb.py). Offline.
"""
from __future__ import annotations

import json
import re
import unittest
from pathlib import Path

from oraclepet import _gcb
from oraclepet import protocol as P

HERE = Path(__file__).resolve().parent
CASES = json.loads((HERE / "grapheme_cases.json").read_text(encoding="utf-8"))["cases"]


class Graphemes(unittest.TestCase):
    def test_swift_lines(self):
        for c in CASES:
            with self.subTest(tag=c["tag"], msg=c["msg"]):
                hb = P.Heartbeat(3, 1, 2, msg=c["msg"], epoch=1792547421, tzoff=25200, who=c["who"])
                self.assertEqual(hb.json_line().decode("utf-8"), c["line"])

    def test_rules(self):
        self.assertEqual(P.graphemes("؀a"), ["؀a"])                  # prepend joins forward
        self.assertEqual(len(P.graphemes("क्ष")), 1)           # Devanagari conjunct
        self.assertEqual(P.graphemes("a‍b"), ["a‍", "b"])           # ZWJ joins pictographs only
        self.assertEqual(len(P.graphemes("❤️‍\U0001F525")), 1)
        self.assertEqual(len(P.graphemes("\U0001F1F9\U0001F1ED\U0001F1EF")), 2)
        self.assertEqual(P.graphemes("\r\n\n"), ["\r\n", "\n"])

    def test_table_matches_core(self):
        core = HERE.parent.parent / "core" / "oraclepet.c"
        if not core.exists():
            self.skipTest("core/ not next to python/")
        body = re.search(r"gcb_table\[\d+\] = \{(.*?)\};", core.read_text(), re.DOTALL).group(1)
        entries = [int(x, 16) for x in re.findall(r"0x([0-9a-f]+)u", body)]
        self.assertEqual(list(_gcb.STARTS), [e >> 4 for e in entries], "rerun tools/gen_gcb.py")
        self.assertEqual(list(_gcb.CLASSES), [e & 15 for e in entries], "rerun tools/gen_gcb.py")


if __name__ == "__main__":
    unittest.main()
