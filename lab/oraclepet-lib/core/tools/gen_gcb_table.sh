#!/usr/bin/env bash
# Rewrite gcb_table in oraclepet.c from Swift's own grapheme breaking (tools/gcb_table.swift
# asks String, code point by code point, which characters join). Run it when the Swift the Mac
# app ships with moves to a new Unicode version. Needs swiftc (macOS); make test does not.
set -euo pipefail
cd "$(dirname "$0")/.."
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
cp tools/gcb_table.swift "$T/main.swift"
swiftc -O "$T/main.swift" -o "$T/gen"
"$T/gen" > "$T/table.inc"
python3 - "$T/table.inc" <<'PY'
import re, sys
table = open(sys.argv[1]).read().rstrip("\n")
src = open("oraclepet.c").read()
begin = src.index("/* BEGIN gcb_table")
begin = src.index("\n", begin) + 1
end = src.index("/* END gcb_table */")
open("oraclepet.c", "w").write(src[:begin] + table + "\n" + src[end:])
PY
echo "oraclepet.c: gcb_table rewritten ($(grep -c . "$T/table.inc") lines)"
